#pragma once

struct proc *kthread_create(int (*fn)(void *), void *arg, const char *name);
void kthread_exit(void) __attribute__((noreturn));
