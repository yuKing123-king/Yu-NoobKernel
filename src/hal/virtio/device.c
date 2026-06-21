#include "device.h"
#include <hal/blk.h>
#include <hal/plic.h>
#include <misc/log.h>
#include <config.h>

int virtio_init(void)
{
	int ret;

	ret = virtio_blk_init_all();
	if (ret <= 0) {
		warnf("virtio_init: no block devices found");
	}

	ret = virtio_net_init();
	if (ret < 0) {
		warnf("virtio_init: no net device found");
	}

	return 0;
}
