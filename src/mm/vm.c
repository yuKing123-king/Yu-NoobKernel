#include <misc/log.h>
#include <mm/pagetable.h>
#include <mm/vm.h>

extern char s_text[];
extern char e_text[];
extern char s_rodata[];
extern char e_rodata[];
extern char s_data[];
extern char e_data[];
extern char s_bss[];
extern char e_bss[];
extern char ekernel[];
extern char trampoline[];

pagetable_t kpagetable;

uintptr_t va2pa(pagetable_t pagetable, uintptr_t va)
{
	uintptr_t page = walkaddr(pagetable, va);
	if (page == 0)
		return 0;
	return page | (va & 0xFFFULL);
}

static inline int kmappages(pagetable_t pagetable, uintptr_t va, uintptr_t pa,
			    size_t npages, int perm)
{
	return mappages(pagetable, va, pa, npages, (perm & ~PTE_U) | PTE_V);
}

static inline void devminit()
{
	if (kmappages(kpagetable, 0, 0, PM_START / PAGE_SIZE, PTE_R | PTE_W)) {
		panic("mapping device memory");
	}
}

void kvminit()
{
	kpagetable = pagetable_create();

	// 设备地址空间
	devminit();
	
	// 内核text段
	if (kmappages(kpagetable, KVM(s_text), (uintptr_t)s_text,
		      ((uintptr_t)e_text - (uintptr_t)s_text) / PAGE_SIZE,
		      PTE_R | PTE_X)) {
		panic("mapping kernel text seg");
	}

	// 内核rodata段
	if (kmappages(kpagetable, KVM(s_rodata), (uintptr_t)s_rodata,
		      ((uintptr_t)e_rodata - (uintptr_t)s_rodata) / PAGE_SIZE,
		      PTE_R)) {
		panic("mapping kernel rodata seg");
	}

	// 内核data&bss段
	if (kmappages(kpagetable, KVM(s_data), (uintptr_t)s_data,
		      ((uintptr_t)e_bss - (uintptr_t)s_data) / PAGE_SIZE,
		      PTE_R | PTE_W)) {
		panic("mapping kernel data&bss seg");
	}

	// 跳板页
	if (kmappages(kpagetable, TRAMPOLINE, (uintptr_t)trampoline, 1,
		      PTE_R | PTE_X | PTE_G)) {
		panic("mapping kernel trampoline");
	}

	// 剩余内存
	if (kmappages(kpagetable, KVM(ekernel), (uintptr_t)ekernel,
		      (PM_END - (uintptr_t)ekernel) / PAGE_SIZE, PTE_R | PTE_W)) {
		panic("mapping remeaing memory");
	}

	w_satp(MAKE_SATP(kpagetable));
	sfence_vma();
	infof("enable pageing at %p", r_satp());
}
