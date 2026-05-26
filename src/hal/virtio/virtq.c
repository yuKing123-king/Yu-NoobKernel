#include <hal/virtio.h>
#include <mm/kalloc.h>
#include <mm/pm.h>
#include <misc/string.h>
#include <misc/log.h>
#include <sync/barrier.h>
#include <hal/riscv.h>

static inline uintptr_t va2pa(void *va) { return (uintptr_t)va; }

struct virtq *virtq_create(struct virtio_mmio_regs *regs, u16 queue_idx,
			   u16 num)
{
	size_t size = vring_size(num);
	size = ALIGN_UP(size, PAGE_SIZE);

	void *mem = kmalloc(size);
	if (!mem) {
		errorf("virtq_create: failed to allocate memory");
		return NULL;
	}
	memset(mem, 0, size);

	struct virtq *vq = kmalloc(sizeof(struct virtq));
	if (!vq) {
		kfree(mem);
		errorf("virtq_create: failed to allocate virtq struct");
		return NULL;
	}

	vq->regs = regs;
	vq->queue_idx = queue_idx;
	vq->num = num;
	vq->num_free = num;
	vq->free_head = 0;
	vq->avail_idx = 0;
	vq->used_idx = 0;
	vq->lock = SPINLOCK_INITIALIZER("virtq");

	for (int i = 0; i < VIRTQ_SIZE; i++) {
		vq->data[i] = NULL;
	}

	uintptr_t pa = va2pa(mem);
	vq->descs = (struct virtq_desc *)mem;

	size_t avail_offset = sizeof(struct virtq_desc) * num;
	vq->avail = (struct virtq_avail *)((uintptr_t)mem + avail_offset);

	size_t used_offset = avail_offset + sizeof(u16) * (3 + num);
	used_offset = ALIGN_UP(used_offset, VRING_ALIGN);
	vq->used = (struct virtq_used *)((uintptr_t)mem + used_offset);

	for (int i = 0; i < num; i++) {
		vq->descs[i].addr = 0;
		vq->descs[i].len = 0;
		vq->descs[i].flags = VRING_DESC_F_NEXT;
		vq->descs[i].next = (i + 1) % num;
	}
	vq->descs[num - 1].flags = 0;
	vq->descs[num - 1].next = 0;

	vq->avail->flags = 0;
	vq->avail->idx = 0;
	vq->used->flags = 0;
	vq->used->idx = 0;

	infof("virtq created: num=%d, descs=%p, avail=%p, used=%p", num,
	      vq->descs, vq->avail, vq->used);

	return vq;
}

void virtq_destroy(struct virtq *vq)
{
	if (!vq)
		return;

	kfree(vq->descs);
	kfree(vq);
}

static u16 virtq_alloc_desc(struct virtq *vq)
{
	if (vq->num_free == 0) {
		return -1;
	}

	u16 idx = vq->free_head;
	vq->free_head = vq->descs[idx].next;
	vq->num_free--;

	return idx;
}

static void virtq_free_desc(struct virtq *vq, u16 idx)
{
	vq->descs[idx].addr = 0;
	vq->descs[idx].len = 0;
	vq->descs[idx].flags = VRING_DESC_F_NEXT;
	vq->descs[idx].next = vq->free_head;
	vq->free_head = idx;
	vq->num_free++;
}

int virtq_add_buf(struct virtq *vq, struct virtq_buf *in_bufs, int n_in,
		  struct virtq_buf *out_bufs, int n_out, void *token)
{
	spinlock_acquire(&vq->lock);

	int total = n_in + n_out;
	if (total > vq->num_free) {
		spinlock_release(&vq->lock);
		return -1;
	}

	u16 head = virtq_alloc_desc(vq);
	u16 prev = head;
	vq->data[head] = token;

	for (int i = 0; i < n_in; i++) {
		vq->descs[prev].addr = va2pa(in_bufs[i].addr);
		vq->descs[prev].len = in_bufs[i].len;
		vq->descs[prev].flags = 0;

		if (i < n_in - 1 || n_out > 0) {
			u16 next = virtq_alloc_desc(vq);
			vq->descs[prev].next = next;
			vq->descs[prev].flags |= VRING_DESC_F_NEXT;
			prev = next;
		}
	}

	for (int i = 0; i < n_out; i++) {
		vq->descs[prev].addr = va2pa(out_bufs[i].addr);
		vq->descs[prev].len = out_bufs[i].len;
		vq->descs[prev].flags = VRING_DESC_F_WRITE;

		if (i < n_out - 1) {
			u16 next = virtq_alloc_desc(vq);
			vq->descs[prev].next = next;
			vq->descs[prev].flags |= VRING_DESC_F_NEXT;
			prev = next;
		}
	}

	vq->descs[prev].flags &= ~VRING_DESC_F_NEXT;
	vq->descs[prev].next = 0;

	u16 avail_idx = vq->avail_idx % vq->num;
	vq->avail->ring[avail_idx] = head;
	wmb();
	vq->avail->idx = ++vq->avail_idx;

	spinlock_release(&vq->lock);

	return head;
}

void *virtq_get_buf(struct virtq *vq, u32 *len)
{
	spinlock_acquire(&vq->lock);

	rmb();
	u16 used_idx = vq->used->idx;
	if (vq->used_idx == used_idx) {
		spinlock_release(&vq->lock);
		return NULL;
	}

	u16 idx = vq->used_idx % vq->num;
	u16 head = vq->used->ring[idx].id;
	if (len) {
		*len = vq->used->ring[idx].len;
	}

	void *token = vq->data[head];
	vq->data[head] = NULL;

	u16 next;
	while (vq->descs[head].flags & VRING_DESC_F_NEXT) {
		next = vq->descs[head].next;
		virtq_free_desc(vq, head);
		head = next;
	}
	virtq_free_desc(vq, head);

	vq->used_idx++;

	spinlock_release(&vq->lock);

	return token;
}

bool virtq_has_buf(struct virtq *vq)
{
	rmb();
	return vq->used_idx != vq->used->idx;
}

void virtq_kick(struct virtq *vq)
{
	mb();
	if (vq->avail->flags & 1) {
		return;
	}
	mmio_write32(&vq->regs->queue_notify, vq->queue_idx);
}

u16 virtq_desc_count(struct virtq *vq)
{
	spinlock_acquire(&vq->lock);
	u16 count = vq->num - vq->num_free;
	spinlock_release(&vq->lock);
	return count;
}
