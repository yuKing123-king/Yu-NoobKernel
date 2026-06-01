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

/*
 * 将虚拟地址转换为物理地址（保留页内偏移）
 * @param pagetable: 根页表指针
 * @param va: 虚拟地址
 * @return: 对应的物理地址，转换失败返回0
 */
uintptr_t va2pa(pagetable_t pagetable, uintptr_t va)
{
	uintptr_t page = walkaddr(pagetable, va);
	if (page == 0)
		return 0;
	return page | (va & 0xFFFULL);
}

/*
 * 内核页表映射接口（自动添加PTE_V标志，移除PTE_U标志）
 * @param pagetable: 根页表指针
 * @param va: 起始虚拟地址
 * @param pa: 起始物理地址
 * @param npages: 映射的页数
 * @param perm: 页权限标志
 * @return: 成功返回0，失败返回负错误码
 */
static inline int kmappages(pagetable_t pagetable, uintptr_t va, uintptr_t pa,
			    size_t npages, int perm)
{
	return mappages(pagetable, va, pa, npages, (perm & ~PTE_U) | PTE_V);
}

/*
 * 初始化设备地址空间映射（将物理地址0~PM_START映射到内核页表）
 * @return: 无返回值
 */
static inline void devminit()
{
	if (kmappages(kpagetable, 0, 0, PM_START / PAGE_SIZE, PTE_R | PTE_W)) {
		panic("mapping device memory");
	}
}

/*
 * 初始化内核虚拟内存映射：创建设备映射、内核各段映射、跳板页和剩余内存，并启用分页
 * @return: 无返回值
 */
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
