#pragma once

#include <misc/stdint.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct kmem_cache{
	const char *name;
	u32 obj_size;
	u32 alloc_size;
	u64 total;
	u64 free;
	u32 low;
	u32 high;
	bool expanding;
	struct list_head list;

	spinlock_t lock;
	struct list_head slabs_full;
	struct list_head slabs_partial;
	struct list_head slabs_free;
};

int kmem_cache_init(struct kmem_cache *kmem, const char *name, size_t obj_size,
		    bool with_initial_alloc);
void *kmem_cache_alloc(struct kmem_cache *kmem);
void kmem_cache_free(void *addr);
void kmem_cache_flush(struct kmem_cache *kmem);
