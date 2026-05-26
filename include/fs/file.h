#pragma once

#include <fs/fs_types.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct inode;
struct dentry;
struct file;
struct address_space;

struct file_operations {
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
	ssize_t (*read)(struct file *file, void *buf, size_t count,
			loff_t *pos);
	ssize_t (*write)(struct file *file, const void *buf, size_t count,
			 loff_t *pos);
	int (*readdir)(struct file *file, struct dirent *buf, size_t count);
	int (*open)(struct inode *inode, struct file *file);
	int (*release)(struct inode *inode, struct file *file);
	int (*flush)(struct file *file);
	int (*fsync)(struct file *file);
	int (*fasync)(struct file *file, int on);
};

struct address_space_operations {
	int (*readpage)(struct address_space *mapping, u64 page);
	int (*writepage)(struct address_space *mapping, u64 page);
	int (*readpages)(struct address_space *mapping, u64 start, u64 count);
	int (*writepages)(struct address_space *mapping, u64 start, u64 count);
};

struct address_space {
	struct inode *host;
	struct address_space_operations *a_ops;
	void *private_data;
	spinlock_t lock;
};

struct file {
	struct dentry *f_dentry;
	struct inode *f_inode;
	struct file_operations *f_op;
	void *f_private;
	loff_t f_pos;
	u32 f_flags;
	u32 f_mode;
	u32 f_refcnt;
	spinlock_t f_lock;
	struct list_head f_list;
};

void file_init(void);
struct file *file_alloc(void);
void file_free(struct file *file);
struct file *file_open(struct dentry *dentry, u32 flags);
int file_close(struct file *file);
ssize_t file_read(struct file *file, void *buf, size_t count);
ssize_t file_write(struct file *file, const void *buf, size_t count);
loff_t file_lseek(struct file *file, loff_t offset, int whence);
ssize_t file_getdents(struct file *file, struct dirent *buf, size_t count);
void file_get(struct file *file);
void file_put(struct file *file);
