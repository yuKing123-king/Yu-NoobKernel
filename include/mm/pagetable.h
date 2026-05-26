#pragma once

#include <config.h>
#include <misc/stddef.h>

#define SATP_SV39 (8L << 60)
#define MAKE_SATP(pagetable) (SATP_SV39 | (((u64)pagetable) >> 12))

#define PA2PTE(pa) ((((u64)pa) >> PAGE_SHIFT) << 10)
#define PTE2PA(pte) (((pte) >> 10) << PAGE_SHIFT)

#define PTE_V (1L << 0) // Valid - 页表项是否有效
#define PTE_R (1L << 1) // Readable - 允许读取
#define PTE_W (1L << 2) // Writable - 允许写入
#define PTE_X (1L << 3) // Executable - 允许执行
#define PTE_U (1L << 4) // User - 用户态是否可访问(1-允许,0-禁止)
#define PTE_G (1L << 5) // Global - 全局映射(TLB在切换地址空间时无需刷新该项)
#define PTE_A (1L << 6) // Accessed - 页是否被访问过
#define PTE_D (1L << 7) // Dirty - 页是否被修改过
#define PTE_M (1L << 8) // Mapped - 自定义flag, 用于标识页是否被映射
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

#define PXMASK 0x1FF // 9 bits
#define PXSHIFT(level) (PAGE_SHIFT + (9 * (level)))
#define PX(level, va) ((((u64)(va)) >> PXSHIFT(level)) & PXMASK)

typedef u64 pte_t;

pte_t *va2pte(pagetable_t pagetable, uintptr_t va, bool alloc);
uintptr_t walkaddr(pagetable_t pagetable, uintptr_t va);
pagetable_t pagetable_create();
void pagetable_destroy(pagetable_t pagetable);
int mappages(pagetable_t pagetable, uintptr_t va, uintptr_t pa, size_t npages,
	     int perm);
int unmappages(pagetable_t pagetable, uintptr_t vm, size_t npages);
