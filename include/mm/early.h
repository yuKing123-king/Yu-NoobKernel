#pragma once

#include <misc/stddef.h>

void early_init();
bool is_early_mem(void *ptr);
void *early_alloc(size_t size);
