#pragma once

#include <misc/align.h>
extern char skernel[];
extern char ekernel[];

#define SBI_BASE PM_START
#define KERNEL_BASE PAGE_ALIGN_DOWN((uintptr_t)skernel)
#define KERNEL_END PAGE_ALIGN_UP((uintptr_t)ekernel)

#define EARLY_HEAP_BASE PAGE_ALIGN_UP(KERNEL_END + PAGE_SIZE)
#define EARLY_HEAP_END (EARLY_HEAP_BASE + EARLY_HEAP_SIZE)

#define BUDDY_SYSTEM_BASE BUDDY_ALIGN_UP(EARLY_HEAP_END)
#define BUDDY_SYSTEM_END (BUDDY_ALIGN_DOWN(PM_END))
#define BUDDY_BLOB_NUM                                                         \
	((BUDDY_SYSTEM_END - BUDDY_SYSTEM_BASE) / BUDDY_BLOB_SIZE)

