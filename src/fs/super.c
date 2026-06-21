#include <fs/super.h>
#include <fs/vfs.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <mm/kalloc.h>
#include <mm/slab.h>
#include <sync/spinlock.h>

static struct kmem_cache super_cache;

static struct {
	struct list_head super_list;
	spinlock_t lock;
	u32 count;
} super_state;

/*
 * 初始化超级块子系统（创建slab缓存和链表）
 */
void super_init(void)
{
	if (kmem_cache_init(&super_cache, "super_block",
			    sizeof(struct super_block), false) != 0) {
		panic("super_init: failed to create cache");
	}

	INIT_LIST_HEAD(&super_state.super_list);
	super_state.lock = SPINLOCK_INITIALIZER("super");
	super_state.count = 0;

}

/*
 * 分配并初始化一个新的超级块
 * @param type: 所属文件系统类型
 * @param dev: 设备号
 * @return: 成功返回超级块指针，失败返回错误指针
 */
struct super_block *super_alloc(struct file_system_type *type, dev_t dev)
{
	struct super_block *sb = kmem_cache_alloc(&super_cache);
	if (!sb) {
		return PTR(-ENOMEM);
	}

	sb->s_dev = dev;
	sb->s_type = type;
	sb->s_root = NULL;
	sb->s_op = NULL;
	sb->s_fs_info = NULL;
	INIT_LIST_HEAD(&sb->s_list);
	INIT_LIST_HEAD(&sb->s_inodes);
	sb->s_lock = SPINLOCK_INITIALIZER("super_block");
	sb->s_maxbytes = 0;
	sb->s_blocksize = BLOCK_SIZE;
	sb->s_blocksize_bits = 9;
	sb->s_flags = 0;
	sb->s_count = 1;

	return sb;
}

/*
 * 释放超级块占用的内存
 * @param sb: 待释放的超级块指针
 */
void super_free(struct super_block *sb)
{
	if (!sb) {
		return;
	}

	spinlock_acquire(&super_state.lock);
	list_del(&sb->s_list);
	super_state.count--;
	spinlock_release(&super_state.lock);

	kmem_cache_free(sb);
}

/*
 * 根据设备号查找超级块
 * @param dev: 设备号
 * @return: 成功返回超级块指针，未找到返回NULL
 */
struct super_block *super_lookup(dev_t dev)
{
	spinlock_acquire(&super_state.lock);

	struct super_block *sb;
	list_for_each_entry(sb, &super_state.super_list, s_list)
	{
		if (sb->s_dev == dev) {
			spinlock_release(&super_state.lock);
			return sb;
		}
	}

	spinlock_release(&super_state.lock);
	return NULL;
}

/*
 * 增加超级块的引用计数
 * @param sb: 超级块指针
 */
void super_get(struct super_block *sb)
{
	if (!sb) {
		return;
	}

	spinlock_acquire(&sb->s_lock);
	sb->s_count++;
	spinlock_release(&sb->s_lock);
}

/*
 * 减少超级块的引用计数，引用为0时释放超级块
 * @param sb: 超级块指针
 */
void super_put(struct super_block *sb)
{
	if (!sb) {
		return;
	}

	spinlock_acquire(&sb->s_lock);
	sb->s_count--;

	if (sb->s_count == 0) {
		spinlock_release(&sb->s_lock);

		if (sb->s_op && sb->s_op->put_super) {
			sb->s_op->put_super(sb);
		}

		super_free(sb);
		return;
	}

	spinlock_release(&sb->s_lock);
}

/*
 * 注册超级块到全局链表
 * @param sb: 待注册的超级块指针
 * @return: 成功返回0，设备号重复返回-EEXIST
 */
int super_register(struct super_block *sb)
{
	if (!sb) {
		return -EINVAL;
	}

	spinlock_acquire(&super_state.lock);

	struct super_block *existing;
	list_for_each_entry(existing, &super_state.super_list, s_list)
	{
		if (existing->s_dev == sb->s_dev) {
			spinlock_release(&super_state.lock);
			return -EEXIST;
		}
	}

	list_add(&sb->s_list, &super_state.super_list);
	super_state.count++;

	spinlock_release(&super_state.lock);
	return 0;
}

/*
 * 从全局链表中注销超级块
 * @param sb: 待注销的超级块指针
 */
void super_unregister(struct super_block *sb)
{
	if (!sb) {
		return;
	}

	spinlock_acquire(&super_state.lock);
	list_del(&sb->s_list);
	super_state.count--;
	spinlock_release(&super_state.lock);
}
