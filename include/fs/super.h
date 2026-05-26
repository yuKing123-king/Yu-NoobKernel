#pragma once

#include <fs/fs_types.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct inode;
struct dentry;
struct file_system_type;
struct super_block;

struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *inode);
	void (*dirty_inode)(struct inode *inode);
	int (*write_inode)(struct inode *inode);
	int (*drop_inode)(struct inode *inode);
	void (*put_super)(struct super_block *sb);
	int (*sync_fs)(struct super_block *sb);
	int (*statfs)(struct super_block *sb);
	void (*remount_fs)(struct super_block *sb);
};

struct super_block {
	dev_t s_dev;
	struct file_system_type *s_type;
	struct dentry *s_root;
	struct super_operations *s_op;
	void *s_fs_info;
	struct list_head s_list;
	struct list_head s_inodes;
	spinlock_t s_lock;
	u64 s_maxbytes;
	u32 s_blocksize;
	u32 s_blocksize_bits;
	u64 s_flags;
	u32 s_count;
};

void super_init(void);
struct super_block *super_alloc(struct file_system_type *type, dev_t dev);
void super_free(struct super_block *sb);
struct super_block *super_lookup(dev_t dev);
void super_get(struct super_block *sb);
void super_put(struct super_block *sb);
int super_register(struct super_block *sb);
void super_unregister(struct super_block *sb);
