#include <misc/errno.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/vm.h>
#include <mm/vma.h>

/*
 * 创建并初始化一个新的VMA结构体
 * @param start: 起始虚拟地址
 * @param length: 区域长度（字节）
 * @param perm: 访问权限标志
 * @param type: VMA类型
 * @return: VMA结构体指针，参数无效时返回NULL
 */
struct vma *vma_create(uintptr_t start, size_t length, int perm, int type)
{
	if (start < VM_START || start + length >= VM_END ||
	    type < VMA_TYPE_START || type > VMA_TYPE_END)
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

/*
 * 销毁一个VMA：从链表移除并释放结构体内存（不解除页表映射）
 * @param vma: 待销毁的VMA指针
 * @return: 无返回值
 */
void vma_destroy(struct vma *vma)
{
	list_del(&vma->list);
	kfree(vma);
}

/*
 * 在进程的VMA链表中查找包含指定虚拟地址的VMA
 * @param proc: 进程结构体指针
 * @param va: 虚拟地址
 * @return: 包含va的VMA指针，未找到返回NULL
 */
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

/*
 * 向进程的VMA链表插入新的VMA（检查重叠但不合并）
 * @param proc: 进程结构体指针
 * @param new_vma: 待插入的VMA指针
 * @return: 成功返回0，重叠时返回-EEXIST
 */
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

/*
 * 删除与指定范围完全匹配的VMA（不支持拆分）
 * @param proc: 进程结构体指针
 * @param start: 起始虚拟地址
 * @param length: 区域长度（字节）
 * @return: 成功返回0，未找到精确匹配返回-ENOENT
 */
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

/*
 * 建立VMA的页表映射（延迟分配：pa=0时仅标记预留，需配合page fault handler分配物理页）
 * @param proc: 进程结构体指针
 * @param vma: 需要建立映射的VMA指针
 * @return: 成功返回0，失败返回负错误码
 */
int vma_map_pages(struct proc *proc, struct vma *vma)
{
	return mappages(proc->pagetable, vma->start, 0, vma->length / PAGE_SIZE,
			vma->perm);
}

/*
 * 解除指定虚拟地址范围的页表映射
 * @param proc: 进程结构体指针
 * @param start: 起始虚拟地址
 * @param length: 区域长度（字节）
 * @return: 成功返回0
 */
int vma_unmap_pages(struct proc *proc, uintptr_t start, size_t length)
{
	return unmappages(proc->pagetable, start, length / PAGE_SIZE);
}
