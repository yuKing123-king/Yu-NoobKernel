#pragma once

#include <misc/stdint.h>
#include <misc/stddef.h>
#include <sync/spinlock.h>

#define BLK_MAJOR_VIRTIO 1
#define BLK_MAJOR_SD 2

struct block_device;

struct blk_operations {
	int (*read)(struct block_device *dev, u64 sector, void *buf,
		    u32 nsectors);
	int (*write)(struct block_device *dev, u64 sector, const void *buf,
		     u32 nsectors);
	int (*flush)(struct block_device *dev);
	u64 (*get_capacity)(struct block_device *dev);
	u32 (*get_block_size)(struct block_device *dev);
};

struct block_device {
	dev_t devno;
	char name[16];
	struct blk_operations *ops;
	void *private;
	spinlock_t lock;
};

int blk_register(struct block_device *dev);
struct block_device *blk_lookup(dev_t devno);
struct block_device *blk_get(dev_t devno);
int blk_read(dev_t devno, u64 sector, void *buf, u32 nsectors);
int blk_write(dev_t devno, u64 sector, const void *buf, u32 nsectors);
int blk_flush(dev_t devno);
u64 blk_capacity(dev_t devno);
u32 blk_block_size(dev_t devno);

void blk_init(void);
