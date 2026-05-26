#pragma once

#include <misc/stddef.h>

int buddy_init();
void *buddy_alloc(size_t size);
void buddy_free(void *ptr);

void buddy_test();
