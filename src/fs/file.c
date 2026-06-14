#include <fs/file.h>
#include <fs/inode.h>
#include <fs/dentry.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <mm/slab.h>
#include <sync/spinlock.h>

static struct kmem_cache file_cache;

static struct {
	struct list_head file_list;
	spinlock_t lock;
	u32 count;
} file_state;

/*
 * 初始化文件子系统（创建slab缓存和链表）
 */
void file_init(void)
{
	if (kmem_cache_init(&file_cache, "file", sizeof(struct file), false) !=
	    0) {
		panic("file_init: failed to create cache");
	}

	INIT_LIST_HEAD(&file_state.file_list);
	file_state.lock = SPINLOCK_INITIALIZER("file");
	file_state.count = 0;

	infof("file initialized");
}

/*
 * 分配并初始化一个新的file结构
 * @return: 成功返回file结构指针，失败返回错误指针
 */
struct file *file_alloc(void)
{
	struct file *file = kmem_cache_alloc(&file_cache);
	if (!file) {
		return PTR(-ENOMEM);
	}

	file->f_dentry = NULL;
	file->f_inode = NULL;
	file->f_op = NULL;
	file->f_private = NULL;
	file->f_pos = 0;
	file->f_flags = 0;
	file->f_mode = 0;
	file->f_refcnt = 1;
	file->f_lock = SPINLOCK_INITIALIZER("file");
	INIT_LIST_HEAD(&file->f_list);

	spinlock_acquire(&file_state.lock);
	list_add(&file->f_list, &file_state.file_list);
	file_state.count++;
	spinlock_release(&file_state.lock);

	return file;
}

/*
 * 释放file结构占用的内存
 * @param file: 待释放的file结构指针
 */
void file_free(struct file *file)
{
	if (!file) {
		return;
	}

	spinlock_acquire(&file_state.lock);
	list_del(&file->f_list);
	file_state.count--;
	spinlock_release(&file_state.lock);

	kmem_cache_free(file);
}

/*
 * 基于dentry打开一个文件
 * @param dentry: 文件对应的dentry指针
 * @param flags: 打开标志
 * @return: 成功返回file结构指针，失败返回错误指针
 */
struct file *file_open(struct dentry *dentry, u32 flags)
{
	if (!dentry || !dentry->d_inode) {
		return PTR(-ENOENT);
	}

	struct file *file = file_alloc();
	if (IS_ERR(file)) {
		return file;
	}

	file->f_dentry = dentry;
	file->f_inode = dentry->d_inode;
	file->f_flags = flags;
	file->f_op = dentry->d_inode->i_fop;

	if (S_ISDIR(dentry->d_inode->i_mode)) {
		file->f_mode = S_IFDIR;
	} else if (S_ISREG(dentry->d_inode->i_mode)) {
		file->f_mode = S_IFREG;
	}

	dentry_get(dentry);

	if (file->f_op && file->f_op->open) {
		int ret = file->f_op->open(dentry->d_inode, file);
		if (ret < 0) {
			dentry_put(dentry);
			file_free(file);
			return PTR((long)ret);
		}
	}

	return file;
}

/*
 * 关闭文件（减少引用计数，为0时释放资源并调用release回调）
 * @param file: 待关闭的file结构指针
 * @return: 成功返回0，失败返回负的错误码
 */
int file_close(struct file *file)
{
	if (!file) {
		return -EINVAL;
	}

	spinlock_acquire(&file->f_lock);
	file->f_refcnt--;

	if (file->f_refcnt == 0) {
		spinlock_release(&file->f_lock);

		if (file->f_op && file->f_op->release) {
			file->f_op->release(file->f_inode, file);
		}

		if (file->f_dentry) {
			dentry_put(file->f_dentry);
		}

		file_free(file);
		return 0;
	}

	spinlock_release(&file->f_lock);
	return 0;
}

/*
 * 从文件中读取数据
 * @param file: 文件结构指针
 * @param buf: 读取缓冲区
 * @param count: 要读取的字节数
 * @return: 成功返回实际读取的字节数，失败返回负的错误码
 */
ssize_t file_read(struct file *file, void *buf, size_t count)
{
	if (!file || !buf || count == 0) {
		return -EINVAL;
	}

	/* 管道和字符设备（如控制台）不需要 f_inode */
	if (file->f_mode == S_IFIFO || file->f_mode == S_IFCHR) {
		if (!file->f_op || !file->f_op->read)
			return -ENOSYS;
		loff_t pos = 0;
		return file->f_op->read(file, buf, count, &pos);
	}

	if (!file->f_inode) {
		return -ENOENT;
	}

	if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
		return -EACCES;
	}

	if (S_ISDIR(file->f_inode->i_mode)) {
		return -EISDIR;
	}

	if (!file->f_op || !file->f_op->read) {
		return -ENOSYS;
	}

	spinlock_acquire(&file->f_lock);

	loff_t pos = file->f_pos;
	if (pos >= file->f_inode->i_size) {
		spinlock_release(&file->f_lock);
		return 0;
	}

	if (pos + count > file->f_inode->i_size) {
		count = file->f_inode->i_size - pos;
	}

	ssize_t ret = file->f_op->read(file, buf, count, &pos);
	if (ret > 0) {
		file->f_pos = pos;
	}

	spinlock_release(&file->f_lock);
	return ret;
}

/*
 * 向文件中写入数据（支持追加模式）
 * @param file: 文件结构指针
 * @param buf: 写入数据缓冲区
 * @param count: 要写入的字节数
 * @return: 成功返回实际写入的字节数，失败返回负的错误码
 */
ssize_t file_write(struct file *file, const void *buf, size_t count)
{
	if (!file || !buf || count == 0) {
		return -EINVAL;
	}

	/* 管道和字符设备（如控制台）不需要 f_inode */
	if (file->f_mode == S_IFIFO || file->f_mode == S_IFCHR) {
		if (!file->f_op || !file->f_op->write)
			return -ENOSYS;
		loff_t pos = 0;
		return file->f_op->write(file, buf, count, &pos);
	}

	if (!file->f_inode) {
		return -ENOENT;
	}

	if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
		return -EACCES;
	}

	if (S_ISDIR(file->f_inode->i_mode)) {
		return -EISDIR;
	}

	if (!file->f_op || !file->f_op->write) {
		return -ENOSYS;
	}

	spinlock_acquire(&file->f_lock);

	loff_t pos = file->f_pos;

	if (file->f_flags & O_APPEND) {
		pos = file->f_inode->i_size;
	}

	ssize_t ret = file->f_op->write(file, buf, count, &pos);
	if (ret > 0) {
		file->f_pos = pos;
		if (pos > file->f_inode->i_size) {
			file->f_inode->i_size = pos;
		}
	}

	spinlock_release(&file->f_lock);
	return ret;
}

/*
 * 调整文件读写位置（支持SEEK_SET/SEEK_CUR/SEEK_END）
 * @param file: 文件结构指针
 * @param offset: 偏移量
 * @param whence: 基准位置
 * @return: 成功返回新的文件位置，失败返回负的错误码
 */
loff_t file_lseek(struct file *file, loff_t offset, int whence)
{
	if (!file) {
		return -EINVAL;
	}

	if (!file->f_inode) {
		return -ENOENT;
	}

	if (S_ISDIR(file->f_inode->i_mode)) {
		return -EISDIR;
	}

	spinlock_acquire(&file->f_lock);

	loff_t new_pos;
	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;
	case SEEK_CUR:
		new_pos = file->f_pos + offset;
		break;
	case SEEK_END:
		new_pos = file->f_inode->i_size + offset;
		break;
	default:
		spinlock_release(&file->f_lock);
		return -EINVAL;
	}

	if (new_pos < 0) {
		spinlock_release(&file->f_lock);
		return -EINVAL;
	}

	if (file->f_op && file->f_op->llseek) {
		new_pos = file->f_op->llseek(file, offset, whence);
		if (new_pos < 0) {
			spinlock_release(&file->f_lock);
			return new_pos;
		}
	}

	file->f_pos = new_pos;
	spinlock_release(&file->f_lock);
	return new_pos;
}

/*
 * 读取目录文件中的目录项
 * @param file: 目录文件结构指针
 * @param buf: 存放目录项的缓冲区
 * @param count: 缓冲区大小
 * @return: 成功返回读取的字节数，失败返回负的错误码
 */
ssize_t file_getdents(struct file *file, struct dirent *buf, size_t count)
{
	if (!file || !buf || count == 0) {
		return -EINVAL;
	}

	if (!file->f_inode) {
		return -ENOENT;
	}

	if (!S_ISDIR(file->f_inode->i_mode)) {
		return -ENOTDIR;
	}

	if (!file->f_op || !file->f_op->readdir) {
		return -ENOSYS;
	}

	int ret = file->f_op->readdir(file, buf, count);

	return ret;
}

/*
 * 增加file结构的引用计数
 * @param file: file结构指针
 */
void file_get(struct file *file)
{
	if (!file) {
		return;
	}

	spinlock_acquire(&file->f_lock);
	file->f_refcnt++;
	spinlock_release(&file->f_lock);
}

/*
 * 减少file结构的引用计数，为0时自动关闭文件
 * @param file: file结构指针
 */
void file_put(struct file *file)
{
	if (!file) {
		return;
	}

	spinlock_acquire(&file->f_lock);
	file->f_refcnt--;

	if (file->f_refcnt == 0) {
		spinlock_release(&file->f_lock);
		file_close(file);
		return;
	}

	spinlock_release(&file->f_lock);
}
