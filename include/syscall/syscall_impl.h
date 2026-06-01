#pragma once

#include <misc/stddef.h>

typedef uintptr_t (*syscall_fn_t)(uintptr_t, uintptr_t, uintptr_t,
                                   uintptr_t, uintptr_t, uintptr_t);

#define SYSCALL_MAX 512

void syscall_register(int nr, syscall_fn_t fn);
syscall_fn_t syscall_lookup(int nr);
