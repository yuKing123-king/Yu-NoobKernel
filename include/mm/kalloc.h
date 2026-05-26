#pragma once

#include <misc/stddef.h>

void kalloc_init();
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t nitems, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

void kalloc_test();
