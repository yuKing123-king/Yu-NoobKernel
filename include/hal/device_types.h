#pragma once

#include <misc/stdint.h>

struct plic_raw{
	uintptr_t addr;
	size_t len;
};

struct virtio_mmio_raw{
	uintptr_t addr;
	size_t len;
	u32 irqno;
};

struct uart_raw{
	uintptr_t addr;
	size_t len;
	u32 irqno;
};

