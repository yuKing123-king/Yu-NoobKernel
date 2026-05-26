#pragma once

#include <config.h>
#include <misc/log.h>

#define SPINLOCK_DEBUG

typedef struct {
	const char *name;
	volatile u64 locked;
#ifdef SPINLOCK_DEBUG
	u64 owner;
#endif
} spinlock_t;

#ifdef SPINLOCK_DEBUG
#define SPINLOCK_INITIALIZER(n)                                                \
	(spinlock_t){.name = n, .locked = 0, .owner = (u64) - 1}

#else
#define SPINLOCK_INITIALIZER(n) (spinlock_t){.name = n, .locked = 0}
#endif

void spinlock_acquire_bare(spinlock_t *lock);
void spinlock_release_bare(spinlock_t *lock);

#define spinlock_acquire(lock)                                                 \
	debugf("spinlock %s accquiring in %s", (lock)->name, __func__);         \
	spinlock_acquire_bare(lock);

#define spinlock_release(lock)                                                 \
	debugf("spinlock %s releasing in %s", (lock)->name, __func__);          \
	spinlock_release_bare(lock);

int spinlock_holding(spinlock_t *lock);
