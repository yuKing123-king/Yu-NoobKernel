#pragma once

#include <fs/fs_types.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct super_block;
struct dentry;
struct file;
struct address_space;
struct inode;

struct inode_operations {
	struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);
	int (*create)(struct inode *dir, struct dentry *dentry, umode_t mode);
	int (*link)(struct dentry *old_dentry, struct inode *dir,
		    struct dentry *dentry);
	int (*unlink)(struct inode *dir, struct dentry *dentry);
	int (*symlink)(struct inode *dir, struct dentry *dentry,
		       const char *symname);
	int (*mkdir)(struct inode *dir, struct dentry *dentry, umode_t mode);
	int (*rmdir)(struct inode *dir, struct dentry *dentry);
	int (*mknod)(struct inode *dir, struct dentry *dentry, umode_t mode,
		     dev_t dev);
	int (*rename)(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry);
	int (*permission)(struct inode *inode, int mask);
	int (*getattr)(struct dentry *dentry);
	int (*setattr)(struct dentry *dentry);
};

struct file_operations;

struct inode {
	ino_t i_ino;
	umode_t i_mode;
	uid_t i_uid;
	gid_t i_gid;
	u64 i_size;
	u64 i_blocks;
	time_t i_atime;
	time_t i_mtime;
	time_t i_ctime;
	u32 i_nlink;
	dev_t i_rdev;

	struct super_block *i_sb;
	struct inode_operations *i_op;
	struct file_operations *i_fop;
	struct address_space *i_mapping;
	void *i_private;

	struct list_head i_list;
	struct list_head i_dentry;
	spinlock_t i_lock;
	u32 i_refcnt;
	u32 i_state;
};

#define I_NEW 1
#define I_CREATING 2
#define I_DIRTY 3

struct inode *inode_alloc(struct super_block *sb);
void inode_free(struct inode *inode);
struct inode *inode_get(struct super_block *sb, ino_t ino);
void inode_put(struct inode *inode);
struct inode *inode_lookup(ino_t ino);
void inode_init(void);
void inode_dirty(struct inode *inode);
int inode_write(struct inode *inode);
void inode_truncate(struct inode *inode);
