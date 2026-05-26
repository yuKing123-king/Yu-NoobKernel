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
