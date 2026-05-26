#pragma once

#include <misc/list.h>
#include <misc/stddef.h>
#include <task/proc.h>

#define VMA_TYPE_START 0
#define VMA_ANON 0  // 匿名映射（如堆、malloc）
#define VMA_FILE 1  // 文件映射（如 mmap 一个可执行文件）
#define VMA_STACK 2 // 用户栈
#define VMA_DEV 3   // 设备映射（内核专用，用户不可见）
#define VMA_TYPE_END 3

struct vma {
	uintptr_t start;
	size_t length;
	int perm;
	int type;

	void *file;
	off_t offset;

	struct list_head list;
};

// 创建一个 VMA（不分配物理页，只记录映射）
struct vma *vma_create(uintptr_t start, size_t length, int perm, int type);

// 销毁一个 VMA（释放结构体内存，不解除页表映射）
void vma_destroy(struct vma *vma);

// 将 vma 插入到进程的 vma 链表中（需合并相邻、检查重叠）
int vma_insert(struct proc *proc, struct vma *new_vma);

// 查找包含 va 的 VMA
struct vma *vma_find(struct proc *proc, uintptr_t va);

// 删除 [start, start+len) 范围内的所有 VMA（可能拆分或删除多个）
int vma_remove(struct proc *proc, uintptr_t start, size_t length);

// 根据 VMA 建立页表映射（仅设置 PTE_M，不分配物理页）
int vma_map_pages(struct proc *proc, struct vma *vma);

// 解除页表映射（调用 unmappages），但保留 VMA（用于 swap 或 lazy free）
int vma_unmap_pages(struct proc *proc, uintptr_t start, size_t length);
