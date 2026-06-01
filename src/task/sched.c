#include <task/sched.h>
#include <misc/list.h>
#include <misc/log.h>
#include <trap/trap.h>

struct {
	struct list_head queue;
	spinlock_t lock;
} __attribute__((aligned(CACHE_LINE_SIZE))) runq[CPU_NUM];

/*
 * 初始化所有CPU核心的运行队列
 */
void init_runq()
{
	for (int i = 0; i < CPU_NUM; i++) {
		INIT_LIST_HEAD(&runq[i].queue);
		runq[i].lock = SPINLOCK_INITIALIZER("runq");
	}
}

/*
 * 检查指定CPU核心的运行队列是否为空
 * @param hartid: CPU核心ID
 * @return: 队列为空返回true，否则返回false
 */
bool is_runq_empty(int hartid) { return list_empty(&runq[hartid].queue); }

/*
 * 将进程加入指定CPU核心的运行队列尾部
 * @param hartid: 目标CPU核心ID
 * @param p: 待入队的进程结构指针
 */
void enqueue_proc(int hartid, struct proc *p)
{
	spinlock_acquire(&runq[hartid].lock);
	list_add_tail(&p->runq, &runq[hartid].queue);
	spinlock_release(&runq[hartid].lock);
}

/*
 * 从指定CPU核心的运行队列头部取出一个进程
 * @param hartid: CPU核心ID
 * @return: 成功返回进程指针，队列为空返回NULL
 */
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

/*
 * 主动让出CPU，触发调度切换到下一个可运行进程
 */
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

/*
 * 执行上下文切换：从当前进程切换到下一个可运行进程（或idle）
 * @param old: 当前进程的结构指针
 */
void context_switch_yield(struct proc *old)
{
	struct cpu *c = thiscpu();
	struct proc *next;

	next = dequeue_proc(r_tp());
	if (!next) {
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
