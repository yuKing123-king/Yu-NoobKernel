#include <sync/wait.h>
#include <task/proc.h>
#include <task/sched.h>
#include <misc/log.h>
#include <sync/barrier.h>

void wait_queue_init(struct wait_queue *wq)
{
	INIT_LIST_HEAD(&wq->list);
	wq->lock = SPINLOCK_INITIALIZER("waitq");
}

void wait_queue_sleep(struct wait_queue *wq, struct proc *p)
{
	spinlock_acquire(&wq->lock);
	list_add_tail(&p->runq, &wq->list);
	p->state = PROC_SLEEPING;
	spinlock_release(&wq->lock);

	sched_yield();
}

static int wait_queue_wakeup(struct wait_queue *wq, int nr)
{
	struct proc *waken = NULL;
	int count = 0;

	spinlock_acquire(&wq->lock);
	while (!list_empty(&wq->list)) {
		struct list_head *lh = wq->list.next;
		list_del(lh);
		struct proc *p = list_entry(lh, struct proc, runq);
		p->state = PROC_RUNNABLE;
		enqueue_proc(r_tp(), p);
		waken = p;
		count++;
		if (nr > 0 && count >= nr)
			break;
	}
	spinlock_release(&wq->lock);

	if (waken) {
		thiscpu()->need_resched = true;
	}
	return count;
}

void wait_queue_wakeup_one(struct wait_queue *wq)
{
	wait_queue_wakeup(wq, 1);
}

void wait_queue_wakeup_all(struct wait_queue *wq)
{
	wait_queue_wakeup(wq, -1);
}

int wait_queue_wakeup_n(struct wait_queue *wq, int nr)
{
	if (nr <= 0)
		return 0;
	return wait_queue_wakeup(wq, nr);
}

int wait_queue_wakeup_addr(struct wait_queue *wq, uintptr_t uaddr, int nr)
{
	struct proc *waken = NULL;
	struct list_head *pos, *n;
	int count = 0;

	if (nr <= 0)
		return 0;

	spinlock_acquire(&wq->lock);
	list_for_each_safe(pos, n, &wq->list) {
		struct proc *p = list_entry(pos, struct proc, runq);

		if (p->futex_uaddr != uaddr)
			continue;

		list_del(pos);
		p->state = PROC_RUNNABLE;
		p->futex_uaddr = 0;
		enqueue_proc(r_tp(), p);
		waken = p;
		count++;
		if (count >= nr)
			break;
	}
	spinlock_release(&wq->lock);

	if (waken)
		thiscpu()->need_resched = true;
	return count;
}

int wait_queue_count_addr(struct wait_queue *wq, uintptr_t uaddr)
{
	struct list_head *pos;
	int count = 0;

	spinlock_acquire(&wq->lock);
	list_for_each(pos, &wq->list) {
		struct proc *p = list_entry(pos, struct proc, runq);

		if (p->futex_uaddr == uaddr)
			count++;
	}
	spinlock_release(&wq->lock);
	return count;
}

/*
 * 原子地将进程挂到等待队列，然后释放外部锁再让出 CPU。
 * 调用者必须已持有 lock，本函数负责释放它。
 * 这消除了 "释放锁 → sleep" 之间的竞态窗口。
 */
void wait_queue_sleep_locked(struct wait_queue *wq, struct proc *p, void *lock)
{
	spinlock_acquire(&wq->lock);
	list_add_tail(&p->runq, &wq->list);
	p->state = PROC_SLEEPING;
	spinlock_release(&wq->lock);

	spinlock_release((spinlock_t *)lock);
	sched_yield();
}
