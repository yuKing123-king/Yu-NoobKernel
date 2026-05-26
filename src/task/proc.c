#include <task/proc.h>
#include <config.h>
#include <hal/riscv.h>
#include <mm/kalloc.h>
#include <mm/pagetable.h>
#include <mm/vma.h>
#include <sync/atomic.h>
#include <misc/string.h>
#include <fs/fd_table.h>
#include <fs/vfs.h>

extern pagetable_t kpagetable;

static struct cpu cpus[CPU_NUM];
extern void idle_loop(void);
static atomic64_t next_pid = ATOMIC64_INIT(PID_MIN);

pid_t alloc_pid(void)
{
	int64_t current;
	int pid;

	do {
		current = atomic64_read(&next_pid);

		if (current > PID_MAX) {
			// 理论上几乎不可能发生（需分配 21 亿次）
			// 可选择 panic、回绕或返回错误
			// 这里简单回绕到 PID_MIN
			current = atomic64_cmpxchg(&next_pid, current, PID_MIN);
			// 重试
			continue;
		}

		// 尝试将 next_pid 从 current 增加到 current+1
		int64_t new_val = current + 1;
		if (atomic64_cmpxchg(&next_pid, current, new_val) == current) {
			pid = (int)
			    current; // safe: current <= PID_MAX = INT32_MAX
			break;
		}
		// 否则重试（其他 CPU 修改了 next_pid）

	} while (1);

	return pid;
}

void init_cpu(u64 id)
{
	struct cpu *c = &cpus[id];
	c->need_resched = false;
	c->idle.ctx.ra = (uintptr_t)idle_loop;
	c->idle.ctx.sp = (uintptr_t)c->idle_stack + IDLE_STACK_SIZE;
	c->idle.ctx.sstatus = SSTATUS_SIE;
	c->idle.state = PROC_IDLE;
	strcpy(c->idle.comm, "idle");
	c->idle.parent = NULL;
	INIT_LIST_HEAD(&c->idle.sibling);
	c->idle.lock = SPINLOCK_INITIALIZER("idle");
	c->proc = &c->idle;
	w_gp((uintptr_t)&c->idle.ktf);
	w_tp(id);
}

inline struct cpu *thiscpu() { return &cpus[r_tp()]; }

struct proc *alloc_proc()
{
	struct proc *p = kzalloc(sizeof(struct proc));
	if (!p)
		return NULL;

	p->state = PROC_UNUSED;
	p->ctx.gp = (uintptr_t)&p->ktf;
	p->ctx.sstatus = SSTATUS_SIE;

	p->fd_table = fd_table_alloc();
	if (!p->fd_table) {
		kfree(p);
		return NULL;
	}

	p->pwd = vfs_get_root();
	if (p->pwd) {
		dentry_get(p->pwd);
	}

	return p;
}

// 调用前需确保进程不在调度队列中，且没有子进程
void free_proc(struct proc *p)
{
	if (!p)
		return;

	if (p->parent) {
		spinlock_acquire(&p->parent->lock);
		list_del(&p->sibling);
		spinlock_release(&p->parent->lock);
		p->parent = NULL;
	}

	if (p->kstack)
		kfree(p->kstack);

	if (p->tf)
		kfree(p->tf);

	if (p->pagetable && p->pagetable != kpagetable) {
		struct list_head *pos, *n;
		list_for_each_safe(pos, n, &p->vma)
		{
			struct vma *vma = list_entry(pos, struct vma, list);
			vma_remove(p, vma->start, vma->length);
		}
		pagetable_destroy(p->pagetable);
	}

	if (p->fd_table) {
		fd_table_free(p->fd_table);
		p->fd_table = NULL;
	}

	if (p->pwd) {
		dentry_put(p->pwd);
		p->pwd = NULL;
	}

	kfree(p);
}
