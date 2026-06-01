#include <fs/fd_table.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <mm/kalloc.h>
#include <sync/spinlock.h>

/*
 * 初始化文件描述符表子系统
 */
void fd_table_init(void) { infof("fd_table initialized"); }

/*
 * 分配并初始化一个新的文件描述符表
 * @return: 成功返回fd_table指针，失败返回NULL
 */
struct fd_table *fd_table_alloc(void)
{
	struct fd_table *fdt = kmalloc(sizeof(struct fd_table));
	if (!fdt) {
		return NULL;
	}

	fdt->fds = kmalloc(sizeof(struct file *) * NR_OPEN_DEFAULT);
	if (!fdt->fds) {
		kfree(fdt);
		return NULL;
	}

	for (u32 i = 0; i < NR_OPEN_DEFAULT; i++) {
		fdt->fds[i] = NULL;
	}

	fdt->max_fds = NR_OPEN_DEFAULT;
	fdt->open_fds = 0;
	fdt->lock = SPINLOCK_INITIALIZER("fd_table");

	return fdt;
}

/*
 * 释放文件描述符表，关闭所有已打开的文件
 * @param fdt: 待释放的fd_table指针
 */
void fd_table_free(struct fd_table *fdt)
{
	if (!fdt) {
		return;
	}

	spinlock_acquire(&fdt->lock);

	for (u32 i = 0; i < fdt->max_fds; i++) {
		if (fdt->fds[i]) {
			file_put(fdt->fds[i]);
			fdt->fds[i] = NULL;
		}
	}

	kfree(fdt->fds);
	spinlock_release(&fdt->lock);
	kfree(fdt);
}

/*
 * 复制文件描述符表（共享底层file结构，增加引用计数）
 * @param fdt: 源fd_table指针
 * @return: 成功返回新的fd_table指针，失败返回NULL
 */
struct fd_table *fd_table_dup(struct fd_table *fdt)
{
	if (!fdt) {
		return NULL;
	}

	struct fd_table *new_fdt = fd_table_alloc();
	if (!new_fdt) {
		return NULL;
	}

	spinlock_acquire(&fdt->lock);
	spinlock_acquire(&new_fdt->lock);

	for (u32 i = 0; i < fdt->max_fds && i < new_fdt->max_fds; i++) {
		if (fdt->fds[i]) {
			file_get(fdt->fds[i]);
			new_fdt->fds[i] = fdt->fds[i];
			new_fdt->open_fds++;
		}
	}

	spinlock_release(&new_fdt->lock);
	spinlock_release(&fdt->lock);

	return new_fdt;
}

/*
 * 从文件描述符表中分配一个空闲的fd编号（必要时扩展表大小）
 * @param fdt: fd_table指针
 * @return: 成功返回fd编号，失败返回负的错误码
 */
int fd_alloc(struct fd_table *fdt)
{
	if (!fdt) {
		return -EINVAL;
	}

	spinlock_acquire(&fdt->lock);

	int fd = -EMFILE;
	for (u32 i = 0; i < fdt->max_fds; i++) {
		if (!fdt->fds[i]) {
			fd = (int)i;
			fdt->open_fds++;
			break;
		}
	}

	if (fd < 0 && fdt->max_fds < NR_OPEN_MAX) {
		u32 new_max = fdt->max_fds * 2;
		if (new_max > NR_OPEN_MAX) {
			new_max = NR_OPEN_MAX;
		}

		struct file **new_fds =
		    kmalloc(sizeof(struct file *) * new_max);
		if (new_fds) {
			for (u32 i = 0; i < fdt->max_fds; i++) {
				new_fds[i] = fdt->fds[i];
			}
			for (u32 i = fdt->max_fds; i < new_max; i++) {
				new_fds[i] = NULL;
			}

			kfree(fdt->fds);
			fdt->fds = new_fds;
			fdt->max_fds = new_max;

			fd = (int)fdt->max_fds / 2;
			fdt->open_fds++;
		}
	}

	spinlock_release(&fdt->lock);
	return fd;
}

/*
 * 释放文件描述符表中的指定fd
 * @param fdt: fd_table指针
 * @param fd: 待释放的文件描述符编号
 */
void fd_free(struct fd_table *fdt, int fd)
{
	if (!fdt || fd < 0 || (u32)fd >= fdt->max_fds) {
		return;
	}

	spinlock_acquire(&fdt->lock);

	if (fdt->fds[fd]) {
		file_put(fdt->fds[fd]);
		fdt->fds[fd] = NULL;
		fdt->open_fds--;
	}

	spinlock_release(&fdt->lock);
}

/*
 * 获取文件描述符表中指定fd对应的file结构（增加引用计数）
 * @param fdt: fd_table指针
 * @param fd: 文件描述符编号
 * @return: 成功返回file结构指针，无效fd返回NULL
 */
struct file *fd_get(struct fd_table *fdt, int fd)
{
	if (!fdt || fd < 0 || (u32)fd >= fdt->max_fds) {
		return NULL;
	}

	spinlock_acquire(&fdt->lock);
	struct file *file = fdt->fds[fd];
	if (file) {
		file_get(file);
	}
	spinlock_release(&fdt->lock);

	return file;
}

/*
 * 将file结构安装到文件描述符表中的指定位置
 * @param fdt: fd_table指针
 * @param fd: 目标文件描述符编号
 * @param file: 待安装的file结构指针
 * @return: 成功返回0，失败返回负的错误码
 */
int fd_install(struct fd_table *fdt, int fd, struct file *file)
{
	if (!fdt || !file || fd < 0 || (u32)fd >= fdt->max_fds) {
		return -EINVAL;
	}

	spinlock_acquire(&fdt->lock);

	if (fdt->fds[fd]) {
		spinlock_release(&fdt->lock);
		return -EBADF;
	}

	fdt->fds[fd] = file;
	fdt->open_fds++;

	spinlock_release(&fdt->lock);
	return 0;
}
