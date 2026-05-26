#include <fs/fd_table.h>
#include <misc/log.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <mm/kalloc.h>
#include <sync/spinlock.h>

void fd_table_init(void) { infof("fd_table initialized"); }

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
