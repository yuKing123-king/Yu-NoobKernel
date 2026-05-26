#include <sync/spinlock.h>
#include <trap/trap.h>

void spinlock_acquire_bare(spinlock_t *lock)
{
	assert(lock != NULL);

#ifdef SPINLOCK_DEBUG
	u64 hart = r_tp();
	if (lock->locked && lock->owner == hart)
		panic("recursive lock acquire: %s (hart %zu)", lock->name,
		      hart);
#endif

	intr_off();
	while (1) {
		if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) ==
		    0) {
			break;
		}
		asm volatile("wfi" ::: "memory");
	}

#ifdef SPINLOCK_DEBUG
	lock->owner = r_tp();
#endif
}

void spinlock_release_bare(spinlock_t *lock)
{
	assert(lock != NULL);
#ifdef SPINLOCK_DEBUG
	if (!lock->locked) {
		panic("release unlocked lock: %s", lock->name);
	}
	if (lock->owner != r_tp()) {
		panic("spinlock_release: wrong CPU");
	}
	lock->owner = (u64)-1;
#endif
	__atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
	intr_on();
}

int spinlock_holding(spinlock_t *lock)
{
	assert(lock != NULL);

#ifdef SPINLOCK_DEBUG
	u64 hart = r_tp();
	return (lock->locked && lock->owner == hart);
#else
	return lock->locked;
#endif
}
