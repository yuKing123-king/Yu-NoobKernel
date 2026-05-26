#pragma once

#include <task/proc.h>

extern void context_switch(struct context *old, struct context *new);
extern void context_switch_to(struct context *new);
void init_runq();
bool is_runq_empty(int hartid);
void enqueue_proc(int hartid, struct proc *p);
struct proc *dequeue_proc(int hartid);
void sched_yield();
void context_switch_yield(struct proc *old);
