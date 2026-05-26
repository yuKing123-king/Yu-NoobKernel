#pragma once

#include <misc/list.h>
#include <misc/stddef.h>
#include <sync/spinlock.h>
#include <fs/fd_table.h>
#include <fs/dentry.h>

#define PROC_UNUSED 0
#define PROC_IDLE 1
#define PROC_RUNNABLE 2
#define PROC_RUNNING 3
#define PROC_SLEEPING 4
#define PROC_ZOMBIE 5

struct context {
	u64 ra;
	u64 sp;
	u64 gp;

	// callee-saved
	u64 s0;
	u64 s1;
	u64 s2;
	u64 s3;
	u64 s4;
	u64 s5;
	u64 s6;
	u64 s7;
	u64 s8;
	u64 s9;
	u64 s10;
	u64 s11;

	// CSR
	u64 sstatus;
};

struct ktrapframe {
	u64 sepc;
	u64 sstatus;
	u64 scause;

	u64 ra;
	u64 sp;
	u64 gp;
	u64 tp;
	u64 t0;
	u64 t1;
	u64 t2;
	u64 s0;
	u64 s1;
	u64 a0;
	u64 a1;
	u64 a2;
	u64 a3;
	u64 a4;
	u64 a5;
	u64 a6;
	u64 a7;
	u64 s2;
	u64 s3;
	u64 s4;
	u64 s5;
	u64 s6;
	u64 s7;
	u64 s8;
	u64 s9;
	u64 s10;
	u64 s11;
	u64 t3;
	u64 t4;
	u64 t5;
	u64 t6;
};

struct proc {
	char comm[16];
	pid_t pid;
	pid_t tgid;

	pagetable_t pagetable;
	struct list_head vma;

	struct context ctx;
	struct ktrapframe ktf;
	struct trapframe *tf;
	void *kstack;

	int state;
	struct list_head runq;

	struct proc *parent;
	struct list_head children;
	struct list_head sibling;

	struct fd_table *fd_table;
	struct dentry *pwd;

	spinlock_t lock;
};

struct cpu {
	struct proc *proc;
	struct proc idle;
	u64 intr_state;
	int intr_depth;
	bool need_resched;
	u8 idle_stack[IDLE_STACK_SIZE] __attribute__((aligned(16)));
};

void init_cpu(u64 id);
struct cpu *thiscpu();

struct proc *alloc_proc();
void free_proc(struct proc *p);
