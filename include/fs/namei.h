#pragma once

#include <fs/fs_types.h>
#include <misc/stddef.h>

struct dentry;
struct inode;
struct nameidata;

struct nameidata {
	struct dentry *dentry;
	struct inode *inode;
	u32 flags;
	u32 last_type;
	struct qstr last;
	u32 depth;
};

#define LOOKUP_FOLLOW 1
#define LOOKUP_DIRECTORY 2
#define LOOKUP_CREATE 4
#define LOOKUP_EXCL 5

#define LAST_NORM 0
#define LAST_ROOT 1
#define LAST_DOT 2
#define LAST_DOTDOT 3

struct dentry *vfs_path_lookup(struct dentry *base, const char *path,
			       u32 flags);
struct dentry *vfs_path_lookup_create(struct dentry *base, const char *path,
				      umode_t mode, u32 flags);
int vfs_path_walk(struct nameidata *nd, const char *path);
int vfs_link_path_walk(struct nameidata *nd, const char *name, u32 len);
struct dentry *vfs_lookup_one_len(struct dentry *base, const char *name,
				  u32 len);
int vfs_do_lookup(struct nameidata *nd);
struct path {
	struct dentry *dentry;
	struct inode *inode;
};
int vfs_path_parent(struct dentry *base, const char *path, struct path *parent,
		    struct qstr *last);
