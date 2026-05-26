#include <misc/stddef.h>
#include <mm/layout.h>
#include <misc/log.h>
#include <misc/complier.h>

extern bool kalloc_inited;
static uintptr_t early_brk;

void early_init(){
	early_brk = EARLY_HEAP_BASE;
}

bool is_early_mem(void *ptr){
	return ((uintptr_t)ptr >= EARLY_HEAP_BASE) && (uintptr_t)ptr < early_brk;
}

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
