#include <misc/stddef.h>
#include <mm/layout.h>
#include <misc/log.h>
#include <misc/complier.h>

extern bool kalloc_inited;
static uintptr_t early_brk;

/*
 * 初始化早期堆分配器，将分配起始位置设置为EARLY_HEAP_BASE
 * @return: 无返回值
 */
void early_init(){
	early_brk = EARLY_HEAP_BASE;
}

/*
 * 判断给定的指针是否属于早期堆内存范围
 * @param ptr: 待检查的指针
 * @return: 如果指针在早期堆范围内返回true，否则返回false
 */
bool is_early_mem(void *ptr){
	return ((uintptr_t)ptr >= EARLY_HEAP_BASE) && (uintptr_t)ptr < early_brk;
}

/*
 * 从早期堆中分配内存（仅限buddy系统初始化前使用）
 * @param size: 分配的字节数（自动8字节对齐）
 * @return: 分配的内存起始地址
 */
void *early_alloc(size_t size){
	if(unlikely(kalloc_inited))
		panic("early heap not usable after buddy inited");
	size = ALIGN_UP(size, 8);
	if(unlikely(early_brk + size >= EARLY_HEAP_END))
		panic("early heap too small");
	void *ptr = (void *)early_brk;
	early_brk += size;
	return ptr;
}
