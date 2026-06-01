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

/*
 * 计算块设备号和块号对应的哈希值
 * @param dev: 设备号
 * @param blockno: 块号
 * @return: 哈希桶索引
 */
static inline u32 bcache_hash(dev_t dev, u64 blockno)
{
	return (dev + blockno) % BCACHE_HASH_SIZE;
}

/*
 * 初始化块缓存系统：初始化哈希表、LRU链表和所有缓冲区
 * @return: 无返回值
 */
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

/*
 * 在哈希表中查找指定设备号和块号的缓冲区
 * @param dev: 设备号
 * @param blockno: 块号
 * @return: 找到的缓冲区指针，未找到返回NULL
 */
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

/*
 * 从LRU链表中淘汰一个缓冲区（脏块会先写回磁盘）
 * @return: 被淘汰的缓冲区指针，无可用缓冲区时返回NULL
 */
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
