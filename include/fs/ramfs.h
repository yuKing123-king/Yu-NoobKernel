#pragma once

#include <fs/vfs.h>

int ramfs_init(void);
struct file_system_type *ramfs_get_fs_type(void);
