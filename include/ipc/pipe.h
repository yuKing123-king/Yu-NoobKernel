#pragma once

#include <fs/file.h>

int pipe_create(struct file *rf, struct file *wf);
ssize_t pipe_read(struct file *f, void *buf, size_t count, loff_t *pos);
ssize_t pipe_write(struct file *f, const void *buf, size_t count, loff_t *pos);
int pipe_release(struct inode *inode, struct file *f);
