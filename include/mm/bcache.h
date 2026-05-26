#pragma once

#include <config.h>
#include <misc/stdint.h>
#include <misc/stddef.h>
#include <misc/list.h>
#include <sync/spinlock.h>

struct buf {
	u8 data[BLOCK_SIZE];
	u64 blockno;
	dev_t dev;
	u32 refcnt;
	bool dirty;
	bool valid;
	spinlock_t lock;
	struct list_head lru;
	struct list_head hash;
};

void bcache_init(void);
struct buf *bread(dev_t dev, u64 blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);
void bcache_flush(dev_t dev);
void bcache_sync(void);
