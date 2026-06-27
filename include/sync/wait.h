#pragma once

#include <misc/stddef.h>
#include <sync/spinlock.h>
#include <misc/list.h>

struct proc;

struct wait_queue {
	struct list_head list;
	spinlock_t lock;
};

void wait_queue_init(struct wait_queue *wq);
void wait_queue_sleep(struct wait_queue *wq, struct proc *p);
void wait_queue_wakeup_one(struct wait_queue *wq);
void wait_queue_wakeup_all(struct wait_queue *wq);
int wait_queue_wakeup_n(struct wait_queue *wq, int nr);
int wait_queue_wakeup_addr(struct wait_queue *wq, uintptr_t uaddr, int nr);
int wait_queue_count_addr(struct wait_queue *wq, uintptr_t uaddr);
void wait_queue_sleep_locked(struct wait_queue *wq, struct proc *p, void *lock);
