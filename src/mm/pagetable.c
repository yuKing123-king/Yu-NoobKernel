#include <misc/errno.h>
#include <misc/log.h>
#include <misc/string.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/vm.h>
#include <misc/align.h>

/*
 * 根据虚拟地址在页表中查找对应的页表项，可选择是否自动分配中间页表
 * @param pagetable: 根页表指针
 * @param va: 虚拟地址
 * @param alloc: 是否在页表不存在时自动分配中间页表
 * @return: 页表项指针，未找到且不分配时返回NULL
 */
pte_t *va2pte(pagetable_t pagetable, uintptr_t va, bool alloc)
{
	for (int level = 2; level > 0; level--) {
		pte_t *pte = &pagetable[PX(level, va)];
		if (*pte & PTE_V) {
			// 如果已有下一级页表，则进入下一级寻找
			pagetable = (pagetable_t)PTE2PA(*pte);
		} else {
			// 如果下一级页表没创建且：
			//    需要创建：分配一页内存存放下一级页表
			//    无需创建：直接返回NULL表示未找到
			if (!alloc || (pagetable = kzalloc(PAGE_SIZE)) == NULL)
				return NULL;
			*pte = PA2PTE(pagetable) | PTE_V;
		}
	}
	return &pagetable[PX(0, va)];
}

/*
 * 通过页表将虚拟地址转换为物理地址（页对齐部分）
 * @param pagetable: 根页表指针
 * @param va: 虚拟地址
 * @return: 对应的物理地址（页对齐），转换失败返回0
 */
uintptr_t walkaddr(pagetable_t pagetable, uintptr_t va)
{
	pte_t *pte;
	uintptr_t pa;

	if (va >= VM_END)
		return 0;

	pte = va2pte(pagetable, va, false);
	if (pte == 0)
		return 0;
	if ((*pte & PTE_M) == 0)
		return 0;
	pa = PTE2PA(*pte);
	return pa;
}

/*
 * 创建一个新的根页表（分配一页并清零）
 * @return: 新页表的指针
 */
pagetable_t pagetable_create() { return kzalloc(PAGE_SIZE); }

/*
 * 递归销毁指定级别的页表及其所有子页表
 * @param pagetable: 当前级别页表指针
 * @param level: 页表级别（0为叶子页表）
 * @return: 无返回值
 */
static void pagetable_destroy_level(pagetable_t pagetable, int level)
{
	if (pagetable == NULL)
		return;

	// 如果不是叶子页表（level > 0），递归释放子页表
	if (level > 0) {
		for (int i = 0; i < 512; i++) {
			pte_t pte = pagetable[i];
			if (pte & PTE_V) {
				// 检查是否为大页？在 Sv39 中，level 1/2
				// 不能是大页（除非扩展），我们假设全是 4KB 页
				// 所以只要 level > 0 且
				// PTE_V，就一定是下一级页表
				pagetable_t child = (pagetable_t)PTE2PA(pte);
				pagetable_destroy_level(child, level - 1);
			}
		}
	}

	// 释放当前页表页（注意：pagetable 是虚拟地址，需能被 kfree 释放）
	kfree(pagetable);
}

/*
 * 销毁整个页表树（从根页表开始递归释放）
 * @param pagetable: 根页表指针
 * @return: 无返回值
 */
void pagetable_destroy(pagetable_t pagetable)
{
	if (pagetable == NULL)
		return;
	pagetable_destroy_level(pagetable, 2); // Sv39 根页表是 level 2
}

/*
 * 将连续的虚拟地址区间映射到物理地址区间
 * @param pagetable: 根页表指针
 * @param va: 起始虚拟地址（需页对齐）
 * @param pa: 起始物理地址（需页对齐）
 * @param npages: 映射的页数
 * @param perm: 页权限标志
 * @return: 成功返回0，失败返回负错误码
 */
int mappages(pagetable_t pagetable, uintptr_t va, uintptr_t pa, size_t npages,
	     int perm)
{
	if (pagetable == NULL) {
		warnf("mappages: NULL pagetable");
		return -EINVAL;
	}
	if (!PAGE_ALIGNED(va) || !PAGE_ALIGNED(pa)) {
		warnf("mappages: unaligned va=%p pa=%p", va, pa);
		return -EINVAL;
	}
	if (npages == 0)
		return 0;
	if (va + npages * PAGE_SIZE > VM_END) {
		warnf("mappages: va=%p out of range", va);
		return -ERANGE;
	}
	if (pa + npages * PAGE_SIZE > PM_END) {
		warnf("mappages: pa=%p out of range", pa);
		return -ERANGE;
	}
	pte_t *pte;
	perm &= 0x3f;
	for (size_t i = 0; i < npages; i++) {
		pte = va2pte(pagetable, va + i * PAGE_SIZE, true);
		if (!pte) {
			warnf("mappages: out of memory at va=%p",
			      va + i * PAGE_SIZE);
			return -ENOMEM;
		}
		if (*pte & PTE_M) {
			warnf("mappages: va=%p already mapped", va + i * PAGE_SIZE);
			return -EADDRINUSE;
		}
		*pte = PA2PTE(pa + i * PAGE_SIZE) | perm | PTE_M;
	}
	return 0;
}

/*
 * 解除虚拟地址区间的页表映射
 * @param pagetable: 根页表指针
 * @param vm: 起始虚拟地址
 * @param npages: 解除映射的页数
 * @return: 成功返回0
 */
int unmappages(pagetable_t pagetable, uintptr_t vm, size_t npages)
{
	pte_t *pte;
	uintptr_t start = PAGE_ALIGN_DOWN(vm);
	uintptr_t end = start + npages * PAGE_SIZE;
	for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
		if ((pte = va2pte(pagetable, a, false)) == NULL)
			continue;
		if(*pte & PTE_M){
			*pte = 0;
		}else{
			debugf("trying unmap not mapped page");
		}
	}
	return 0;
}
