#pragma once

#include <fs/fs_types.h>
#include <fs/super.h>
#include <fs/inode.h>
#include <fs/dentry.h>
#include <fs/file.h>
#include <fs/namei.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct statfs;

struct file_system_type {
	const char *name;
	struct super_block *(*mount)(struct file_system_type *fs_type,
				     dev_t dev, void *data);
	void (*kill_sb)(struct super_block *sb);
	struct list_head fs_list;
	spinlock_t fs_lock;
};

struct mount {
	struct mount *mnt_parent;
	struct dentry *mnt_mountpoint;
	struct dentry *mnt_root;
	struct super_block *mnt_sb;
	struct list_head mnt_mounts;
	struct list_head mnt_child;
	spinlock_t mnt_lock;
	u32 mnt_flags;
	u32 mnt_refcnt;
};

struct vfsmount {
	struct mount *mnt;
};

#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT 32
#define MS_MANDLOCK 64

int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
struct file_system_type *get_fs(const char *name);

struct super_block *vfs_mount(struct file_system_type *fs_type, dev_t dev,
			      void *data);
int vfs_umount(struct super_block *sb);
int vfs_mount_root(struct file_system_type *fs_type, dev_t dev);
int vfs_mount_to(const char *target_path, struct file_system_type *fs_type,
		 dev_t dev);

struct mount *vfs_lookup_mount(struct dentry *mountpoint);
struct mount *vfs_get_root_mount(void);
void vfs_mount_get(struct mount *mnt);
void vfs_mount_put(struct mount *mnt);

struct file *vfs_open(const char *path, u32 flags);
int vfs_close(struct file *file);
ssize_t vfs_read(struct file *file, void *buf, size_t count);
ssize_t vfs_write(struct file *file, const void *buf, size_t count);
loff_t vfs_lseek(struct file *file, loff_t offset, int whence);
ssize_t vfs_getdents(struct file *file, struct dirent *buf, size_t count);

int vfs_mkdir(const char *path, umode_t mode);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_create(const char *path, umode_t mode);
int vfs_rename(const char *old_path, const char *new_path);

struct dentry *vfs_get_root(void);

int vfs_statfs(const char *path, struct statfs *buf);
int vfs_sync(void);
int vfs_fsync(struct file *file);

void vfs_init(void);
