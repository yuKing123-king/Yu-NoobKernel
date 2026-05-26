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

	infof("inode initialized");
}

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

struct inode *inode_lookup(ino_t ino)
{
	spinlock_acquire(&inode_state.lock);
	struct inode *inode = hashtable_lookup(&inode_state.ht, (void *)ino);
	spinlock_release(&inode_state.lock);
	return inode;
}

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
