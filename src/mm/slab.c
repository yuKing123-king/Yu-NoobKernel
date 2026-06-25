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

/*
 * 计算一个slab块中能容纳的对象数量
 * @param obj_size: 每个对象的大小（字节）
 * @return: 对象数量
 */
static u32 object_num(u32 obj_size)
{
	size_t D = 8 * obj_size + 1;
	size_t N = 8 * (SLAB_BLOB_SIZE - sizeof(struct slab)) - 7;
	return (u32)(N / D);
}

/*
 * 判断slab是否全部空闲（无已分配对象）
 * @param s: slab结构体指针
 * @return: 全部空闲返回true，否则返回false
 */
static inline bool slab_empty(struct slab *s) { return (s->total == s->free); }

// 上层确保alloc_size对指令宽度对齐
/*
 * 初始化一个slab块：设置页面标志、元数据、位图和空闲链表
 * @param kmem: 所属的kmem_cache缓存
 * @param blob: slub块的起始地址
 * @return: 无返回值
 */
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
	while (offset + total * kmem->alloc_size > SLAB_BLOB_SIZE) {
		total--;
		bitmap_size = ALIGN_UP(total, 8) / 8;
		offset = ALIGN_UP(sizeof(struct slab) + bitmap_size,
				  kmem->obj_size);
	}
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

/*
 * 销毁一个slab块：清除页面标志、从链表移除并归还给buddy系统
 * @param s: 待销毁的slab结构体指针
 * @return: 无返回值
 */
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

/*
 * 从指定slab中分配一个对象
 * @param s: slab结构体指针
 * @return: 分配的对象地址
 */
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
	void *mem = (void *)((uintptr_t)s + s->offset + idx * s->kmem->alloc_size);
	return mem;
}

/*
 * 释放slab中的对象，更新位图和空闲计数（调用者需确保addr属于slab内存）
 * @param addr: 待释放的对象地址
 * @return: 无返回值
 */
static bool slab_free(void *addr)
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
		void *ra0 = __builtin_return_address(0);
		warnf("double free %p in slab '%s' ra0=%p",
		      addr, s->kmem->name, ra0);
		return false;
	}

	if (s->free == s->total)
		panic("meta data error in slab");

	if (s->free == 0) {
		list_move(&s->list, &s->kmem->slabs_partial);
	}
	bitmap_clear(&s->bm, idx);
	s->free++;
	slab_log("%s freed %p", s->kmem->name, addr);
	return true;
}

/*
 * 初始化kmem_cache缓存，可选择是否立即分配首个slab块
 * @param kmem: 待初始化的kmem_cache指针
 * @param name: 缓存名称
 * @param obj_size: 对象大小（字节）
 * @param with_initial_alloc: 是否立即从buddy系统中分配初始slab块
 * @return: 成功返回0，失败返回负错误码
 */
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

/*
 * 从kmem_cache中分配一个对象（不加锁，由调用者保证同步）
 * @param kmem: kmem_cache指针
 * @return: 分配的对象地址，空闲不足时返回NULL
 */
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

/*
 * 从kmem_cache中分配一个对象（对外接口，带锁保护）
 * @param kmem: kmem_cache指针
 * @return: 分配的对象地址，失败返回NULL
 */
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

/*
 * 释放kmem_cache中的对象（调用者需确保地址有效并属于slab内存）
 * @param addr: 待释放的对象地址
 * @return: 无返回值
 */
void kmem_cache_free(void *addr)
{
	struct slab *slab = (void *)SLAB_ALIGN_DOWN(addr);
	struct kmem_cache *kmem = slab->kmem;
	if (spinlock_holding(&kmem->lock)) {
		if (slab_free(addr))
			kmem->free++;
	} else {
		spinlock_acquire(&kmem->lock);
		if (slab_free(addr))
			kmem->free++;
		spinlock_release(&kmem->lock);
	}
}

/*
 * 回收kmem_cache中空闲的slab块，将完全空闲的slab移入空闲链表
 * @param kmem: kmem_cache指针
 * @return: 无返回值
 */
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
