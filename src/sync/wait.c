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
	spinlock_release(&wq->lock);

	p->state = PROC_SLEEPING;
	sched_yield();
}

static void wait_queue_wakeup(struct wait_queue *wq, int all)
{
	struct proc *waken = NULL;

	spinlock_acquire(&wq->lock);
	while (!list_empty(&wq->list)) {
		struct list_head *lh = wq->list.next;
		list_del(lh);
		struct proc *p = list_entry(lh, struct proc, runq);
		p->state = PROC_RUNNABLE;
		enqueue_proc(r_tp(), p);
		waken = p;
		if (!all)
			break;
	}
	spinlock_release(&wq->lock);

	if (waken) {
		thiscpu()->need_resched = true;
	}
}

void wait_queue_wakeup_one(struct wait_queue *wq)
{
	wait_queue_wakeup(wq, 0);
}

void wait_queue_wakeup_all(struct wait_queue *wq)
{
	wait_queue_wakeup(wq, 1);
}
