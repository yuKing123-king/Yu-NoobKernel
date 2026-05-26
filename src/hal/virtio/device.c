#include "device.h"
#include <hal/blk.h>
#include <hal/plic.h>
#include <misc/log.h>
#include <config.h>

int virtio_init(void)
{
	int ret;

	ret = virtio_blk_init();
	if (ret < 0) {
		warnf("virtio_init: virtio_blk_init failed");
		return ret;
	}

	infof("virtio_init: all devices initialized");
	return 0;
}
