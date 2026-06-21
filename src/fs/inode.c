#include <fs/inode.h>
#include <fs/super.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <misc/hashtable.h>
#include <mm/slab.h>
#include <sync/spinlock.h>

static struct kmem_cache inode_cache;

static struct {
	struct hashtable ht;
	struct list_head inode_list;
	spinlock_t lock;
	u32 count;
} inode_state;

/*
 * 初始化inode子系统（创建slab缓存和哈希表）
 */
void inode_init(void)
{
	if (kmem_cache_init(&inode_cache, "inode", sizeof(struct inode),
			    false) != 0) {
		panic("inode_init: failed to create cache");
	}

	if (hashtable_init(&inode_state.ht, 64) != 0) {
		panic("inode_init: failed to init hashtable");
	}

	INIT_LIST_HEAD(&inode_state.inode_list);
	inode_state.lock = SPINLOCK_INITIALIZER("inode");
	inode_state.count = 0;

}

/*
 * 分配并初始化一个新的inode
 * @param sb: 所属超级块指针
 * @return: 成功返回inode指针，失败返回错误指针
 */
struct inode *inode_alloc(struct super_block *sb)
{
	struct inode *inode;

	if (sb && sb->s_op && sb->s_op->alloc_inode) {
		inode = sb->s_op->alloc_inode(sb);
		if (IS_ERR(inode) || !inode) {
			return inode;
		}
	} else {
		inode = kmem_cache_alloc(&inode_cache);
		if (!inode) {
			return PTR(-ENOMEM);
		}
	}

	inode->i_ino = 0;
	inode->i_mode = 0;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode->i_atime = 0;
	inode->i_mtime = 0;
	inode->i_ctime = 0;
	inode->i_nlink = 1;
	inode->i_rdev = 0;

	inode->i_sb = sb;
	inode->i_op = NULL;
	inode->i_fop = NULL;
	inode->i_mapping = NULL;

	INIT_LIST_HEAD(&inode->i_list);
	INIT_LIST_HEAD(&inode->i_dentry);
	inode->i_lock = SPINLOCK_INITIALIZER("inode");
	inode->i_refcnt = 1;
	inode->i_state = I_NEW;

	return inode;
}

/*
 * 释放inode占用的内存
 * @param inode: 待释放的inode指针
 */
void inode_free(struct inode *inode)
{
	if (!inode) {
		return;
	}

	if (inode->i_sb && inode->i_sb->s_op &&
	    inode->i_sb->s_op->destroy_inode) {
		inode->i_sb->s_op->destroy_inode(inode);
		return;
	}

	spinlock_acquire(&inode_state.lock);
	hashtable_delete(&inode_state.ht, (void *)inode->i_ino);
	list_del(&inode->i_list);
	inode_state.count--;
	spinlock_release(&inode_state.lock);

	kmem_cache_free(inode);
}

/*
 * 获取指定inode号的inode（从缓存查找或新建）
 * @param sb: 超级块指针
 * @param ino: inode号
 * @return: 成功返回inode指针（引用计数已增加），失败返回错误指针
 */
struct inode *inode_get(struct super_block *sb, ino_t ino)
{
	spinlock_acquire(&inode_state.lock);

	struct inode *inode = hashtable_lookup(&inode_state.ht, (void *)ino);
	if (inode) {
		inode->i_refcnt++;
		spinlock_release(&inode_state.lock);
		return inode;
	}

	spinlock_release(&inode_state.lock);

	inode = inode_alloc(sb);
	if (IS_ERR(inode)) {
		return inode;
	}

	inode->i_ino = ino;

	spinlock_acquire(&inode_state.lock);
	hashtable_insert(&inode_state.ht, (void *)ino, inode);
	list_add(&inode->i_list, &inode_state.inode_list);
	inode_state.count++;
	spinlock_release(&inode_state.lock);

	return inode;
}

/*
 * 减少inode的引用计数，引用为0时释放inode
 * @param inode: inode指针
 */
void inode_put(struct inode *inode)
{
	if (!inode) {
		return;
	}

	spinlock_acquire(&inode->i_lock);
	inode->i_refcnt--;

	if (inode->i_refcnt == 0) {
		spinlock_release(&inode->i_lock);

		if (inode->i_sb && inode->i_sb->s_op &&
		    inode->i_sb->s_op->drop_inode) {
			if (inode->i_sb->s_op->drop_inode(inode) != 0) {
				return;
			}
		}

		inode_free(inode);
		return;
	}

	spinlock_release(&inode->i_lock);
}

/*
 * 根据inode号在全局哈希表中查找inode（不增加引用计数）
 * @param ino: inode号
 * @return: 成功返回inode指针，未找到返回NULL
 */
struct inode *inode_lookup(ino_t ino)
{
	spinlock_acquire(&inode_state.lock);
	struct inode *inode = hashtable_lookup(&inode_state.ht, (void *)ino);
	spinlock_release(&inode_state.lock);
	return inode;
}

/*
 * 标记inode为脏（数据已修改待写回）
 * @param inode: inode指针
 */
void inode_dirty(struct inode *inode)
{
	if (!inode) {
		return;
	}

	spinlock_acquire(&inode->i_lock);
	inode->i_state |= I_DIRTY;

	if (inode->i_sb && inode->i_sb->s_op &&
	    inode->i_sb->s_op->dirty_inode) {
		inode->i_sb->s_op->dirty_inode(inode);
	}

	spinlock_release(&inode->i_lock);
}

/*
 * 将脏inode写回到磁盘
 * @param inode: inode指针
 * @return: 成功返回0，失败返回负的错误码
 */
int inode_write(struct inode *inode)
{
	if (!inode) {
		return -EINVAL;
	}

	spinlock_acquire(&inode->i_lock);

	if (!(inode->i_state & I_DIRTY)) {
		spinlock_release(&inode->i_lock);
		return 0;
	}

	if (!inode->i_sb || !inode->i_sb->s_op ||
	    !inode->i_sb->s_op->write_inode) {
		spinlock_release(&inode->i_lock);
		return -ENOSYS;
	}

	int ret = inode->i_sb->s_op->write_inode(inode);
	if (ret == 0) {
		inode->i_state &= ~I_DIRTY;
	}

	spinlock_release(&inode->i_lock);
	return ret;
}

/*
 * 截断inode数据（清空大小和块数，标记为脏）
 * @param inode: inode指针
 */
void inode_truncate(struct inode *inode)
{
	if (!inode) {
		return;
	}

	spinlock_acquire(&inode->i_lock);
	inode->i_size = 0;
	inode->i_blocks = 0;
	inode_dirty(inode);
	spinlock_release(&inode->i_lock);
}
