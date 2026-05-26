#include "device.h"
#include <misc/log.h>
#include <sync/barrier.h>
#include <hal/plic.h>
#include <hal/riscv.h>
#include <config.h>

#define VIRTIO_MMIO_MAGIC 0x74726976
#define VIRTIO_MMIO_VERSION 2

#define VIRTIO_REG_MAGIC_VALUE 0x000
#define VIRTIO_REG_VERSION 0x004
#define VIRTIO_REG_DEVICE_ID 0x008
#define VIRTIO_REG_VENDOR_ID 0x00c
#define VIRTIO_REG_DEVICE_FEATURES 0x010
#define VIRTIO_REG_DRIVER_FEATURES 0x020
#define VIRTIO_REG_QUEUE_SEL 0x030
#define VIRTIO_REG_QUEUE_NUM_MAX 0x034
#define VIRTIO_REG_QUEUE_NUM 0x038
#define VIRTIO_REG_QUEUE_READY 0x044
#define VIRTIO_REG_QUEUE_NOTIFY 0x050
#define VIRTIO_REG_INT_STATUS 0x060
#define VIRTIO_REG_INT_ACK 0x064
#define VIRTIO_REG_STATUS 0x070
#define VIRTIO_REG_QUEUE_DESC_LO 0x080
#define VIRTIO_REG_QUEUE_DESC_HI 0x084
#define VIRTIO_REG_QUEUE_AVAIL_LO 0x090
#define VIRTIO_REG_QUEUE_AVAIL_HI 0x094
#define VIRTIO_REG_QUEUE_USED_LO 0x0a0
#define VIRTIO_REG_QUEUE_USED_HI 0x0a4
#define VIRTIO_REG_CONFIG_GEN 0x0fc
#define VIRTIO_REG_CONFIG 0x100

static inline u32 virtio_read_reg(struct virtio_device *dev, u32 offset)
{
	return mmio_read32((void *)((uintptr_t)dev->regs + offset));
}

static inline void virtio_write_reg(struct virtio_device *dev, u32 offset,
				    u32 val)
{
	mmio_write32((void *)((uintptr_t)dev->regs + offset), val);
}

void virtio_set_status(struct virtio_device *dev, u32 status)
{
	virtio_write_reg(dev, VIRTIO_REG_STATUS, status);
	mb();
}

u32 virtio_get_status(struct virtio_device *dev)
{
	rmb();
	return virtio_read_reg(dev, VIRTIO_REG_STATUS);
}

int virtio_mmio_probe(struct virtio_mmio_regs *regs, u32 irqno)
{
	u32 magic = mmio_read32(&regs->magic_value);
	u32 version = mmio_read32(&regs->version);
	u32 device_id = mmio_read32(&regs->device_id);
	u32 vendor_id = mmio_read32(&regs->vendor_id);

	if (magic != VIRTIO_MMIO_MAGIC) {
		warnf("virtio_mmio: invalid magic %x", magic);
		return -1;
	}

	if (version != VIRTIO_MMIO_VERSION) {
		warnf("virtio_mmio: unsupported version %d", version);
		return -1;
	}

	if (device_id == 0) {
		debugf("virtio_mmio: no device at this address");
		return -1;
	}

	infof("virtio_mmio: found device id=%d, vendor=%x", device_id,
	      vendor_id);
	return 0;
}

int virtio_mmio_init(struct virtio_device *dev)
{
	virtio_set_status(dev, 0);
	mb();

	virtio_set_status(dev, VIRTIO_CONFIG_S_ACKNOWLEDGE);
	if (!(virtio_get_status(dev) & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
		errorf("virtio_mmio_init: ACKNOWLEDGE failed");
		return -1;
	}

	virtio_set_status(dev,
			  VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
	if (!(virtio_get_status(dev) & VIRTIO_CONFIG_S_DRIVER)) {
		errorf("virtio_mmio_init: DRIVER failed");
		return -1;
	}

	dev->device_id = virtio_read_reg(dev, VIRTIO_REG_DEVICE_ID);
	dev->vendor_id = virtio_read_reg(dev, VIRTIO_REG_VENDOR_ID);

	infof("virtio_mmio_init: device initialized, id=%d", dev->device_id);
	return 0;
}

int virtio_negotiate_features(struct virtio_device *dev, u32 required,
			      u32 rejected)
{
	u32 device_features = virtio_read_reg(dev, VIRTIO_REG_DEVICE_FEATURES);

	u32 driver_features = device_features & ~rejected;
	driver_features |= required;

	virtio_write_reg(dev, VIRTIO_REG_DRIVER_FEATURES, driver_features);
	dev->negotiated_features = driver_features;

	virtio_set_status(dev,
			  virtio_get_status(dev) | VIRTIO_CONFIG_S_FEATURES_OK);
	mb();

	u32 status = virtio_get_status(dev);
	if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
		errorf("virtio_negotiate_features: FEATURES_OK not set");
		virtio_set_status(dev, VIRTIO_CONFIG_S_FAILED);
		return -1;
	}

	infof("virtio_negotiate_features: negotiated=%x", driver_features);
	return 0;
}

int virtio_setup_vq(struct virtio_device *dev, u32 vq_idx, u16 num)
{
	if (vq_idx >= 8) {
		errorf("virtio_setup_vq: invalid vq_idx %d", vq_idx);
		return -1;
	}

	virtio_write_reg(dev, VIRTIO_REG_QUEUE_SEL, vq_idx);
	mb();

	u32 max = virtio_read_reg(dev, VIRTIO_REG_QUEUE_NUM_MAX);
	if (num > max) {
		warnf("virtio_setup_vq: requested %d > max %d, using max", num,
		      max);
		num = max;
	}

	if (num == 0) {
		errorf("virtio_setup_vq: queue %d not available", vq_idx);
		return -1;
	}

	struct virtq *vq = virtq_create(dev->regs, vq_idx, num);
	if (!vq) {
		errorf("virtio_setup_vq: failed to create virtq");
		return -1;
	}

	virtio_write_reg(dev, VIRTIO_REG_QUEUE_NUM, num);
	mb();

	uintptr_t desc_pa = (uintptr_t)vq->descs;
	uintptr_t avail_pa = (uintptr_t)vq->avail;
	uintptr_t used_pa = (uintptr_t)vq->used;

	virtio_write_reg(dev, VIRTIO_REG_QUEUE_DESC_LO,
			 (u32)(desc_pa & 0xffffffff));
	virtio_write_reg(dev, VIRTIO_REG_QUEUE_DESC_HI, (u32)(desc_pa >> 32));
	virtio_write_reg(dev, VIRTIO_REG_QUEUE_AVAIL_LO,
			 (u32)(avail_pa & 0xffffffff));
	virtio_write_reg(dev, VIRTIO_REG_QUEUE_AVAIL_HI, (u32)(avail_pa >> 32));
	virtio_write_reg(dev, VIRTIO_REG_QUEUE_USED_LO,
			 (u32)(used_pa & 0xffffffff));
	virtio_write_reg(dev, VIRTIO_REG_QUEUE_USED_HI, (u32)(used_pa >> 32));
	mb();

	virtio_write_reg(dev, VIRTIO_REG_QUEUE_READY, 1);
	mb();

	dev->vqs[vq_idx] = vq;
	dev->num_vqs++;

	infof(
	    "virtio_setup_vq: vq %d ready, num=%d, desc=%p, avail=%p, used=%p",
	    vq_idx, num, desc_pa, avail_pa, used_pa);
	return 0;
}

void virtio_irq_handler(struct virtio_device *dev)
{
	u32 status = virtio_read_reg(dev, VIRTIO_REG_INT_STATUS);

	if (status & 1) {
		debugf("virtio_irq: used buffer notification");
	}

	if (status & 2) {
		debugf("virtio_irq: configuration change");
	}

	virtio_write_reg(dev, VIRTIO_REG_INT_ACK, status);
	mb();
}

u32 virtio_read_config(struct virtio_device *dev, u32 offset)
{
	u32 old_gen, new_gen;
	u32 value;

	do {
		old_gen = virtio_read_reg(dev, VIRTIO_REG_CONFIG_GEN);
		value = virtio_read_reg(dev, VIRTIO_REG_CONFIG + offset);
		new_gen = virtio_read_reg(dev, VIRTIO_REG_CONFIG_GEN);
	} while (old_gen != new_gen);

	return value;
}

void virtio_write_config(struct virtio_device *dev, u32 offset, u32 value)
{
	virtio_write_reg(dev, VIRTIO_REG_CONFIG + offset, value);
	mb();
}
