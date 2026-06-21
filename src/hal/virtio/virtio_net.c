#include "device.h"
#include <hal/virtio.h>
#include <hal/virtio_net.h>
#include <hal/device_types.h>
#include <hal/plic.h>
#include <hal/riscv.h>
#include <mm/kalloc.h>
#include <platform/qemu_virt.h>
#include <misc/string.h>
#include <misc/log.h>
#include <sync/barrier.h>
#include <config.h>

#define VIRTIO_NET_HDR_SIZE 10
#define VIRTIO_NET_BUF_SIZE  (VIRTIO_NET_HDR_SIZE + 1600)

struct virtio_net_device {
	struct virtio_device vdev;
	u8 mac[6];
	void *rx_bufs[VIRTQ_SIZE];
};

static struct virtio_net_device *net_dev;

struct virtio_net_tx_req {
	void *buf;
	bool completed;
};

static void virtio_net_fill_rx(struct virtio_net_device *dev)
{
	struct virtq *vq = dev->vdev.vqs[0];
	int count = 0;

	for (int i = 0; i < VIRTQ_SIZE; i++) {
		void *buf = kzalloc(VIRTIO_NET_BUF_SIZE);
		if (!buf)
			break;

		dev->rx_bufs[i] = buf;

		struct virtq_buf out_bufs[1] = {
		    { .addr = buf, .len = VIRTIO_NET_BUF_SIZE }
		};

		if (virtq_add_buf(vq, NULL, 0, out_bufs, 1,
				  (void *)(uintptr_t)i) < 0) {
			kfree(buf);
			dev->rx_bufs[i] = NULL;
			break;
		}
		count++;
	}

	virtq_kick(vq);
}

static int virtio_net_init_one(uintptr_t mmio_addr, u32 irqno)
{
	struct virtio_mmio_regs *regs = (void *)mmio_addr;

	if (virtio_mmio_probe(regs, irqno) < 0)
		return -1;

	u32 device_id = mmio_read32(&regs->device_id);
	if (device_id != VIRTIO_DEVICE_TYPE_NET) {
		warnf("virtio_net_init: not a net device at %p, id=%d",
		      (void *)mmio_addr, device_id);
		return -1;
	}

	struct virtio_net_device *dev = kzalloc(sizeof(*dev));
	if (!dev) {
		errorf("virtio_net_init: failed to allocate device");
		return -1;
	}

	dev->vdev.regs = regs;
	dev->vdev.irqno = irqno;
	dev->vdev.lock = SPINLOCK_INITIALIZER("virtio_net");
	dev->vdev.polling_mode = true;

	if (virtio_mmio_init(&dev->vdev) < 0) {
		kfree(dev);
		return -1;
	}

	u32 required = 0;
	u32 rejected = VIRTIO_NET_F_CSUM | VIRTIO_F_ANY_LAYOUT |
		       VIRTIO_RING_F_INDIRECT_DESC | VIRTIO_RING_F_EVENT_IDX;

	if (virtio_negotiate_features(&dev->vdev, required, rejected) < 0) {
		kfree(dev);
		return -1;
	}

	/* receiveq (queue 0) + transmitq (queue 1) */
	if (virtio_setup_vq(&dev->vdev, 0, VIRTQ_SIZE) < 0) {
		kfree(dev);
		return -1;
	}
	if (virtio_setup_vq(&dev->vdev, 1, VIRTQ_SIZE) < 0) {
		kfree(dev);
		return -1;
	}

	/* 读取 MAC 地址（config 偏移 0-5） */
	u32 mac0 = virtio_read_config(&dev->vdev, 0);
	u32 mac1 = virtio_read_config(&dev->vdev, 4);
	dev->mac[0] = mac0 & 0xff;
	dev->mac[1] = (mac0 >> 8) & 0xff;
	dev->mac[2] = (mac0 >> 16) & 0xff;
	dev->mac[3] = (mac0 >> 24) & 0xff;
	dev->mac[4] = mac1 & 0xff;
	dev->mac[5] = (mac1 >> 8) & 0xff;

	virtio_set_status(&dev->vdev,
			  virtio_get_status(&dev->vdev) |
			      VIRTIO_CONFIG_S_DRIVER_OK);

	virtio_net_fill_rx(dev);

	net_dev = dev;

	return 0;
}

int virtio_net_init(void)
{
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

		if (device_id == VIRTIO_DEVICE_TYPE_NET) {
			if (virtio_net_init_one(slots[i].addr, slots[i].irqno) == 0)
				return 0;
		}
	}

	warnf("virtio_net_init: no net device found");
	return -1;
}

int virtio_net_get_mac(u8 mac[6])
{
	if (!net_dev)
		return -1;

	memcpy(mac, net_dev->mac, 6);
	return 0;
}

int virtio_net_send(const void *data, int len)
{
	if (!net_dev)
		return -1;

	struct virtq *vq = net_dev->vdev.vqs[1];
	if (!vq)
		return -1;

	if (len > 1600 || len <= 0)
		return -1;

	int total = VIRTIO_NET_HDR_SIZE + len;
	void *buf = kzalloc(total);
	if (!buf)
		return -1;

	/* net_hdr 全零（无 offload） */
	memcpy((u8 *)buf + VIRTIO_NET_HDR_SIZE, data, len);

	struct virtio_net_tx_req *req = kzalloc(sizeof(*req));
	if (!req) {
		kfree(buf);
		return -1;
	}
	req->buf = buf;
	req->completed = false;

	struct virtq_buf in_bufs[1] = {
	    { .addr = buf, .len = total }
	};

	if (virtq_add_buf(vq, in_bufs, 1, NULL, 0, req) < 0) {
		kfree(buf);
		kfree(req);
		return -1;
	}

	virtq_kick(vq);

	while (!req->completed) {
		if (virtq_has_buf(vq)) {
			u32 done_len;
			struct virtio_net_tx_req *done = virtq_get_buf(vq, &done_len);
			if (done)
				done->completed = true;
		}
	}

	kfree(buf);
	kfree(req);
	return len;
}

int virtio_net_recv(void *buf, int buf_len)
{
	if (!net_dev)
		return -1;

	struct virtq *vq = net_dev->vdev.vqs[0];
	if (!vq)
		return -1;

	if (!virtq_has_buf(vq))
		return -1;

	u32 len;
	void *token = virtq_get_buf(vq, &len);
	if (!token)
		return -1;

	int idx = (int)(uintptr_t)token;
	void *rx_buf = net_dev->rx_bufs[idx];

	int pkt_len = len - VIRTIO_NET_HDR_SIZE;
	if (pkt_len <= 0)
		pkt_len = 0;
	if (pkt_len > buf_len)
		pkt_len = buf_len;

	if (pkt_len > 0)
		memcpy(buf, (u8 *)rx_buf + VIRTIO_NET_HDR_SIZE, pkt_len);

	/* 立即重新提交 buffer 到 receiveq */
	struct virtq_buf out_bufs[1] = {
	    { .addr = rx_buf, .len = VIRTIO_NET_BUF_SIZE }
	};

	if (virtq_add_buf(vq, NULL, 0, out_bufs, 1,
			  (void *)(uintptr_t)idx) < 0) {
		warnf("virtio_net_recv: failed to re-add rx buffer");
	}

	virtq_kick(vq);

	return pkt_len;
}

void virtio_net_isr(void)
{
	if (!net_dev)
		return;

	virtio_irq_handler(&net_dev->vdev);

	/* TX: 标记完成的请求 */
	struct virtq *txq = net_dev->vdev.vqs[1];
	if (txq) {
		while (virtq_has_buf(txq)) {
			u32 len;
			struct virtio_net_tx_req *req =
			    virtq_get_buf(txq, &len);
			if (req)
				req->completed = true;
		}
	}
}
