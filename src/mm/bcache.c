#include <mm/bcache.h>
#include <hal/blk.h>
#include <misc/string.h>
#include <misc/log.h>
#include <sync/barrier.h>

static struct {
	struct buf bufs[BCACHE_SIZE];
	struct list_head hash_table[BCACHE_HASH_SIZE];
	struct list_head lru_list;
	spinlock_t lock;
} bcache;

static inline u32 bcache_hash(dev_t dev, u64 blockno)
{
	return (dev + blockno) % BCACHE_HASH_SIZE;
}

void bcache_init(void)
{
	spinlock_acquire(&bcache.lock);

	INIT_LIST_HEAD(&bcache.lru_list);
	for (int i = 0; i < BCACHE_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&bcache.hash_table[i]);
	}

	for (int i = 0; i < BCACHE_SIZE; i++) {
		struct buf *b = &bcache.bufs[i];
		b->dev = 0;
		b->blockno = 0;
		b->refcnt = 0;
		b->dirty = false;
		b->valid = false;
		b->lock = SPINLOCK_INITIALIZER("bcache_buf");
		INIT_LIST_HEAD(&b->hash);
		INIT_LIST_HEAD(&b->lru);
		list_add(&b->lru, &bcache.lru_list);
	}

	spinlock_release(&bcache.lock);
	infof("bcache initialized: %d buffers, %d hash buckets", BCACHE_SIZE,
	      BCACHE_HASH_SIZE);
}

static struct buf *bcache_lookup(dev_t dev, u64 blockno)
{
	u32 hash = bcache_hash(dev, blockno);
	struct list_head *head = &bcache.hash_table[hash];

	struct buf *b;
	list_for_each_entry(b, head, hash)
	{
		if (b->dev == dev && b->blockno == blockno) {
			return b;
		}
	}
	return NULL;
}

static struct buf *bcache_evict(void)
{
	struct buf *b;
	list_for_each_entry_reverse(b, &bcache.lru_list, lru)
	{
		if (b->refcnt == 0) {
			if (b->dirty) {
				blk_write(b->dev, b->blockno, b->data, 1);
				b->dirty = false;
			}
			list_del(&b->hash);
			b->valid = false;
			return b;
		}
	}
	return NULL;
}

static struct buf *bcache_get(dev_t dev, u64 blockno)
{
	spinlock_acquire(&bcache.lock);

	struct buf *b = bcache_lookup(dev, blockno);
	if (b) {
		b->refcnt++;
		list_del(&b->lru);
		list_add(&b->lru, &bcache.lru_list);
		spinlock_release(&bcache.lock);
		spinlock_acquire(&b->lock);
		return b;
	}

	b = bcache_evict();
	if (!b) {
		panic("bcache: no free buffers");
	}

	b->dev = dev;
	b->blockno = blockno;
	b->refcnt = 1;
	b->valid = false;
	b->dirty = false;

	u32 hash = bcache_hash(dev, blockno);
	list_add(&b->hash, &bcache.hash_table[hash]);
	list_del(&b->lru);
	list_add(&b->lru, &bcache.lru_list);

	spinlock_release(&bcache.lock);
	spinlock_acquire(&b->lock);

	return b;
}

struct buf *bread(dev_t dev, u64 blockno)
{
	struct buf *b = bcache_get(dev, blockno);

	if (!b->valid) {
		blk_read(dev, blockno, b->data, 1);
		b->valid = true;
	}

	return b;
}

void bwrite(struct buf *b)
{
	if (!b->valid) {
		warnf("bwrite: buffer not valid");
		return;
	}

	b->dirty = true;
	blk_write(b->dev, b->blockno, b->data, 1);
	b->dirty = false;
}

void brelse(struct buf *b)
{
	spinlock_release(&b->lock);

	spinlock_acquire(&bcache.lock);
	b->refcnt--;
	if (b->refcnt == 0) {
		list_del(&b->lru);
		list_add_tail(&b->lru, &bcache.lru_list);
	}
	spinlock_release(&bcache.lock);
}

void bcache_flush(dev_t dev)
{
	spinlock_acquire(&bcache.lock);

	struct buf *b;
	list_for_each_entry(b, &bcache.lru_list, lru)
	{
		if (b->dev == dev && b->dirty) {
			spinlock_acquire(&b->lock);
			blk_write(b->dev, b->blockno, b->data, 1);
			b->dirty = false;
			spinlock_release(&b->lock);
		}
	}

	spinlock_release(&bcache.lock);
}

void bcache_sync(void)
{
	spinlock_acquire(&bcache.lock);

	struct buf *b;
	list_for_each_entry(b, &bcache.lru_list, lru)
	{
		if (b->dirty) {
			spinlock_acquire(&b->lock);
			blk_write(b->dev, b->blockno, b->data, 1);
			b->dirty = false;
			spinlock_release(&b->lock);
		}
	}

	spinlock_release(&bcache.lock);
}
