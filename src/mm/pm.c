#include <mm/pm.h>
#include <mm/layout.h>
#include <misc/stdint.h>
#include <misc/list.h>
#include <mm/early.h>
#include <mm/kalloc.h>
#include <mm/buddy.h>
#include <misc/log.h>
#include <misc/errno.h>

struct page pages[PAGE_NUM];

struct page_list {
	uintptr_t addr;
	struct list_head list;
};
static struct list_head page_free_list;
size_t single_free_pages = 0;
extern size_t buddy_free_pages;

static inline int addr2index(uintptr_t addr)
{
	return (addr >= PM_START && addr < PM_END) ?
		       (addr - PM_START) / PAGE_SIZE :
		       -1;
}

static inline uintptr_t index2addr(int index)
{
	return (index >= 0 && index < PAGE_NUM) ? PM_START + index * PAGE_SIZE :
						  (uintptr_t)NULL;
}

static int pm_pages_init()
{
	// 初始时所有页均为空闲
	for (int i = 0; i < PAGE_NUM; i++) {
		pages[i].refs = 0;
		pages[i].flags = 0;
	}

	// 设置SBI代码页面属性
	for (uintptr_t addr = SBI_BASE; addr < KERNEL_BASE; addr += PAGE_SIZE) {
		int index = addr2index(addr);
		if (index != -1) {
			pages[index].flags = PM_STATIC;
		} else {
			return -EINVAL;
		}
	}

	// 设置内核代码页面属性
	for (uintptr_t addr = KERNEL_BASE; addr < KERNEL_END;
	     addr += PAGE_SIZE) {
		int index = addr2index(addr);
		if (index != -1) {
			pages[index].flags = PM_STATIC;
		} else {
			return -EINVAL;
		}
	}

	// 设置early heap页面属性
	for (uintptr_t addr = EARLY_HEAP_BASE; addr < EARLY_HEAP_END;
	     addr += PAGE_SIZE) {
		int index = addr2index(addr);
		if (index != -1) {
			pages[index].flags = PM_STATIC;
		} else {
			return -EINVAL;
		}
	}

	// 设置buddy系统页面属性
	for (uintptr_t addr = BUDDY_SYSTEM_BASE; addr < BUDDY_SYSTEM_END;
	     addr += PAGE_SIZE) {
		int index = addr2index(addr);
		if (index != -1) {
			pages[index].flags = PM_STATIC | PM_BUDDY;
			pages[index].order =
				BUDDY_MAX_ORDER + 1; //表示这个页属于某个大块
			pages[index].private = NULL;
		} else {
			return -EINVAL;
		}
	}

	return 0;
}

void print_pm_layout()
{
	printf("\x1b[92mphysical memory layout:\n");
	printf("  SBI        [%p - %p]\n", SBI_BASE, KERNEL_BASE);
	printf("  KERNEL     [%p - %p]\n", KERNEL_BASE, KERNEL_END);
	printf("  EARLY HEAP [%p - %p]\n", EARLY_HEAP_BASE, EARLY_HEAP_END);
	printf("  BUDDY      [%p - %p]\n", BUDDY_SYSTEM_BASE, BUDDY_SYSTEM_END);
	printf("\x1b[0m");
}

static int collect_free_pages()
{
	// INIT_LIST_HEAD(&page_free_list);
	// // 遍历所有页面，将空闲页面添加到free_pages列表
	// for (size_t i = 0; i < PAGE_NUM; i++) {
	// 	if (pages[i].refs == 0 && !(pages[i].flags & PM_STATIC)) {
	// 		single_free_pages++;
	// 		struct page_list *new_page =
	// 			slab_alloc(sizeof(struct page_list));
	// 		if (new_page == NULL) {
	// 			return -ENOMEM;
	// 		}
	// 		new_page->addr = index2addr(i);
	// 		list_add(&new_page->list, &page_free_list);
	// 	}
	// }
	return 0;
}

int pm_init()
{
	int ret = pm_pages_init();
	if (ret)
		return ret;

	early_init();

	ret = buddy_init();
	if (ret)
		return ret;

	kalloc_init();

	ret = collect_free_pages();
	if (ret)
		return ret;

	infof("Free Pages: %zu, Usage: %u%%", get_free_pages_num(),
	      100 - get_free_pages_num() * 100ULL / PAGE_NUM);

	return 0;
}

struct page *addr2page(void *addr)
{
	int index = addr2index((uintptr_t)addr);
	if (index == -1) {
		return NULL;
	}
	return &pages[index];
}

void *page2addr(struct page *page)
{
	if (page == NULL || page < &pages[0] || page > &pages[PAGE_NUM - 1] ||
	    !ALIGNED(page, sizeof(struct page))) {
		return NULL;
	}
	int index =
		((uintptr_t)page - (uintptr_t)&pages[0]) / sizeof(struct page);
	return (void *)index2addr(index);
}

void *page_alloc(u8 flags)
{
	// if (list_empty(&page_free_list)) {
	// 	return NULL; // 没有空闲页面
	// }

	// struct page_list *page_entry =
	// 	list_first_entry(&page_free_list, struct page_list, list);
	// list_del(&page_entry->list);
	// uintptr_t addr = page_entry->addr;
	// slab_free(page_entry);

	// int index = addr2index(addr);
	// if (index == -1) {
	// 	return NULL;
	// }
	// pages[index].flags = flags;
	// single_free_pages--;

	// return (void *)addr;
	panic("calling unimplemented function");
}

int page_free(void *addr)
{
	// if (addr == NULL) {
	// 	return -EINVAL;
	// }

	// int index = addr2index((uintptr_t)addr);
	// if (index == -1 || pages[index].refs != 0 || pages[index].flags) {
	// 	return -EINVAL;
	// }
	// struct page_list *new_page = slab_alloc(sizeof(struct page_list));
	// if (new_page == NULL) {
	// 	return -ENOMEM; // 内存分配失败
	// }
	// single_free_pages++;
	// pages[index].flags = 0;
	// new_page->addr = (uintptr_t)addr;
	// list_add(&new_page->list, &page_free_list); // 将页面重新加入空闲列表
	// return 0;
	panic("calling unimplemented function");
}

size_t get_free_pages_num()
{
	return buddy_free_pages + single_free_pages;
}
