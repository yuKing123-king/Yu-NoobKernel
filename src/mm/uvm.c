#include <misc/errno.h>
#include <misc/log.h>
#include <misc/string.h>
#include <misc/align.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/vm.h>

#include <task/proc.h>

extern pagetable_t kpagetable;

/**
 * @brief 创建一个空的用户页表（仅分配根页表页）
 * @param 无
 * @return 成功返回页表指针，失败返回 NULL
 */
pagetable_t uvmcreate(void)
{
	pagetable_t pt = pagetable_create();
	if (pt == NULL)
		return NULL;

	return pt;
}

/**
 * @brief 在用户页表中分配连续的物理页并建立映射
 *        仅在 oldsz 到 newsz 之间的新页上分配物理页
 * @param pt 用户页表
 * @param oldsz 原虚拟地址上限（页对齐）
 * @param newsz 新虚拟地址上限
 * @return 成功返回 newsz，失败返回 0
 */
uintptr_t uvmalloc(pagetable_t pt, uintptr_t oldsz, uintptr_t newsz)
{
	uintptr_t a;
	uintptr_t pa;

	if (newsz > USER_TOP || oldsz > newsz)
		return 0;

	oldsz = PAGE_ALIGN_UP(oldsz);
	newsz = PAGE_ALIGN_UP(newsz);

	for (a = oldsz; a < newsz; a += PAGE_SIZE) {
		pa = (uintptr_t)kzalloc(PAGE_SIZE);
		if (pa == 0)
			goto err;
		if (mappages(pt, a, pa, 1, PTE_R | PTE_W | PTE_X | PTE_U) != 0) {
			kfree((void *)pa);
			goto err;
		}
	}
	return newsz;

err:
	uvmdealloc(pt, a, oldsz);
	return 0;
}

/**
 * @brief 解除 [newsz, oldsz) 范围的映射并释放物理页
 * @param pt 用户页表
 * @param oldsz 原虚拟地址上限
 * @param newsz 新虚拟地址上限（更小）
 * @return 返回 newsz
 */
uintptr_t uvmdealloc(pagetable_t pt, uintptr_t oldsz, uintptr_t newsz)
{
	uintptr_t a;
	uintptr_t pa;

	if (newsz >= oldsz)
		return oldsz;

	oldsz = PAGE_ALIGN_UP(oldsz);
	newsz = PAGE_ALIGN_UP(newsz);

	for (a = newsz; a < oldsz; a += PAGE_SIZE) {
		pa = walkaddr(pt, a);
		if (pa == 0)
			continue;
		unmappages(pt, a, 1);
		kfree((void *)pa);
	}
	return newsz;
}

/**
 * @brief 释放整个用户地址空间：解除所有映射，释放物理页，销毁页表
 * @param pt 用户页表
 * @param sz 用户地址空间大小（需映射的最大 VA）
 * @return 无返回值
 */
void uvmfree(pagetable_t pt, uintptr_t sz)
{
	if (pt == NULL || pt == kpagetable)
		return;

	uvmdealloc(pt, sz, 0);
	pagetable_destroy(pt);
}

/**
 * @brief 复制用户页表（用于 fork）：逐页复制映射和物理页内容
 * @param src 源用户页表
 * @param dst 目标用户页表
 * @param sz 需要复制的地址空间大小
 * @return 成功返回 0，失败返回 -1
 */
int uvmcopy(pagetable_t src, pagetable_t dst, uintptr_t sz)
{
	uintptr_t pa;
	uintptr_t va;
	pte_t *pte;
	void *mem;

	for (va = 0; va < sz; va += PAGE_SIZE) {
		if (va == TRAMPOLINE || va == TRAPFRAME)
			continue;
		pte = va2pte(src, va, false);
		if (pte == NULL || (*pte & PTE_V) == 0)
			continue;
		pa = PTE2PA(*pte);
		mem = kzalloc(PAGE_SIZE);
		if (mem == NULL)
			goto err;
		memcpy(mem, (void *)pa, PAGE_SIZE);
		if (mappages(dst, va, (uintptr_t)mem, 1,
			     PTE_FLAGS(*pte)) != 0) {
			kfree(mem);
			goto err;
		}
	}
	return 0;

err:
	uvmfree(dst, sz);
	return -1;
}

/**
 * @brief 将数据从用户空间复制到内核空间
 * @param pt 用户页表
 * @param dst 内核目标缓冲区
 * @param srcva 用户空间源地址
 * @param len 复制长度
 * @return 成功返回 0，失败返回 -1
 */
int copyin(pagetable_t pt, char *dst, uintptr_t srcva, size_t len)
{
	uintptr_t pa;
	size_t n;
	size_t offset;

	while (len > 0) {
		if (srcva >= USER_TOP)
			return -1;
		pa = walkaddr(pt, srcva);
		if (pa == 0)
			return -1;
		offset = srcva & (PAGE_SIZE - 1);
		n = PAGE_SIZE - offset;
		if (n > len)
			n = len;
		memcpy(dst, (void *)(pa + offset), n);
		dst += n;
		srcva += n;
		len -= n;
	}
	return 0;
}

/**
 * @brief 将数据从内核空间复制到用户空间
 * @param pt 用户页表
 * @param dstva 用户空间目标地址
 * @param src 内核源缓冲区
 * @param len 复制长度
 * @return 成功返回 0，失败返回 -1
 */
int copyout(pagetable_t pt, uintptr_t dstva, char *src, size_t len)
{
	uintptr_t pa;
	size_t n;
	size_t offset;

	while (len > 0) {
		if (dstva >= USER_TOP)
			return -1;
		pa = walkaddr(pt, dstva);
		if (pa == 0)
			return -1;
		offset = dstva & (PAGE_SIZE - 1);
		n = PAGE_SIZE - offset;
		if (n > len)
			n = len;
		memcpy((void *)(pa + offset), src, n);
		src += n;
		dstva += n;
		len -= n;
	}
	return 0;
}

/**
 * @brief 从用户空间复制一个以 null 结尾的字符串到内核缓冲区
 * @param pt 用户页表
 * @param dst 内核目标缓冲区
 * @param srcva 用户空间源地址
 * @param max 最大复制长度（含结尾 null）
 * @return 成功返回字符串长度，失败返回 -1
 */
int copyinstr(pagetable_t pt, char *dst, uintptr_t srcva, size_t max)
{
	uintptr_t pa;
	size_t offset;
	size_t n;
	int got_null = 0;
	char *start = dst;
	
	while (max > 0 && got_null == 0) {
		if (srcva >= USER_TOP)
			return -1;
		pa = walkaddr(pt, srcva);
		if (pa == 0)
			return -1;
		offset = srcva & (PAGE_SIZE - 1);
		n = PAGE_SIZE - offset;
		if (n > max)
			n = max;
		for (size_t i = 0; i < n; i++) {
			char c = *((char *)pa + offset + i);
			*dst++ = c;
			max--;
			if (c == '\0') {
				got_null = 1;
				break;
			}
		}
		srcva += n;
	}

	if (got_null)
		return dst - start - 1;
	return -1;
}

/* ============================================================
 * 页表树遍历：释放所有用户物理页 + 逐页复制（用于 execve/fork）
 *
 * 替代原来的线性扫描 [0, USER_TOP)，改为递归遍历页表树。
 * 复杂度从 O(2GB/PAGE_SIZE) 降为 O(已映射页数 + 中间页表页数)。
 *
 * RISC-V Sv39 规范：叶 PTE 至少有一个 R/W/X 位，
 * 非叶 PTE 只有 V 位（指向下一级页表页）。
 * ============================================================ */

static void uvm_free_user_pages_level(pagetable_t pt, int level, uintptr_t base)
{
	for (int i = 0; i < 512; i++) {
		pte_t pte = pt[i];
		if (!(pte & PTE_V))          /* <项目内: src/mm/pagetable.h> 跳过无效项 */
			continue;
		uintptr_t va = base | ((uintptr_t)i << PXSHIFT(level));
		if (pte & (PTE_R | PTE_W | PTE_X)) {
			/* 叶 PTE：释放物理页，跳过 trampoline/trapframe */
			if (va != TRAMPOLINE && va != TRAPFRAME) {
				kfree((void *)PTE2PA(pte));   /* <项目内: src/mm/kalloc.c> */
				pt[i] = 0;                     /* 清除 PTE 避免悬垂指针 */
			}
		} else if (level > 0) {
			/* 非叶 PTE：递归进入下一级 */
			uvm_free_user_pages_level((pagetable_t)PTE2PA(pte), level - 1, va);
		}
	}
}

void uvm_free_user_pages(pagetable_t pt)
{
	if (!pt || pt == kpagetable)
		return;
	uvm_free_user_pages_level(pt, 2, 0);
}

static int uvmcopy_tree_level(pagetable_t src, pagetable_t dst,
			       int level, uintptr_t base)
{
	for (int i = 0; i < 512; i++) {
		pte_t pte = src[i];
		if (!(pte & PTE_V))
			continue;
		uintptr_t va = base | ((uintptr_t)i << PXSHIFT(level));
		if (pte & (PTE_R | PTE_W | PTE_X)) {
			/* 叶 PTE：复制物理页到目标页表 */
			if (va == TRAMPOLINE || va == TRAPFRAME)
				continue;
			void *mem = kzalloc(PAGE_SIZE);    /* <项目内: src/mm/kalloc.c> */
			if (!mem)
				return -ENOMEM;
			memcpy(mem, (void *)PTE2PA(pte), PAGE_SIZE);  /* <string.h> */
			pte_t *dst_pte = va2pte(dst, va, true);  /* <项目内: src/mm/pagetable.c> */
			if (!dst_pte) {
				kfree(mem);
				return -ENOMEM;
			}
			*dst_pte = PA2PTE(mem) | PTE_FLAGS(pte);
		} else if (level > 0) {
			/* 非叶 PTE：递归进入下一级 */
			if (uvmcopy_tree_level((pagetable_t)PTE2PA(pte), dst,
					       level - 1, va) != 0)
				return -ENOMEM;
		}
	}
	return 0;
}

int uvmcopy_tree(pagetable_t src, pagetable_t dst)
{
	if (!src || !dst)
		return -EINVAL;
	return uvmcopy_tree_level(src, dst, 2, 0);
}

static int uvmshare_tree_level(pagetable_t src, pagetable_t dst,
			       int level, uintptr_t base)
{
	for (int i = 0; i < 512; i++) {
		pte_t pte = src[i];
		if (!(pte & PTE_V))
			continue;
		uintptr_t va = base | ((uintptr_t)i << PXSHIFT(level));
		if (pte & (PTE_R | PTE_W | PTE_X)) {
			pte_t *dst_pte;

			if (va == TRAMPOLINE || va == TRAPFRAME)
				continue;
			dst_pte = va2pte(dst, va, true);
			if (!dst_pte)
				return -ENOMEM;
			*dst_pte = pte;
		} else if (level > 0) {
			if (uvmshare_tree_level((pagetable_t)PTE2PA(pte), dst,
						level - 1, va) != 0)
				return -ENOMEM;
		}
	}
	return 0;
}

int uvmshare_tree(pagetable_t src, pagetable_t dst)
{
	if (!src || !dst)
		return -EINVAL;
	return uvmshare_tree_level(src, dst, 2, 0);
}
