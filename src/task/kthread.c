// kthread.c
#include <task/proc.h>
#include <task/sched.h>
#include <mm/kalloc.h>
#include <misc/string.h>
#include <misc/log.h>

extern void kthread_start(void); // 汇编入口
extern pid_t alloc_pid();
extern pagetable_t kpagetable;

struct proc *kthread_create(int (*fn)(void *), void *arg, const char *name)
{
	struct proc *p = alloc_proc();
	if (!p)
		return NULL;

	// 1. 设置基本信息
	if (name)
		strncpy(p->comm, name, sizeof(p->comm));
	p->pid = alloc_pid();
	p->tgid = p->pid;
	p->pagetable = kpagetable; // 共享内核页表
	p->lock = SPINLOCK_INITIALIZER(name);
	// 2. 分配内核栈（1 页足够）
	p->kstack = kmalloc(PAGE_SIZE);
	if (!p->kstack) {
		free_proc(p);
		return NULL;
	}

	// 3. 初始化上下文
	// 栈顶对齐（RISC-V ABI 要求 16 字节对齐）
	u64 stack_top = (u64)p->kstack + PAGE_SIZE;
	stack_top &= ~0xF; // 16-byte align

	// 设置初始 context
	p->ctx.ra = (u64)kthread_start; // 返回地址 = kthread_start
	p->ctx.sp = stack_top;

	// 将 fn 和 arg 压入栈中，供 kthread_start 使用
	u64 *args = (u64 *)(stack_top - 16);
	args[0] = (u64)fn;
	args[1] = (u64)arg;

	// 调整 sp，为参数留出空间（kthread_start 会读取）
	p->ctx.sp = stack_top - 16;

	// 4. 设置状态为 RUNNABLE
	p->state = PROC_RUNNABLE;

	// 5. 加入调度队列（当前 CPU）
	enqueue_proc(r_tp(), p);

	return p;
}

void kthread_exit(int ret_code)
{
	struct proc *p = thiscpu()->proc;
	if (!p)
		panic("kthread_exit: no current proc");

	infof("kthread %s exited with code %d", p->comm, ret_code);
	// 1. 从 runqueue 中移除（如果还在）
	// 但通常线程退出时不在 runqueue（正在运行）
	// 所以直接标记状态

	// 2. 释放资源
	// 注意：不能在这里直接 free_proc，因为还需要切换出去
	// 正确做法：标记为 ZOMBIE，由调度器或 idle 回收

	p->state = PROC_ZOMBIE;

	// 3. 触发调度，切换到其他线程
	sched_yield();

	// 4. 理论上不会到这里，但为了安全
	panic("kthread_exit returned");
}
