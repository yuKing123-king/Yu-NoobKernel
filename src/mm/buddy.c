#include <mm/buddy.h>
#include <misc/list.h>
#include <misc/math.h>
#include <mm/kalloc.h>
#include <mm/early.h>
#include <misc/errno.h>
#include <mm/pm.h>
#include <mm/layout.h>
#include <misc/log.h>
#include <misc/complier.h>
#include <sync/spinlock.h>

extern bool kalloc_inited;

bool buddy_inited = false;
size_t buddy_free_pages = 0;

#if 0
#define buddy_log(fmt, ...) infof("buddy: " fmt, ##__VA_ARGS__)
#else
#define buddy_log(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif

struct mem_block {
	void *start;
	struct list_head list;
};

static struct list_head buddy_free_list[BUDDY_MAX_ORDER + 1];
static spinlock_t lock = SPINLOCK_INITIALIZER("buddy");
/*
 * 将内存大小转换为对应的order值（2^order * PAGE_SIZE）
 * @param size: 内存大小（字节）
 * @return: 对应的order值
 */
static inline u8 size2order(size_t size)
{
	return (u8)(log2_ceil(size) - PAGE_SHIFT);
}

/*
 * 计算给定地址的buddy块的地址
 * @param addr: 内存块起始地址
 * @param order: 块的大小阶数
 * @return: buddy块的起始地址
 */
static inline void *get_buddy_addr(void *addr, u8 order)
{
	return (void *)((uintptr_t)addr ^ (PAGE_SIZE << order));
}

/*
 * 分配一个mem_block元数据结构体（优先使用kalloc，fallback到early堆）
 * @return: 新分配的mem_block指针
 */
static struct mem_block *alloc_mem_block()
{
	if (likely(kalloc_inited)) {
		return kmalloc(sizeof(struct mem_block));
	}
	return early_alloc(sizeof(struct mem_block));
}

/*
 * 释放mem_block元数据结构体（early堆内存不做释放）
 * @param ptr: 待释放的mem_block指针
 * @return: 无返回值
 */
static void free_mem_block(struct mem_block *ptr)
{
	if (unlikely(is_early_mem(ptr))) {
		(void)ptr;
		return;
	}
	kfree(ptr);
}

/*
 * 初始化buddy系统：初始化空闲链表，将BUDDY_SYSTEM区域划分为最大order的块
 * @return: 成功返回0，失败返回负错误码
 */
int buddy_init()
{
	for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
		INIT_LIST_HEAD(&buddy_free_list[i]);
	}
	for (uintptr_t p = BUDDY_SYSTEM_BASE; p < BUDDY_SYSTEM_END;
	     p += BUDDY_BLOB_SIZE) {
		struct page *pg = addr2page((void *)p);
		if (pg == NULL) {
			return -EINVAL;
		}
		struct mem_block *t = alloc_mem_block();
		if (t == NULL)
			panic("no enough early heap memory for buddy init");
		t->start = (void *)p;
		pg->private = t;
		pg->order = BUDDY_MAX_ORDER;
		buddy_free_pages += (1 << BUDDY_MAX_ORDER);
		list_add_tail(&t->list, &buddy_free_list[BUDDY_MAX_ORDER]);
	}
	buddy_inited = true;
	return 0;
};

/*
 * 合并低阶空闲块为高阶块，直到目标order级别
 * @param order: 目标合并到的order值
 * @return: 无返回值
 */
static void merge_blocks(u8 order)
{
	struct list_head *p = NULL, *n;
	for (u8 i = 0; i < order; i++) {
		if (list_empty(&buddy_free_list[i]))
			continue;

		// 这一层时间复杂度大概为O(nlogn)，可以优化成O(n)
		while (p != &buddy_free_list[i]) {
			struct mem_block *block, *buddy_block = NULL;
			struct page *page, *buddy_page;
			list_for_each_safe(p, n, &buddy_free_list[i])
			{
				block = list_entry(p, struct mem_block, list);
				page = addr2page(block->start);
				buddy_page = addr2page(
				    get_buddy_addr(block->start, page->order));
				if (buddy_page->private == NULL ||
				    buddy_page->order != i)
					continue;
				buddy_block = buddy_page->private;
				break;
			}
			if (buddy_block != NULL) {
				list_del(&block->list);
				list_del(&buddy_block->list);
				if ((uintptr_t)block->start >
				    (uintptr_t)buddy_block->start) {
					free_mem_block(block);
					page->private = NULL;
					page->order = BUDDY_MAX_ORDER + 1;
					buddy_page->order++;
					list_add(&buddy_block->list,
						 &buddy_free_list[buddy_page
								      ->order]);
				} else {
					free_mem_block(buddy_block);
					buddy_page->private = NULL;
					buddy_page->order = BUDDY_MAX_ORDER + 1;
					page->order++;
					list_add(&block->list,
						 &buddy_free_list[page->order]);
				}
			}
		}
	}
}

/*
 * 从高阶空闲块中分裂出目标order大小的块
 * @param target_order: 目标分裂到的order值
 * @return: 无返回值
 */
static void split_blocks(u8 target_order)
{
	u8 source_order = target_order + 1;
	while (source_order <= BUDDY_MAX_ORDER &&
	       list_empty(&buddy_free_list[source_order])) {
		source_order++;
	}

	if (source_order > BUDDY_MAX_ORDER) {
		return;
	}

	for (u8 order = source_order; order > target_order; order--) {
		struct mem_block *block = list_first_entry(
		    &buddy_free_list[order], struct mem_block, list);
		struct page *page = addr2page(block->start);

		if (page->order != order) {
			panic("split_blocks: page->order=%u != list order=%u",
			      page->order, order);
		}

		list_del(&block->list);

		u8 new_order = order - 1;

		struct mem_block *buddy_block = alloc_mem_block();
		if (!buddy_block) {
			panic("buddy metadata OOM in split_blocks");
		}

		page->order = new_order;

		buddy_block->start = get_buddy_addr(block->start, new_order);
		struct page *buddy_page = addr2page(buddy_block->start);
		buddy_page->order = new_order;
		buddy_page->private = buddy_block;

		list_add_tail(&block->list, &buddy_free_list[new_order]);
		list_add_tail(&buddy_block->list, &buddy_free_list[new_order]);
	}

	return;
}

/*
 * 从指定的order空闲链表中分配一个内存块（不加锁，由调用者保证同步）
 * @param order: 分配块的order值
 * @return: 分配的内存块地址
 */
static void *buddy_alloc_inner(u8 order)
{
	void *addr = NULL;
	struct mem_block *block =
	    list_first_entry(&buddy_free_list[order], struct mem_block, list);
	struct page *page = addr2page(block->start);
	if (page->order != order)
		panic("buddy order broken, expected: %u, actual: %u", order,
		      page->order);

	page->private =
	    NULL; // 这里把private设置为空表示此内存已不属于buddy管理
	addr = block->start;
	list_del(&block->list);
	free_mem_block(block);
	buddy_free_pages -= (1 << order);
	buddy_log("allocated %p - %p", addr, addr + (PAGE_SIZE << order));
	return addr;
}

/*
 * 分配指定大小的内存块（不加锁），需要时自动合并或分裂块
 * @param size: 需要分配的内存大小（字节）
 * @return: 分配的内存地址，失败返回NULL
 */
static void *buddy_alloc_nolock(size_t size)
{
	u8 order = size2order(size);
	if (!list_empty(&buddy_free_list[order])) {
		return buddy_alloc_inner(order);
	}
	debugf("no blocks avaliable, merging");
	merge_blocks(order);
	if (!list_empty(&buddy_free_list[order])) {
		return buddy_alloc_inner(order);
	}
	debugf("no blocks avaliable, spliting");
	split_blocks(order);
	if (!list_empty(&buddy_free_list[order])) {
		return buddy_alloc_inner(order);
	}
	return NULL;
}

/*
 * 分配指定大小的内存块（对外接口，带锁保护）
 * @param size: 需要分配的内存大小（字节），需在[PAGE_SIZE, BUDDY_BLOB_SIZE]范围内
 * @return: 分配的内存地址，失败返回NULL
 */
void *buddy_alloc(size_t size)
{
	if (size < PAGE_SIZE || size > BUDDY_BLOB_SIZE) {
		errorf("buddy_alloc: invalid size %u", size);
		return NULL;
	}
	buddy_log("allocating size: %zu", size);
	if (spinlock_holding(&lock))
		return buddy_alloc_nolock(size);
	spinlock_acquire(&lock);
	void *addr = buddy_alloc_nolock(size);
	spinlock_release(&lock);
	return addr;
}

/*
 * 释放由buddy_alloc分配的内存块，将其归还到对应order的空闲链表
 * @param addr: 待释放的内存块地址
 * @return: 无返回值
 */
void buddy_free(void *addr)
{
	struct page *page = addr2page(addr);
	struct mem_block *block = alloc_mem_block();
	if (block == NULL)
		panic("OOM during buddy free");

	buddy_log("freed %p - %p", addr, addr + (PAGE_SIZE << page->order));
	block->start = addr;
	page->private = block;
	spinlock_acquire(&lock);
	buddy_free_pages += (1 << page->order);
	list_add(&block->list, &buddy_free_list[page->order]);
	spinlock_release(&lock);
}

/*
 * 打印指定order的空闲链表中的块地址
 * @param order: 需要打印的order值
 * @return: 无返回值
 */
void buddy_print_free_list(u8 order)
{
	const int colors[] = {92, 95, 96};
	const int n = ARRAY_SIZE(colors);
	if (order > BUDDY_MAX_ORDER)
		return;
	printf("\x1b[%dmfree_list[%u]:", order % n, order);
	if (list_empty(&buddy_free_list[order])) {
		printf(" empty\x1b[0m\n");
		return;
	}
	struct list_head *p;
	int i = 0;
	list_for_each(p, &buddy_free_list[order])
	{
		if (i % 8 == 0)
			printf("\n");
		printf("%p ", list_entry(p, struct mem_block, list)->start);
		i++;
	}
	printf("\x1b[0m\n");
}

/*
 * buddy系统测试函数：循环测试各order的分配和释放功能
 * @return: 无返回值
 */
void buddy_test(void)
{
	for (int order = 0; order < 12; order++) {
		void *addr[20];
		for (int i = 0; i < 20; i++) {
			addr[i] = buddy_alloc(PAGE_SIZE << order);
			if (addr[i] == NULL) {
				infof(
				    "%d: Failed to allocate block of order %d",
				    i, order);
			} else {
				infof("%d: Allocated block of order %d at "
				      "address %p",
				      i, order, addr[i]);
			}
		}
		for (int i = 0; i < 20; i++) {
			if (addr[i] != NULL) {
				buddy_free(addr[i]);
				infof("%d: Freed block at address %p", i,
				      addr[i]);
			}
		}
	}
}
