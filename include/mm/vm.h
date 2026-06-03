#pragma once

#define VM_START (uintptr_t)0x00000000
#define VM_END (uintptr_t)(1ULL << (9 + 9 + 9 + 12 - 1))
// 用户地址空间
#define USER_BASE (0x1000ULL)
#define USER_TOP (0x80000000ULL)

#define KVM(pa) (uintptr_t)(pa)

// 特权级切换页
#define TRAMPOLINE (VM_END - PAGE_SIZE)
// 中断状态保存页
#define TRAPFRAME (TRAMPOLINE - PAGE_SIZE)

void kvminit();

/* 用户态页表管理 */
pagetable_t uvmcreate(void);
uintptr_t uvmalloc(pagetable_t pt, uintptr_t oldsz, uintptr_t newsz);
uintptr_t uvmdealloc(pagetable_t pt, uintptr_t oldsz, uintptr_t newsz);
void uvmfree(pagetable_t pt, uintptr_t sz);
int uvmcopy(pagetable_t src, pagetable_t dst, uintptr_t sz);

/* 页表树遍历：O(已映射页数) 替代线性扫描 O(USER_TOP) */
void uvm_free_user_pages(pagetable_t pt);
int uvmcopy_tree(pagetable_t src, pagetable_t dst);

/* 用户/内核数据拷贝 */
int copyin(pagetable_t pt, char *dst, uintptr_t srcva, size_t len);
int copyout(pagetable_t pt, uintptr_t dstva, char *src, size_t len);
int copyinstr(pagetable_t pt, char *dst, uintptr_t srcva, size_t max);
