#pragma once

#include <misc/stdint.h>

#define PM_BUDDY 0x01
#define PM_SLAB 0x02
#define PM_STATIC 0x04 // 静态内存，不参与分配回收

struct page {
	u32 refs;
	u16 flags;
	u8 order;
	void *private;
};

int pm_init();
void print_pm_layout();
struct page *addr2page(void *addr);
void *page2addr(struct page *page);
void *page_alloc(u8 flags);
int page_free(void *ptr);
size_t get_free_pages_num();
