#pragma once

#include <hal/virtio.h>
#include <sync/spinlock.h>

struct virtio_device {
	spinlock_t lock;
	struct virtio_mmio_regs *regs;
	u32 irqno;
	u32 device_id;
	u32 vendor_id;
	u32 negotiated_features;
	struct virtq *vqs[8];
	u32 num_vqs;
	bool polling_mode;
};

int virtio_mmio_probe(struct virtio_mmio_regs *regs, u32 irqno);
int virtio_mmio_init(struct virtio_device *dev);
int virtio_negotiate_features(struct virtio_device *dev, u32 required,
			      u32 rejected);
int virtio_setup_vq(struct virtio_device *dev, u32 vq_idx, u16 num);
void virtio_irq_handler(struct virtio_device *dev);
u32 virtio_read_config(struct virtio_device *dev, u32 offset);
void virtio_write_config(struct virtio_device *dev, u32 offset, u32 value);
void virtio_set_status(struct virtio_device *dev, u32 status);
u32 virtio_get_status(struct virtio_device *dev);

int virtio_blk_init(void);
void virtio_blk_isr(void);

int virtio_init(void);
