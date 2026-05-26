#pragma once

#include <fs/fs_types.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct inode;
struct super_block;
struct dentry;

struct dentry_operations {
	int (*d_revalidate)(struct dentry *dentry);
	int (*d_hash)(struct dentry *dentry, struct qstr *name);
	int (*d_compare)(struct dentry *dentry, struct qstr *name1,
			 struct qstr *name2);
	int (*d_delete)(struct dentry *dentry);
	void (*d_release)(struct dentry *dentry);
};

struct dentry {
	struct qstr d_name;
	struct inode *d_inode;
	struct super_block *d_sb;
	struct dentry *d_parent;
	struct list_head d_children;
	struct list_head d_sibling;
	struct dentry_operations *d_op;
	void *d_fsdata;

	struct list_head d_lru;
	struct list_head d_hash;
	struct list_head d_list;
	spinlock_t d_lock;
	u32 d_refcnt;
	u32 d_flags;
};

#define DCACHE_UNHASHED 0x0001

void dentry_init(void);
struct dentry *dentry_alloc(struct dentry *parent, struct super_block *sb,
			    const char *name);
void dentry_free(struct dentry *dentry);
struct dentry *dentry_lookup(struct super_block *sb, struct dentry *parent,
			     struct qstr *name);
void dentry_put(struct dentry *dentry);
void dentry_get(struct dentry *dentry);
int dentry_insert(struct dentry *dentry);
void dentry_delete(struct dentry *dentry);
struct dentry *dentry_root(struct super_block *sb);
