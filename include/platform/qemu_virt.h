#pragma once

#define CPU_NUM 1

#define CACHE_LINE_SIZE 64

#define TIMEBASE_FREQ 10000000UL
#define MEM_SIZE (128 * 1024 * 1024)
#define PM_START 0x80000000ULL

#ifndef __ASSEMBLY__

#define PLIC {.addr = 0x0c000000, .len = 0x600000}

#define VIRTIO_MMIO_0 {.addr = 0x10001000, .len = 0x1000, .irqno = 0x1}
#define VIRTIO_MMIO_1 {.addr = 0x10002000, .len = 0x1000, .irqno = 0x2}
#define VIRTIO_MMIO_2 {.addr = 0x10003000, .len = 0x1000, .irqno = 0x3}
#define VIRTIO_MMIO_3 {.addr = 0x10004000, .len = 0x1000, .irqno = 0x4}
#define VIRTIO_MMIO_4 {.addr = 0x10005000, .len = 0x1000, .irqno = 0x5}
#define VIRTIO_MMIO_5 {.addr = 0x10006000, .len = 0x1000, .irqno = 0x6}
#define VIRTIO_MMIO_6 {.addr = 0x10007000, .len = 0x1000, .irqno = 0x7}
#define VIRTIO_MMIO_7 {.addr = 0x10008000, .len = 0x1000, .irqno = 0x8}

#define UART_0 {.addr = 0x10000000, , .len = 0x100, .irqno = 0xa}

#endif
