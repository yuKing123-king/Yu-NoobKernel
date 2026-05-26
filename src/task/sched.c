#include <task/sched.h>
#include <misc/list.h>
#include <misc/log.h>
#include <trap/trap.h>

struct {
	struct list_head queue;
	spinlock_t lock;
} __attribute__((aligned(CACHE_LINE_SIZE))) runq[CPU_NUM];

void init_runq()
{
	for (int i = 0; i < CPU_NUM; i++) {
		INIT_LIST_HEAD(&runq[i].queue);
		runq[i].lock = SPINLOCK_INITIALIZER("runq");
	}
}

bool is_runq_empty(int hartid) { return list_empty(&runq[hartid].queue); }

void enqueue_proc(int hartid, struct proc *p)
{
	spinlock_acquire(&runq[hartid].lock);
	list_add_tail(&p->runq, &runq[hartid].queue);
	spinlock_release(&runq[hartid].lock);
}

struct proc *dequeue_proc(int hartid)
{
	if (is_runq_empty(hartid)) {
		return NULL;
	}
	spinlock_acquire(&runq[hartid].lock);
	struct list_head *lh = runq[hartid].queue.next;
	list_del(lh);
	spinlock_release(&runq[hartid].lock);
	return container_of(lh, struct proc, runq);
}

void sched_yield()
{
	struct cpu *c = thiscpu();
	struct proc *p = c->proc;
	if (p) {
		switch (p->state) {
		case PROC_RUNNING:
			p->state = PROC_RUNNABLE;
			enqueue_proc(r_tp(), p);
			break;
		default:
			break;
		}
	} else
		panic("null proc pointer in %s", __func__);

	thiscpu()->proc = NULL;
	context_switch_yield(p);
}

void context_switch_yield(struct proc *old)
{
	struct cpu *c = thiscpu();
	struct proc *next;

	next = dequeue_proc(r_tp());
	if (!next) {
		infof("cpu %d idle", r_tp());
		next = &c->idle;
	}

	spinlock_acquire(&next->lock);
	if (next != &c->idle) {
		next->state = PROC_RUNNING;
	}
	c->proc = next;
	spinlock_release(&next->lock);

	context_switch(&old->ctx, &next->ctx);
}
