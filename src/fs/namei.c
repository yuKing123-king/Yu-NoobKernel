#include <fs/namei.h>
#include <fs/vfs.h>
#include <fs/dentry.h>
#include <fs/inode.h>
#include <fs/super.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <misc/string.h>
#include <mm/kalloc.h>

/*
 * 如果dentry是挂载点则跟随到挂载文件系统的根dentry
 * @param dentry: 当前dentry指针
 * @return: 如果是挂载点则返回挂载根dentry，否则返回原dentry
 */
static struct dentry *follow_mount(struct dentry *dentry)
{
	struct mount *mnt = vfs_lookup_mount(dentry);
	if (mnt) {
		dentry_put(dentry);
		dentry_get(mnt->mnt_root);
		return mnt->mnt_root;
	}
	return dentry;
}

/*
 * 从路径中提取路径的第一个组件（直到遇到'/'或结束）
 * @param path: 路径字符串
 * @param name: 存放提取出的名称的缓冲区
 * @param len: 输出名称长度
 * @return: 成功返回0，失败返回负的错误码
 */
static int vfs_get_name(const char *path, char *name, u32 *len)
{
	if (!path || !name || !len) {
		return -EINVAL;
	}

	u32 i = 0;
	while (path[i] && path[i] != '/' && i < NAME_MAX) {
		name[i] = path[i];
		i++;
	}

	name[i] = '\0';
	*len = i;

	return 0;
}

/*
 * 跳过路径字符串开头的所有斜杠'/'
 * @param path: 路径字符串
 * @return: 指向第一个非斜杠字符的指针，输入为NULL时返回NULL
 */
static const char *vfs_skip_slashes(const char *path)
{
	if (!path) {
		return NULL;
	}

	while (*path == '/') {
		path++;
	}

	return path;
}

/*
 * 在base目录下查找单个路径组件的dentry（先查缓存再调用文件系统lookup）
 * @param base: 父目录dentry
 * @param name: 待查找的文件名
 * @param len: 文件名长度
 * @return: 成功返回dentry指针，失败返回错误指针
 */
static struct dentry *vfs_lookup_single(struct dentry *base, const char *name,
					u32 len)
{
	if (!base || !name || len == 0) {
		return PTR(-EINVAL);
	}

	struct mount *mnt = vfs_lookup_mount(base);
	if (mnt && mnt->mnt_root) {
		base = mnt->mnt_root;
	}

	if (!base->d_inode || !S_ISDIR(base->d_inode->i_mode)) {
		return PTR(-ENOTDIR);
	}

	if (!base->d_inode->i_op || !base->d_inode->i_op->lookup) {
		return PTR(-ENOSYS);
	}

	struct qstr qstr;
	qstr.name = (char *)name;
	qstr.len = len;
	qstr.hash = hash_string(name, len);

	struct dentry *dentry = dentry_lookup(base->d_sb, base, &qstr);
	if (dentry) {
		return dentry;
	}

	dentry = dentry_alloc(base, base->d_sb, name);
	if (IS_ERR(dentry)) {
		return dentry;
	}

	tracef("vfs_lookup_single: calling lookup, dentry=%p", dentry);
	struct dentry *result =
	    base->d_inode->i_op->lookup(base->d_inode, dentry);

	if (IS_ERR(result)) {
		dentry_free(dentry);
		return result;
	}

	if (result != dentry) {
		dentry_free(dentry);
		dentry = result;
	}

	dentry_insert(dentry);
	return dentry;
}

/*
 * 路径遍历主函数，逐组件解析路径直到末尾
 * @param nd: 路径遍历上下文
 * @param path: 待解析的路径字符串
 * @return: 成功返回0，失败返回负的错误码
 */
int vfs_path_walk(struct nameidata *nd, const char *path)
{
	if (!nd || !path) {
		return -EINVAL;
	}

	struct dentry *dentry = nd->dentry;
	if (!dentry) {
		return -ENOENT;
	}

	path = vfs_skip_slashes(path);
	if (!*path) {
		nd->dentry = dentry;
		nd->inode = dentry->d_inode;
		return 0;
	}

	char name[NAME_MAX + 1];
	u32 len;

	while (*path) {
		int ret = vfs_get_name(path, name, &len);
		if (ret < 0) {
			return ret;
		}

		if (strcmp(name, ".") == 0) {
			nd->last_type = LAST_DOT;
		} else if (strcmp(name, "..") == 0) {
			nd->last_type = LAST_DOTDOT;
			if (dentry->d_parent && dentry->d_parent != dentry) {
				dentry = dentry->d_parent;
			}
		} else {
			nd->last_type = LAST_NORM;

			struct dentry *next =
			    vfs_lookup_single(dentry, name, len);
			if (IS_ERR(next)) {
				return PTR_ERR(next);
			}

			next = follow_mount(next);

			dentry_put(dentry);
			dentry = next;
		}

		path += len;
		path = vfs_skip_slashes(path);

		if (!*path) {
			break;
		}

		if (!dentry->d_inode || !S_ISDIR(dentry->d_inode->i_mode)) {
			dentry_put(dentry);
			return -ENOTDIR;
		}
	}

	nd->dentry = dentry;
	nd->inode = dentry->d_inode;

	qstr_init(&nd->last, name);

	return 0;
}

/*
 * 解析路径字符串并返回对应的dentry
 * @param base: 起始dentry（为NULL时使用根dentry）
 * @param path: 待解析的路径
 * @param flags: 查找标志
 * @return: 成功返回目标dentry指针（引用计数已增加），失败返回错误指针
 */
struct dentry *vfs_path_lookup(struct dentry *base, const char *path, u32 flags)
{
	if (!path) {
		return PTR(-EINVAL);
	}

	if (!base) {
		base = vfs_get_root();
		if (!base) {
			return PTR(-ENOENT);
		}
	}

	dentry_get(base);

	struct nameidata nd;
	nd.dentry = base;
	nd.inode = base->d_inode;
	nd.flags = flags;
	nd.last_type = LAST_NORM;
	nd.depth = 0;

	int ret = vfs_path_walk(&nd, path);
	if (ret < 0) {
		dentry_put(base);
		return PTR((long)ret);
	}

	return nd.dentry;
}

/*
 * 在base目录下查找一个路径组件（不进行完整路径遍历）
 * @param base: 父目录dentry
 * @param name: 待查找的文件名
 * @param len: 文件名长度
 * @return: 成功返回dentry指针，失败返回错误指针
 */
struct dentry *vfs_lookup_one_len(struct dentry *base, const char *name,
				  u32 len)
{
	if (!base || !name || len == 0) {
		return PTR(-EINVAL);
	}

	return vfs_lookup_single(base, name, len);
}

/*
 * 解析路径，返回父目录dentry和最后一个路径组件的名称
 * @param base: 起始dentry（为NULL时使用根dentry）
 * @param path: 待解析的路径
 * @param parent: 输出父目录的path结构
 * @param last: 输出最后一个路径组件的名称
 * @return: 成功返回0，失败返回负的错误码
 */
int vfs_path_parent(struct dentry *base, const char *path, struct path *parent,
		    struct qstr *last)
{
	if (!path || !parent || !last) {
		return -EINVAL;
	}

	if (!base) {
		base = vfs_get_root();
		tracef("vfs_path_parent: root=%p", base);
		if (!base) {
			tracef("vfs_path_parent: no root");
			return -ENOENT;
		}
	}

	dentry_get(base);
	tracef("vfs_path_parent: base refcnt increased");

	char name[NAME_MAX + 1];
	u32 len;

	const char *p = path;
	const char *last_start = NULL;
	struct dentry *current = base;

	p = vfs_skip_slashes(p);
	tracef("vfs_path_parent: path='%s', p='%s'", path, p);

	while (*p) {
		tracef("vfs_path_parent: loop iteration, *p='%c'", *p);
		last_start = p;

		int ret = vfs_get_name(p, name, &len);
		if (ret < 0) {
			dentry_put(base);
			return ret;
		}

		tracef("vfs_path_parent: extracted name='%s', len=%u", name,
		       len);

		p += len;
		p = vfs_skip_slashes(p);
		tracef("vfs_path_parent: after skip, p='%s'", p);

		if (*p) {
			tracef("vfs_path_parent: looking up intermediate '%s'",
			       name);
			struct dentry *next =
			    vfs_lookup_single(current, name, len);
			if (IS_ERR(next)) {
				dentry_put(base);
				return PTR_ERR(next);
			}

			next = follow_mount(next);

			if (current != base) {
				dentry_put(current);
			}
			current = next;
		} else {
			tracef("vfs_path_parent: last component '%s'", name);
		}
	}

	tracef("vfs_path_parent: setting parent->dentry=%p", current);
	parent->dentry = current;

	tracef("vfs_path_parent: current->d_inode=%p", current->d_inode);
	parent->inode = current->d_inode;

	if (last_start) {
		vfs_get_name(last_start, name, &len);

		char *name_copy = kmalloc(len + 1);
		if (!name_copy) {
			dentry_put(base);
			return -ENOMEM;
		}
		strcpy(name_copy, name);
		qstr_init(last, name_copy);
	} else {
		char *empty = kmalloc(1);
		if (!empty) {
			dentry_put(base);
			return -ENOMEM;
		}
		empty[0] = '\0';
		qstr_init(last, empty);
	}

	return 0;
}
