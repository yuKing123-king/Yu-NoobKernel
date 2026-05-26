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

static struct virtio_blk_device *virtio_blk;

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

void virtio_blk_isr(void)
{
	if (!virtio_blk)
		return;

	virtio_irq_handler(&virtio_blk->vdev);

	struct virtq *vq = virtio_blk->vdev.vqs[0];
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

int virtio_blk_init(void)
{
	struct virtio_mmio_raw raw = VIRTIO_MMIO_0;
	struct virtio_mmio_regs *regs = (void *)raw.addr;

	if (virtio_mmio_probe(regs, raw.irqno) < 0) {
		return -1;
	}

	u32 device_id = mmio_read32(&regs->device_id);
	if (device_id != VIRTIO_DEVICE_TYPE_DISK) {
		warnf("virtio_blk_init: not a block device, id=%d", device_id);
		return -1;
	}

	virtio_blk = kzalloc(sizeof(*virtio_blk));
	if (!virtio_blk) {
		errorf("virtio_blk_init: failed to allocate device");
		return -1;
	}

	virtio_blk->vdev.regs = regs;
	virtio_blk->vdev.irqno = raw.irqno;
	virtio_blk->vdev.lock = SPINLOCK_INITIALIZER("virtio_blk");
	virtio_blk->vdev.polling_mode = true;

	if (virtio_mmio_init(&virtio_blk->vdev) < 0) {
		kfree(virtio_blk);
		return -1;
	}

	u32 required = 0;
	u32 rejected = VIRTIO_BLK_F_RO | VIRTIO_BLK_F_SCSI |
		       VIRTIO_BLK_F_CONFIG_WCE | VIRTIO_BLK_F_MQ |
		       VIRTIO_F_ANY_LAYOUT | VIRTIO_RING_F_INDIRECT_DESC |
		       VIRTIO_RING_F_EVENT_IDX;

	if (virtio_negotiate_features(&virtio_blk->vdev, required, rejected) <
	    0) {
		kfree(virtio_blk);
		return -1;
	}

	if (virtio_setup_vq(&virtio_blk->vdev, 0, VIRTQ_SIZE) < 0) {
		kfree(virtio_blk);
		return -1;
	}

	u32 capacity_lo = virtio_read_config(&virtio_blk->vdev, 0);
	u32 capacity_hi = virtio_read_config(&virtio_blk->vdev, 4);
	virtio_blk->capacity = ((u64)capacity_hi << 32) | capacity_lo;
	virtio_blk->block_size = virtio_read_config(&virtio_blk->vdev, 20);
	if (virtio_blk->block_size == 0) {
		virtio_blk->block_size = BLOCK_SIZE;
	}

	virtio_set_status(&virtio_blk->vdev,
			  virtio_get_status(&virtio_blk->vdev) |
			      VIRTIO_CONFIG_S_DRIVER_OK);

	virtio_blk_ops.read = virtio_blk_read;
	virtio_blk_ops.write = virtio_blk_write;

	virtio_blk->blk_dev.devno = MKDEV(BLK_MAJOR_VIRTIO, 0);
	snprintf(virtio_blk->blk_dev.name, 16, "virtio-blk");
	virtio_blk->blk_dev.ops = &virtio_blk_ops;
	virtio_blk->blk_dev.private = virtio_blk;
	virtio_blk->blk_dev.lock = SPINLOCK_INITIALIZER("blk_virtio");

	if (blk_register(&virtio_blk->blk_dev) < 0) {
		errorf("virtio_blk_init: failed to register block device");
		kfree(virtio_blk);
		return -1;
	}

	// TODO: enable PLIC interrupt after fixing PLIC init order
	// plic_set_priority(raw.irqno, 1);
	// plic_enable('S', r_mhartid(), raw.irqno);

	infof("virtio_blk_init: device ready, capacity=%llu sectors, "
	      "block_size=%d",
	      virtio_blk->capacity, virtio_blk->block_size);

	return 0;
}

void virtio_disk_test(void)
{
	infof("=== virtio disk test ===");

	u8 buf[512];
	dev_t devno = MKDEV(BLK_MAJOR_VIRTIO, 0);

	infof("Testing blk_read...");
	int ret = blk_read(devno, 0, buf, 1);
	if (ret < 0) {
		errorf("blk_read failed");
		return;
	}

	infof("First 16 bytes of sector 0:");
	for (int i = 0; i < 16; i++) {
		printf("%02x ", buf[i]);
	}
	printf("\n");

	infof("Testing bcache...");
	struct buf *b = bread(devno, 0);
	if (b) {
		infof("bcache read successful, refcnt=%d", b->refcnt);
		infof("bcache data: %02x %02x %02x %02x", b->data[0],
		      b->data[1], b->data[2], b->data[3]);
		brelse(b);
		infof("bcache released");
	}

	infof("=== virtio disk test complete ===");
}
