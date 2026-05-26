#pragma once

#include <misc/stdint.h>
#include <misc/stdbool.h>

#define NULL ((void *)0)

#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN_UP(x, n) (((size_t)(x) + (size_t)(n)-1) & ~((size_t)(n)-1))
#define ALIGN_DOWN(x, n) ((size_t)(x) & ~((size_t)(n)-1))
#define ALIGNED(x, n) (((size_t)(x) & ((size_t)(n)-1)) == 0)

#undef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)

#define container_of(ptr, type, member)                                        \
	({                                                                     \
		void *__mptr = (void *)(ptr);                                  \
		((type *)(__mptr - offsetof(type, member)));                   \
	})

typedef int dev_t;
#define MAJOR(dev) ((dev) >> 20)
#define MINOR(dev) ((dev) & 0x000fffff)
#define MKDEV(major, minor) (((major) << 20) | ((minor) & 0xfffff))

typedef ssize_t off_t;
typedef size_t ino_t;
typedef u32 mode_t;

typedef u64 tick_t;
typedef u64 *pagetable_t;

typedef int pid_t;
