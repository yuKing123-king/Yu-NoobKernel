#include <sync/spinlock.h>
#include <mm/slab.h>
#include <misc/align.h>
#include <misc/stdbool.h>
#include <misc/string.h>
#include <misc/errno.h>
#include <misc/log.h>
#include <misc/math.h>
#include <mm/buddy.h>
#include <mm/pm.h>
#include <misc/list.h>
#include <misc/bitmap.h>
#include <mm/kalloc.h>
#include <misc/complier.h>

extern bool buddy_inited;
const u16 SLAB_MAGIC = 0x51AB; // 魔数，用于验证slab的有效性
#if 0
#define slab_log(fmt, ...) infof("slab: " fmt, ##__VA_ARGS__)
#else
#define slab_log(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif
struct slab {
	struct list_head list;
	struct kmem_cache *kmem;
	struct bitmap bm;
	u32 total;
	u32 free;
	u32 offset;
	u16 magic;
	u8 bm_data[];
};

static u32 object_num(u32 obj_size)
{
	size_t D = 8 * obj_size + 1;
	size_t N = 8 * (SLAB_BLOB_SIZE - sizeof(struct slab)) - 7;
	return (u32)(N / D);
}

static inline bool slab_empty(struct slab *s) { return (s->total == s->free); }

// 上层确保alloc_size对指令宽度对齐
static void slab_init(struct kmem_cache *kmem, void *blob)
{
	for (uintptr_t p = 0; p < SLAB_BLOB_SIZE; p += PAGE_SIZE) {
		struct page *page = addr2page((void *)(p + (uintptr_t)blob));
		page->flags |= PM_SLAB;
	}
	struct slab *s = blob;
	u32 total = object_num(kmem->alloc_size);
	u32 bitmap_size = ALIGN_UP(total, 8) / 8;
	u32 offset =
	    ALIGN_UP(sizeof(struct slab) + bitmap_size, kmem->obj_size);
	*s = (struct slab){
	    .kmem = kmem,
	    .total = total,
	    .free = total,
	    .offset = offset,
	    .magic = SLAB_MAGIC,
	};
	memset(s->bm_data, 0, bitmap_size);
	s->bm.data = s->bm_data;
	s->bm.len = total;
	list_add_tail(
	    &s->list,
	    &kmem->slabs_partial); // 这里虽然全空，但是直接放到可用链表
	slab_log("inited %p - %p", blob, blob + SLAB_BLOB_SIZE);
}

static void slab_destroy(struct slab *s)
{
	void *blob = s;
	for (uintptr_t p = 0; p < SLAB_BLOB_SIZE; p += PAGE_SIZE) {
		struct page *page = addr2page((void *)((uintptr_t)blob + p));
		if (page)
			page->flags &= ~PM_SLAB;
		else
			panic("invalid addr in slab_destroy: %p", blob);
	}

	s->magic = 0;
	list_del(&s->list);
	buddy_free(blob);
}

static void *slab_alloc(struct slab *s)
{
	s->free--;
	if (s->free == 0) {
		list_move(&s->list, &s->kmem->slabs_full);
	}
	u32 idx = bitmap_find_next_clear(&s->bm, 0);
	if (idx == s->bm.len)
		panic("slab meta data error");
	bitmap_set(&s->bm, idx);
	slab_log(
	    "%s allocated: %p", s->kmem->name,
	    (void *)((uintptr_t)s + s->offset + idx * s->kmem->alloc_size));
	return (void *)((uintptr_t)s + s->offset + idx * s->kmem->alloc_size);
}

// 上层确保addr属于slab内存，且s->kmem有效
static void slab_free(void *addr)
{
	struct slab *s = (void *)SLAB_ALIGN_DOWN(addr);
	if (s->magic != SLAB_MAGIC)
		panic("trying free %p that doesn't belong to a slab", addr);

	u32 obj_offset = (uintptr_t)addr - (uintptr_t)s;
	if (obj_offset < s->offset)
		panic("trying free %p belongs to a slab's meta data", addr);

	u32 idx = (obj_offset - s->offset) / s->kmem->alloc_size;
	if (idx >= s->total)
		panic("trying free out-bounded block at %p in slab", addr);

	if (bitmap_get(&s->bm, idx) == 0) {
		warnf("double free %p in a slab", addr);
		return;
	}

	if (s->free == s->total)
		panic("meta data error in slab");

	if (s->free == 0) {
		list_move(&s->list, &s->kmem->slabs_partial);
	}
	bitmap_clear(&s->bm, idx);
	s->free++;
	slab_log("%s freed %p", s->kmem->name, addr);
}

int kmem_cache_init(struct kmem_cache *kmem, const char *name, size_t obj_size,
		    bool with_initial_alloc)
{
	if (unlikely(!kmem))
		panic("null pointer in %s", __func__);
	kmem->alloc_size = ALIGN_UP(obj_size, 8);
	kmem->obj_size = obj_size;
	kmem->total = 0;
	kmem->free = 0;
	kmem->low = object_num(kmem->alloc_size);
	kmem->high = 2 * kmem->low;
	kmem->name = name;
	kmem->expanding = false;
	INIT_LIST_HEAD(&kmem->slabs_free);
	INIT_LIST_HEAD(&kmem->slabs_full);
	INIT_LIST_HEAD(&kmem->slabs_partial);
	kmem->lock = SPINLOCK_INITIALIZER("kmemcache");
	if (!with_initial_alloc)
		return 0;
	if (!buddy_inited)
		panic("trying create slab without buddy inited");
	struct slab *s = buddy_alloc(SLAB_BLOB_SIZE);
	if (!s)
		return -ENOMEM;
	slab_log("%s: expanded at %p", kmem->name, s);
	slab_init(kmem, s);
	kmem->total += s->total;
	kmem->free += s->free;
	return 0;
}

static void *kmem_cache_alloc_nolock(struct kmem_cache *kmem)
{
	struct slab *slab;
	// 检查余量是否充足
	if (!kmem->expanding && kmem->free < kmem->low) {
		// 检查free链表中是否有还未回收的slab
		if (!list_empty(&kmem->slabs_free)) {
			// 存在未回收slab,直接移入partial
			slab = list_first_entry(&kmem->slabs_free, struct slab,
						list);
			list_move(&slab->list, &kmem->slabs_partial);
			kmem->total += slab->total;
			kmem->free += slab->free;
		} else {
			// 不存在未回收slab,尝试从buddy分配
			kmem->expanding = true;
			slab = buddy_alloc(SLAB_BLOB_SIZE);
			kmem->expanding = false;
			if (!slab) {
				// buddy分配失败
				if (likely(kmem->free > 0)) {
					// 如果暂时还有可用object
					slab_log(
					    "%s free objects num dangerously "
					    "low",
					    kmem->name);
				} else {
					// OOM
					warnf("no avaliable object in %s",
					      kmem->name);
					return NULL;
				}
			} else {
				slab_log("%s: expanded at %p", kmem->name,
					 slab);
				slab_init(kmem, slab);
				kmem->total += slab->total;
				kmem->free += slab->free;
			}
		}
	}

	if (unlikely(kmem->free == 0)) {
		warnf("no avaliable object in %s", kmem->name);
		return NULL;
	}

	slab = list_first_entry(&kmem->slabs_partial, struct slab, list);
	void *mem = slab_alloc(slab);
	kmem->free--;
	return mem;
}

void *kmem_cache_alloc(struct kmem_cache *kmem)
{
	if (unlikely(!kmem))
		panic("null pointer in %s", __func__);
	void *mem = NULL;
	if (spinlock_holding(&kmem->lock)) {
		mem = kmem_cache_alloc_nolock(kmem);
	} else {
		spinlock_acquire(&kmem->lock);
		mem = kmem_cache_alloc_nolock(kmem);
		spinlock_release(&kmem->lock);
	}
	return mem;
}

// 上层确保地址合理，且对应page有PM_SLAB标志
void kmem_cache_free(void *addr)
{
	struct slab *slab = (void *)SLAB_ALIGN_DOWN(addr), *n;
	struct kmem_cache *kmem = slab->kmem;
	if (spinlock_holding(&kmem->lock)) {
		slab_free(addr);
		kmem->free++;
	} else {
		spinlock_acquire(&kmem->lock);
		slab_free(addr);
		kmem->free++;
		spinlock_release(&kmem->lock);
	}
}

void kmem_cache_flush(struct kmem_cache *kmem)
{
	if (unlikely(!kmem))
		panic("null pointer in %s", __func__);
	spinlock_acquire(&kmem->lock);
	if (kmem->free < kmem->high)
		return;
	struct slab *s, *n;
	list_for_each_entry_safe(s, n, &kmem->slabs_partial, list)
	{
		if (slab_empty(s))
			list_move(&s->list, &kmem->slabs_free);
	}
	spinlock_release(&kmem->lock);
}
