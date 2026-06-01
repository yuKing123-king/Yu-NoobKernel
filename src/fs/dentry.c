#include <fs/dentry.h>
#include <fs/inode.h>
#include <fs/super.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <misc/string.h>
#include <misc/hashtable.h>
#include <mm/slab.h>
#include <mm/kalloc.h>
#include <sync/spinlock.h>

static struct kmem_cache dentry_cache;

static struct {
	struct hashtable ht;
	struct list_head dentry_list;
	struct list_head dentry_lru;
	spinlock_t lock;
	u32 count;
} dentry_state;

/*
 * 计算dentry在哈希表中的键值
 * @param sb: 超级块指针
 * @param parent: 父dentry指针
 * @param name: 文件名qstr结构
 * @return: 哈希桶索引
 */
static u32 dentry_hash_key(struct super_block *sb, struct dentry *parent,
			   struct qstr *name)
{
	u32 hash = (u32)((uintptr_t)sb ^ (uintptr_t)parent);
	hash = hash ^ name->hash;
	return hash_ptr((void *)(uintptr_t)hash, dentry_state.ht.size);
}

/*
 * 初始化dentry子系统（创建slab缓存和哈希表）
 */
void dentry_init(void)
{
	if (kmem_cache_init(&dentry_cache, "dentry", sizeof(struct dentry),
			    false) != 0) {
		panic("dentry_init: failed to create cache");
	}

	if (hashtable_init(&dentry_state.ht, 128) != 0) {
		panic("dentry_init: failed to init hashtable");
	}

	INIT_LIST_HEAD(&dentry_state.dentry_list);
	INIT_LIST_HEAD(&dentry_state.dentry_lru);
	dentry_state.lock = SPINLOCK_INITIALIZER("dentry");
	dentry_state.count = 0;

	infof("dentry initialized");
}

/*
 * 分配并初始化一个新的dentry
 * @param parent: 父dentry指针（可为NULL）
 * @param sb: 超级块指针
 * @param name: 文件名
 * @return: 成功返回dentry指针，失败返回错误指针
 */
struct dentry *dentry_alloc(struct dentry *parent, struct super_block *sb,
			    const char *name)
{
	struct dentry *dentry = kmem_cache_alloc(&dentry_cache);
	if (!dentry) {
		return PTR(-ENOMEM);
	}

	char *name_copy = kmalloc(strlen(name) + 1);
	if (!name_copy) {
		kmem_cache_free(dentry);
		return PTR(-ENOMEM);
	}
	strcpy(name_copy, name);

	dentry->d_name.name = name_copy;
	dentry->d_name.len = strlen(name);
	dentry->d_name.hash = hash_string(name, dentry->d_name.len);

	dentry->d_inode = NULL;
	dentry->d_sb = sb;
	dentry->d_parent = parent ? parent : dentry;
	dentry->d_op = NULL;
	dentry->d_fsdata = NULL;

	INIT_LIST_HEAD(&dentry->d_children);
	INIT_LIST_HEAD(&dentry->d_sibling);
	INIT_LIST_HEAD(&dentry->d_lru);
	INIT_LIST_HEAD(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_list);
	dentry->d_lock = SPINLOCK_INITIALIZER("dentry");
	dentry->d_refcnt = 1;
	dentry->d_flags = 0;

	if (parent) {
		spinlock_acquire(&parent->d_lock);
		list_add(&dentry->d_sibling, &parent->d_children);
		spinlock_release(&parent->d_lock);
	}

	return dentry;
}

/*
 * 释放dentry占用的所有资源
 * @param dentry: 待释放的dentry指针
 */
void dentry_free(struct dentry *dentry)
{
	if (!dentry) {
		return;
	}

	if (dentry->d_op && dentry->d_op->d_release) {
		dentry->d_op->d_release(dentry);
	}

	spinlock_acquire(&dentry_state.lock);
	hashtable_delete(&dentry_state.ht,
			 (void *)(uintptr_t)dentry_hash_key(
			     dentry->d_sb, dentry->d_parent, &dentry->d_name));
	list_del(&dentry->d_list);
	list_del(&dentry->d_lru);
	dentry_state.count--;
	spinlock_release(&dentry_state.lock);

	if (dentry->d_parent && dentry->d_parent != dentry) {
		spinlock_acquire(&dentry->d_parent->d_lock);
		list_del(&dentry->d_sibling);
		spinlock_release(&dentry->d_parent->d_lock);
	}

	if (dentry->d_inode) {
		inode_put(dentry->d_inode);
	}

	if (dentry->d_name.name) {
		kfree(dentry->d_name.name);
	}

	kmem_cache_free(dentry);
}

/*
 * 在dentry哈希表中查找指定的dentry
 * @param sb: 超级块指针
 * @param parent: 父dentry指针
 * @param name: 待查找的文件名
 * @return: 成功返回dentry指针，未找到返回NULL
 */
struct dentry *dentry_lookup(struct super_block *sb, struct dentry *parent,
			     struct qstr *name)
{
	spinlock_acquire(&dentry_state.lock);

	u32 bucket_idx = dentry_hash_key(sb, parent, name);

	struct hash_node *node;
	list_for_each_entry(node, &dentry_state.ht.buckets[bucket_idx], list)
	{
		struct dentry *dentry = (struct dentry *)node->value;
		if (dentry && dentry->d_sb == sb &&
		    dentry->d_parent == parent &&
		    dentry->d_name.len == name->len &&
		    strcmp(dentry->d_name.name, name->name) == 0) {
			spinlock_release(&dentry_state.lock);
			return dentry;
		}
	}

	spinlock_release(&dentry_state.lock);

	return NULL;
}

/*
 * 减少dentry的引用计数，引用为0时将dentry加入LRU链表
 * @param dentry: dentry指针
 */
void dentry_put(struct dentry *dentry)
{
	if (!dentry) {
		return;
	}

	spinlock_acquire(&dentry->d_lock);
	dentry->d_refcnt--;

	if (dentry->d_refcnt == 0) {
		spinlock_release(&dentry->d_lock);

		spinlock_acquire(&dentry_state.lock);
		list_add(&dentry->d_lru, &dentry_state.dentry_lru);
		spinlock_release(&dentry_state.lock);
		return;
	}

	spinlock_release(&dentry->d_lock);
}

/*
 * 增加dentry的引用计数
 * @param dentry: dentry指针
 */
void dentry_get(struct dentry *dentry)
{
	if (!dentry) {
		return;
	}

	spinlock_acquire(&dentry->d_lock);
	dentry->d_refcnt++;
	spinlock_release(&dentry->d_lock);
}

/*
 * 将dentry插入全局哈希表
 * @param dentry: 待插入的dentry指针
 * @return: 成功返回0，已存在返回-EEXIST，参数错误返回-EINVAL
 */
int dentry_insert(struct dentry *dentry)
{
	if (!dentry) {
		tracef("dentry_insert: dentry is NULL");
		return -EINVAL;
	}

	tracef("dentry_insert: dentry=%p, d_sb=%p, d_parent=%p, d_name=%s",
	       dentry, dentry->d_sb, dentry->d_parent,
	       dentry->d_name.name ? dentry->d_name.name : "(null)");

	if (!dentry->d_sb) {
		tracef("dentry_insert: d_sb is NULL for dentry %p", dentry);
		return -EINVAL;
	}

	if (!dentry->d_parent) {
		tracef("dentry_insert: d_parent is NULL for dentry %p", dentry);
		return -EINVAL;
	}

	spinlock_acquire(&dentry_state.lock);

	u32 key =
	    dentry_hash_key(dentry->d_sb, dentry->d_parent, &dentry->d_name);
	struct dentry *existing =
	    hashtable_lookup(&dentry_state.ht, (void *)(uintptr_t)key);
	if (existing) {
		spinlock_release(&dentry_state.lock);
		return -EEXIST;
	}

	hashtable_insert(&dentry_state.ht, (void *)(uintptr_t)key, dentry);
	list_add(&dentry->d_list, &dentry_state.dentry_list);
	dentry_state.count++;

	spinlock_release(&dentry_state.lock);
	return 0;
}

/*
 * 删除dentry（调用d_op->d_delete后释放dentry）
 * @param dentry: 待删除的dentry指针
 */
void dentry_delete(struct dentry *dentry)
{
	if (!dentry) {
		return;
	}

	if (dentry->d_op && dentry->d_op->d_delete) {
		if (dentry->d_op->d_delete(dentry) != 0) {
			return;
		}
	}

	dentry_free(dentry);
}

/*
 * 创建并插入根dentry（名称为"/"，父dentry指向自身）
 * @param sb: 超级块指针
 * @return: 成功返回根dentry指针，失败返回错误指针
 */
struct dentry *dentry_root(struct super_block *sb)
{
	struct dentry *root = dentry_alloc(NULL, sb, "/");
	if (IS_ERR(root)) {
		return root;
	}

	root->d_parent = root;

	if (dentry_insert(root) != 0) {
		dentry_free(root);
		return PTR(-EEXIST);
	}

	return root;
}
