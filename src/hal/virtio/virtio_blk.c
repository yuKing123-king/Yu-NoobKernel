#include "device.h"
#include <hal/blk.h>
#include <hal/plic.h>
#include <hal/riscv.h>
#include <hal/device_types.h>
#include <mm/bcache.h>
#include <config.h>
#include <misc/string.h>
#include <misc/log.h>
#include <sync/barrier.h>
#include <mm/kalloc.h>
#include <platform/qemu_virt.h>

struct virtio_blk_request {
	struct virtio_blk_req hdr;
	void *data;
	u8 status;
	u32 nsectors;
	bool completed;
};

struct virtio_blk_device {
	struct virtio_device vdev;
	struct block_device blk_dev;
	u64 capacity;
	u32 block_size;
};

static struct virtio_blk_device *virtio_blks[VIRTIO_BLK_MAX_DEVICES];
static int virtio_blk_count = 0;

static int virtio_blk_rw(struct block_device *dev, u64 sector, void *buf,
			 u32 nsectors, bool write);
static int virtio_blk_flush(struct block_device *dev);
static u64 virtio_blk_capacity(struct block_device *dev);
static u32 virtio_blk_block_size(struct block_device *dev);

static struct blk_operations virtio_blk_ops = {
    .read = NULL,
    .write = NULL,
    .flush = virtio_blk_flush,
    .get_capacity = virtio_blk_capacity,
    .get_block_size = virtio_blk_block_size,
};

static int virtio_blk_rw_internal(struct block_device *dev, u64 sector,
				  void *buf, u32 nsectors, bool write)
{
	struct virtio_blk_device *blk_dev = dev->private;
	struct virtio_device *vdev = &blk_dev->vdev;
	struct virtq *vq = vdev->vqs[0];

	if (!vq) {
		errorf("virtio_blk_rw: no virtqueue");
		return -1;
	}

	if (sector + nsectors > blk_dev->capacity) {
		errorf("virtio_blk_rw: sector overflow");
		return -1;
	}

	struct virtio_blk_request *req = kzalloc(sizeof(*req));
	if (!req) {
		errorf("virtio_blk_rw: failed to allocate request");
		return -1;
	}

	req->hdr.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
	req->hdr.reserved = 0;
	req->hdr.sector = sector;
	req->data = buf;
	req->nsectors = nsectors;
	req->status = VIRTIO_BLK_S_IOERR;
	req->completed = false;

	struct virtq_buf in_bufs[1] = {
	    {.addr = &req->hdr, .len = sizeof(req->hdr)}};

	struct virtq_buf out_bufs[2];
	int n_out;

	if (write) {
		out_bufs[0].addr = buf;
		out_bufs[0].len = nsectors * BLOCK_SIZE;
		out_bufs[1].addr = &req->status;
		out_bufs[1].len = 1;
		n_out = 2;
	} else {
		out_bufs[0].addr = buf;
		out_bufs[0].len = nsectors * BLOCK_SIZE;
		out_bufs[1].addr = &req->status;
		out_bufs[1].len = 1;
		n_out = 2;
	}

	int idx = virtq_add_buf(vq, in_bufs, 1, out_bufs, n_out, req);
	if (idx < 0) {
		kfree(req);
		errorf("virtio_blk_rw: failed to add buffer");
		return -1;
	}

	virtq_kick(vq);

	while (!req->completed) {
		if (virtq_has_buf(vq)) {
			u32 len;
			struct virtio_blk_request *done =
			    virtq_get_buf(vq, &len);
			if (done) {
				done->completed = true;
			}
		}
	}

	int ret = 0;
	if (req->status != VIRTIO_BLK_S_OK) {
		warnf("virtio_blk_rw: status=%d", req->status);
		ret = -1;
	}

	kfree(req);
	return ret;
}

static int virtio_blk_read(struct block_device *dev, u64 sector, void *buf,
			   u32 nsectors)
{
	return virtio_blk_rw_internal(dev, sector, buf, nsectors, false);
}

static int virtio_blk_write(struct block_device *dev, u64 sector,
			    const void *buf, u32 nsectors)
{
	return virtio_blk_rw_internal(dev, sector, (void *)buf, nsectors, true);
}

static int virtio_blk_flush(struct block_device *dev) { return 0; }

static u64 virtio_blk_capacity(struct block_device *dev)
{
	struct virtio_blk_device *blk_dev = dev->private;
	return blk_dev->capacity;
}

static u32 virtio_blk_block_size(struct block_device *dev)
{
	struct virtio_blk_device *blk_dev = dev->private;
	return blk_dev->block_size;
}

void virtio_blk_isr(int irqno)
{
	for (int i = 0; i < virtio_blk_count; i++) {
		struct virtio_blk_device *blk = virtio_blks[i];
		if (!blk)
			continue;

		virtio_irq_handler(&blk->vdev);

		struct virtq *vq = blk->vdev.vqs[0];
		if (vq) {
			while (virtq_has_buf(vq)) {
				u32 len;
				struct virtio_blk_request *req =
				    virtq_get_buf(vq, &len);
				if (req) {
					req->completed = true;
				}
			}
		}
	}
}

/*
 * 初始化单个 Virtio 块设备
 * @param mmio_addr: MMIO 寄存器基地址
 * @param irqno: 中断号
 * @param minor: 设备 minor 号
 * @return: 成功返回0，失败返回-1
 */
static int virtio_blk_init_one(uintptr_t mmio_addr, u32 irqno, int minor)
{
	struct virtio_mmio_regs *regs = (void *)mmio_addr;

	if (virtio_mmio_probe(regs, irqno) < 0)
		return -1;

	u32 device_id = mmio_read32(&regs->device_id);
	if (device_id != VIRTIO_DEVICE_TYPE_DISK) {
		warnf("virtio_blk_init: not a block device at %p, id=%d",
		      (void *)mmio_addr, device_id);
		return -1;
	}

	if (minor >= VIRTIO_BLK_MAX_DEVICES || virtio_blk_count >= VIRTIO_BLK_MAX_DEVICES) {
		errorf("virtio_blk_init: too many devices");
		return -1;
	}

	struct virtio_blk_device *blk = kzalloc(sizeof(*blk));
	if (!blk) {
		errorf("virtio_blk_init: failed to allocate device");
		return -1;
	}

	blk->vdev.regs = regs;
	blk->vdev.irqno = irqno;
	blk->vdev.lock = SPINLOCK_INITIALIZER("virtio_blk");
	blk->vdev.polling_mode = true;

	if (virtio_mmio_init(&blk->vdev) < 0) {
		kfree(blk);
		return -1;
	}

	u32 required = 0;
	u32 rejected = VIRTIO_BLK_F_RO | VIRTIO_BLK_F_SCSI |
		       VIRTIO_BLK_F_CONFIG_WCE | VIRTIO_BLK_F_MQ |
		       VIRTIO_F_ANY_LAYOUT | VIRTIO_RING_F_INDIRECT_DESC |
		       VIRTIO_RING_F_EVENT_IDX;

	if (virtio_negotiate_features(&blk->vdev, required, rejected) < 0) {
		kfree(blk);
		return -1;
	}

	if (virtio_setup_vq(&blk->vdev, 0, VIRTQ_SIZE) < 0) {
		kfree(blk);
		return -1;
	}

	u32 capacity_lo = virtio_read_config(&blk->vdev, 0);
	u32 capacity_hi = virtio_read_config(&blk->vdev, 4);
	blk->capacity = ((u64)capacity_hi << 32) | capacity_lo;
	blk->block_size = virtio_read_config(&blk->vdev, 20);
	if (blk->block_size == 0) {
		blk->block_size = BLOCK_SIZE;
	}

	virtio_set_status(&blk->vdev,
			  virtio_get_status(&blk->vdev) |
			      VIRTIO_CONFIG_S_DRIVER_OK);

	virtio_blk_ops.read = virtio_blk_read;
	virtio_blk_ops.write = virtio_blk_write;

	blk->blk_dev.devno = MKDEV(BLK_MAJOR_VIRTIO, minor);
	snprintf(blk->blk_dev.name, 16, "virtio-blk%d", minor);
	blk->blk_dev.ops = &virtio_blk_ops;
	blk->blk_dev.private = blk;
	blk->blk_dev.lock = SPINLOCK_INITIALIZER("blk_virtio");

	if (blk_register(&blk->blk_dev) < 0) {
		errorf("virtio_blk_init: failed to register block device");
		kfree(blk);
		return -1;
	}

	virtio_blks[minor] = blk;
	virtio_blk_count++;

	infof("virtio_blk_init: device %d ready, capacity=%llu sectors, "
	      "block_size=%d, irqno=%d",
	      minor, blk->capacity, blk->block_size, irqno);

	return 0;
}

/*
 * 探测并初始化所有 Virtio 块设备
 * 遍历所有 MMIO 槽位，找到类型为 DISK 的设备并初始化
 * @return: 成功初始化的设备数量
 */
int virtio_blk_init_all(void)
{
	int count = 0;

	/* 遍历所有 VirtIO MMIO 槽位 (QEMU virt 机器有 8 个) */
	struct virtio_mmio_raw slots[] = {
		VIRTIO_MMIO_0, VIRTIO_MMIO_1, VIRTIO_MMIO_2,
		VIRTIO_MMIO_3, VIRTIO_MMIO_4, VIRTIO_MMIO_5,
		VIRTIO_MMIO_6, VIRTIO_MMIO_7,
	};

	for (int i = 0; i < 8; i++) {
		struct virtio_mmio_regs *regs = (void *)slots[i].addr;
		u32 magic = mmio_read32(&regs->magic_value);

		if (magic != 0x74726976)
			continue;

		u32 device_id = mmio_read32(&regs->device_id);
		if (device_id == 0)
			continue;

		if (device_id == VIRTIO_DEVICE_TYPE_DISK) {
			if (virtio_blk_init_one(slots[i].addr, slots[i].irqno, count) == 0) {
				count++;
			}
		}
		/* 非 DISK 设备（如网络设备）跳过，后续扩展 */
	}

	infof("virtio_blk_init_all: %d block device(s) initialized", count);
	return count;
}
