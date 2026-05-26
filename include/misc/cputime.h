#pragma once

#include <config.h>
#include <misc/stdint.h>

static inline uint64_t ns_to_cputime(uint64_t ns)
{
	return (ns * TIMEBASE_FREQ) / 1000000000ULL;
}

static inline uint64_t us_to_cputime(uint64_t us)
{
	return (us * TIMEBASE_FREQ) / 1000000ULL;
}

static inline uint64_t ms_to_cputime(uint64_t ms)
{
	return (ms * TIMEBASE_FREQ) / 1000ULL;
}

static inline uint64_t sec_to_cputime(uint64_t sec)
{
	return sec * TIMEBASE_FREQ;
}

static inline uint64_t cputime_to_ns(uint64_t cputime)
{
	return (cputime * 1000000000ULL) / TIMEBASE_FREQ;
}

static inline uint64_t cputime_to_us(uint64_t cputime)
{
	return (cputime * 1000000ULL) / TIMEBASE_FREQ;
}

static inline uint64_t cputime_to_ms(uint64_t cputime)
{
	return (cputime * 1000ULL) / TIMEBASE_FREQ;
}
