#include <misc/errno.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/vm.h>
#include <mm/vma.h>

struct vma *vma_create(uintptr_t start, size_t length, int perm, int type)
{
	if (start < VM_START || start + length >= VM_END ||
	    type >= VMA_TYPE_START || type <= VMA_TYPE_END)
		return NULL;
	struct vma *vma = kzalloc(sizeof(struct vma));
	if (!vma)
		return NULL;

	vma->start = start;
	vma->length = length;
	vma->perm = perm;
	vma->type = type;
	return vma;
}

// 销毁一个 VMA（释放结构体内存，不解除页表映射）
void vma_destroy(struct vma *vma)
{
	list_del(&vma->list);
	kfree(vma);
}

// 查找包含 va 的 VMA（链表有序，可提前终止）
struct vma *vma_find(struct proc *proc, uintptr_t va)
{
	struct list_head *pos;
	struct vma *vma;
	list_for_each(pos, &proc->vma)
	{
		vma = list_entry(pos, struct vma, list);
		if (va < vma->start)
			break; // 后续 VMA 起始更大，无需继续
		if (va < vma->start + vma->length)
			return vma;
	}
	return NULL;
}

// 插入 VMA（仅检查重叠，不合并）
int vma_insert(struct proc *proc, struct vma *new_vma)
{
	struct list_head *pos;
	list_for_each(pos, &proc->vma)
	{
		struct vma *vma = list_entry(pos, struct vma, list);
		// 检查重叠：[a, a+len) 与 [b, b+len) 重叠 ⇨ !(a+len <= b ||
		// b+len <= a)
		if (!(new_vma->start + new_vma->length <= vma->start ||
		      vma->start + vma->length <= new_vma->start)) {
			return -EEXIST; // 重叠，不允许
		}
		if (vma->start > new_vma->start)
			break;
	}
	list_add_tail(&new_vma->list, pos);
	return 0;
}

// 删除完全匹配的 VMA（不支持拆分，调用者需保证范围对齐整个 VMA）
int vma_remove(struct proc *proc, uintptr_t start, size_t length)
{
	struct vma *vma = vma_find(proc, start);
	if (!vma || vma->start != start || vma->length != length)
		return -ENOENT; // 要求精确匹配（简化逻辑）

	vma_unmap_pages(proc, start, length);
	list_del(&vma->list);
	vma_destroy(vma);
	return 0;
}

// 建立映射（延迟分配：pa=0 ⇒ PTE_V=0, PTE_M=1）
int vma_map_pages(struct proc *proc, struct vma *vma)
{
	return mappages(proc->pagetable, vma->start, 0, vma->length / PAGE_SIZE,
			vma->perm);
}

// 解除页表映射
int vma_unmap_pages(struct proc *proc, uintptr_t start, size_t length)
{
	return unmappages(proc->pagetable, start, length / PAGE_SIZE);
}
