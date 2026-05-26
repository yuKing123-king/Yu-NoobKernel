#include <hal/blk.h>
#include <misc/log.h>
#include <misc/string.h>
#include <mm/kalloc.h>

#define BLK_MAX_DEVICES 16

static struct {
	struct block_device *devices[BLK_MAX_DEVICES];
	u32 count;
	spinlock_t lock;
} blk_registry;

void blk_init(void)
{
	blk_registry.count = 0;
	blk_registry.lock = SPINLOCK_INITIALIZER("blk_registry");
	for (int i = 0; i < BLK_MAX_DEVICES; i++) {
		blk_registry.devices[i] = NULL;
	}
}

int blk_register(struct block_device *dev)
{
	spinlock_acquire(&blk_registry.lock);

	if (blk_registry.count >= BLK_MAX_DEVICES) {
		spinlock_release(&blk_registry.lock);
		errorf("blk_register: too many devices");
		return -1;
	}

	u32 major = MAJOR(dev->devno);
	u32 minor = MINOR(dev->devno);

	for (int i = 0; i < blk_registry.count; i++) {
		if (blk_registry.devices[i]->devno == dev->devno) {
			spinlock_release(&blk_registry.lock);
			warnf("blk_register: device %d:%d already registered",
			      major, minor);
			return -1;
		}
	}

	blk_registry.devices[blk_registry.count++] = dev;

	spinlock_release(&blk_registry.lock);

	infof("blk_register: device %d:%d (%s) registered", major, minor,
	      dev->name);
	return 0;
}

struct block_device *blk_lookup(dev_t devno)
{
	spinlock_acquire(&blk_registry.lock);

	for (int i = 0; i < blk_registry.count; i++) {
		if (blk_registry.devices[i]->devno == devno) {
			struct block_device *dev = blk_registry.devices[i];
			spinlock_release(&blk_registry.lock);
			return dev;
		}
	}

	spinlock_release(&blk_registry.lock);
	return NULL;
}

struct block_device *blk_get(dev_t devno) { return blk_lookup(devno); }

int blk_read(dev_t devno, u64 sector, void *buf, u32 nsectors)
{
	struct block_device *dev = blk_lookup(devno);
	if (!dev) {
		errorf("blk_read: device %d:%d not found", MAJOR(devno),
		       MINOR(devno));
		return -1;
	}

	if (!dev->ops || !dev->ops->read) {
		errorf("blk_read: device %s has no read operation", dev->name);
		return -1;
	}

	spinlock_acquire(&dev->lock);
	int ret = dev->ops->read(dev, sector, buf, nsectors);
	spinlock_release(&dev->lock);

	return ret;
}

int blk_write(dev_t devno, u64 sector, const void *buf, u32 nsectors)
{
	struct block_device *dev = blk_lookup(devno);
	if (!dev) {
		errorf("blk_write: device %d:%d not found", MAJOR(devno),
		       MINOR(devno));
		return -1;
	}

	if (!dev->ops || !dev->ops->write) {
		errorf("blk_write: device %s has no write operation",
		       dev->name);
		return -1;
	}

	spinlock_acquire(&dev->lock);
	int ret = dev->ops->write(dev, sector, buf, nsectors);
	spinlock_release(&dev->lock);

	return ret;
}

int blk_flush(dev_t devno)
{
	struct block_device *dev = blk_lookup(devno);
	if (!dev) {
		return -1;
	}

	if (!dev->ops || !dev->ops->flush) {
		return 0;
	}

	spinlock_acquire(&dev->lock);
	int ret = dev->ops->flush(dev);
	spinlock_release(&dev->lock);

	return ret;
}

u64 blk_capacity(dev_t devno)
{
	struct block_device *dev = blk_lookup(devno);
	if (!dev || !dev->ops || !dev->ops->get_capacity) {
		return 0;
	}
	return dev->ops->get_capacity(dev);
}

u32 blk_block_size(dev_t devno)
{
	struct block_device *dev = blk_lookup(devno);
	if (!dev || !dev->ops || !dev->ops->get_block_size) {
		return BLOCK_SIZE;
	}
	return dev->ops->get_block_size(dev);
}
