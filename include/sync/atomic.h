#pragma once

#include <misc/stddef.h>

/* ------------------------ */
/* Helper: READ_ONCE / WRITE_ONCE semantics */
/* ------------------------ */

#define READ_ONCE(x)                                                           \
	(__builtin_choose_expr(                                                \
	    __builtin_types_compatible_p(typeof(x), typeof(x)),                \
	    __atomic_load_n(&(x), __ATOMIC_RELAXED), (void)0))

#define WRITE_ONCE(x, val)                                                     \
	(__builtin_choose_expr(                                                \
	    __builtin_types_compatible_p(typeof(x), typeof(x)),                \
	    __atomic_store_n(&(x), (val), __ATOMIC_RELAXED), (void)0))

/* ------------------------ */
/* 32-bit atomic type       */
/* ------------------------ */

typedef struct {
	volatile int32_t counter;
} atomic_t;

#define ATOMIC_INIT(i) {(i)}

static inline int32_t atomic_read(const atomic_t *v)
{
	return READ_ONCE(v->counter);
}

static inline void atomic_set(atomic_t *v, int32_t i)
{
	WRITE_ONCE(v->counter, i);
}

static inline int32_t atomic_add_return(int32_t i, atomic_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int32_t atomic_sub_return(int32_t i, atomic_t *v)
{
	return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int32_t atomic_fetch_add(int32_t i, atomic_t *v)
{
	return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int32_t atomic_inc(atomic_t *v)
{
	return atomic_add_return(1, v);
}

static inline int32_t atomic_dec(atomic_t *v)
{
	return atomic_sub_return(1, v);
}

static inline int32_t atomic_cmpxchg(atomic_t *v, int32_t old_val,
				     int32_t new_val)
{
	/* Returns actual old value (like Linux kernel) */
	__atomic_compare_exchange_n(&v->counter, &old_val, new_val, false,
				    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
	return old_val;
}

static inline int32_t atomic_xchg(atomic_t *v, int32_t new_val)
{
	return __atomic_exchange_n(&v->counter, new_val, __ATOMIC_SEQ_CST);
}

/* ------------------------ */
/* 64-bit atomic type       */
/* ------------------------ */

typedef struct {
	volatile int64_t counter;
} atomic64_t;

#define ATOMIC64_INIT(i) {(i)}

static inline int64_t atomic64_read(const atomic64_t *v)
{
	return READ_ONCE(v->counter);
}

static inline void atomic64_set(atomic64_t *v, int64_t i)
{
	WRITE_ONCE(v->counter, i);
}

static inline int64_t atomic64_add_return(int64_t i, atomic64_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic64_sub_return(int64_t i, atomic64_t *v)
{
	return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic64_fetch_add(int64_t i, atomic64_t *v)
{
	return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int64_t atomic64_inc(atomic64_t *v)
{
	return atomic64_add_return(1, v);
}

static inline int64_t atomic64_dec(atomic64_t *v)
{
	return atomic64_sub_return(1, v);
}

static inline int64_t atomic64_cmpxchg(atomic64_t *v, int64_t old_val,
				       int64_t new_val)
{
	__atomic_compare_exchange_n(&v->counter, &old_val, new_val, false,
				    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
	return old_val;
}

static inline int64_t atomic64_xchg(atomic64_t *v, int64_t new_val)
{
	return __atomic_exchange_n(&v->counter, new_val, __ATOMIC_SEQ_CST);
}

/* ------------------------ */
/* Pointer-sized atomic (rv64 = 64-bit) */
/* ------------------------ */

typedef atomic64_t atomic_ptr_t;
#define ATOMIC_PTR_INIT(i) ATOMIC64_INIT((int64_t)(uintptr_t)(i))

static inline void *atomic_ptr_read(const atomic_ptr_t *v)
{
	return (void *)(uintptr_t)atomic64_read(v);
}

static inline void atomic_ptr_set(atomic_ptr_t *v, void *p)
{
	atomic64_set(v, (int64_t)(uintptr_t)p);
}

static inline void *atomic_ptr_xchg(atomic_ptr_t *v, void *new_ptr)
{
	return (void *)(uintptr_t)atomic64_xchg(v, (int64_t)(uintptr_t)new_ptr);
}

static inline void *atomic_ptr_cmpxchg(atomic_ptr_t *v, void *old_ptr,
				       void *new_ptr)
{
	return (void *)(uintptr_t)atomic64_cmpxchg(
	    v, (int64_t)(uintptr_t)old_ptr, (int64_t)(uintptr_t)new_ptr);
}
