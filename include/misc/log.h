#pragma once

#include <config.h>
#include <misc/printf.h>
#include <hal/sbi.h>

#define DETAILED_LOG (0)

static void dummy(int _, ...) {}

#if DETAILED_LOG
#define DETAILED_FORMAT "(%s:%d) "
#define DETAILED_PARAMS , __FILE__, __LINE__
#else
#define DETAILED_FORMAT "\t"
#define DETAILED_PARAMS
#endif // DETAILED_LOG

#define LOG_LEVEL_OFF 0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_TRACE 5

enum LOG_COLOR {
	RED = 31,
	GREEN = 32,
	BLUE = 36,
	GRAY = 90,
	YELLOW = 93,
};

#if (LOG_LEVEL >= LOG_LEVEL_ERROR)
#define errorf(fmt, ...)                                                       \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", RED,    \
		       "ERROR" DETAILED_PARAMS, ##__VA_ARGS__);                \
	} while (0)
#else
#define errorf(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif // LOG_LEVEL_ERROR

#if (LOG_LEVEL >= LOG_LEVEL_WARN)
#define warnf(fmt, ...)                                                        \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", YELLOW, \
		       "WARN" DETAILED_PARAMS, ##__VA_ARGS__);                 \
	} while (0)
#else
#define warnf(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif // LOG_LEVEL_WARN

#if (LOG_LEVEL >= LOG_LEVEL_INFO)
#define infof(fmt, ...)                                                        \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", BLUE,   \
		       "INFO" DETAILED_PARAMS, ##__VA_ARGS__);                 \
	} while (0)
#else
#define infof(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif // LOG_LEVEL_INFO

#if (LOG_LEVEL >= LOG_LEVEL_DEBUG)
#define debugf(fmt, ...)                                                       \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", GREEN,  \
		       "DEBUG" DETAILED_PARAMS, ##__VA_ARGS__);                \
	} while (0)
#else
#define debugf(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif // LOG_LEVEL_DEBUG

#if (LOG_LEVEL >= LOG_LEVEL_TRACE)
#define tracef(fmt, ...)                                                       \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", GRAY,   \
		       "TRACE" DETAILED_PARAMS, ##__VA_ARGS__);                \
	} while (0)
#else
#define tracef(fmt, ...) dummy(0, ##__VA_ARGS__)
#endif // LOG_LEVEL_TRACE

#define panic(fmt, ...)                                                        \
	do {                                                                   \
		printf("\x1b[%dm[%s]" DETAILED_FORMAT fmt "\x1b[0m\n", RED,    \
		       "PANIC" DETAILED_PARAMS, ##__VA_ARGS__);                \
		sbi_shutdown();                                                \
		__builtin_unreachable();                                       \
	} while (0)

#define assert(x)                                                              \
	do {                                                                   \
		if (!(x))                                                      \
			panic("dynamic assertion failed: %s:%d", __FILE__,     \
			      __LINE__);                                       \
	} while (0)

