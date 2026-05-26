#pragma once

#include <fs/file.h>
#include <misc/stdint.h>
#include <misc/stddef.h>
#include <sync/spinlock.h>

#define NR_OPEN_DEFAULT 32
#define NR_OPEN_MAX 1024

struct fd_table {
	struct file **fds;
	u32 max_fds;
	u32 open_fds;
	spinlock_t lock;
};

struct fd_table *fd_table_alloc(void);
void fd_table_free(struct fd_table *fdt);
struct fd_table *fd_table_dup(struct fd_table *fdt);

int fd_alloc(struct fd_table *fdt);
void fd_free(struct fd_table *fdt, int fd);
struct file *fd_get(struct fd_table *fdt, int fd);
int fd_install(struct fd_table *fdt, int fd, struct file *file);

void fd_table_init(void);
