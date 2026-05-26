#pragma once

#if defined(QEMU)
#include <platform/qemu_virt.h>
#endif

#define LOG_LEVEL 3

#define TIMER_IRQ_HZ 100

#define PAGE_SHIFT 12
#define PAGE_SIZE (1ULL << PAGE_SHIFT)
#define PAGE_NUM (MEM_SIZE / PAGE_SIZE)

#define PM_END (PM_START + MEM_SIZE)

#define SLAB_MAX_OBJ_SIZE (8192)
#define SLAB_BLOB_SIZE (16 * PAGE_SIZE)

#define EARLY_HEAP_SIZE (PAGE_SIZE)

#define BUDDY_MAX_ORDER 11
#define BUDDY_BLOB_SIZE ((1ULL << BUDDY_MAX_ORDER) * PAGE_SIZE)

#define USTACK_SIZE (8 * PAGE_SIZE)
#define KSTACK_SIZE (PAGE_SIZE)
#define TRAP_PAGE_SIZE (PAGE_SIZE)
#define IDLE_STACK_SIZE (PAGE_SIZE)
#define BOOT_STACK_SIZE (PAGE_SIZE)

#define PID_MIN 2
#define PID_MAX INT32_MAX

#define BLOCK_SIZE 512
#define BCACHE_SIZE 128
#define BCACHE_HASH_SIZE 17

#define VIRTQ_SIZE 32
