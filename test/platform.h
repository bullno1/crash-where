/**
 * @file platform.h
 * Operating-system specific parts of the test harness.
 */
#ifndef CW_TEST_PLATFORM_H
#define CW_TEST_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#	include <intrin.h>
#	define TEST_NOINLINE __declspec(noinline)
#	define TEST_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
#else
#	define TEST_NOINLINE __attribute__((noinline))
#	define TEST_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#endif

/**
 * How a child process ended.
 */
typedef struct {
	bool signaled;  /**< Ended by a signal; `code` is the signal number. */
	int code;       /**< Exit status, or the signal number when `signaled`. */
} test_exit_t;

/**
 * Run this executable again with extra environment variables.
 *
 * Returns once the child and every process that inherited its stdout,
 * which includes the watcher, have exited. The child's stdout is
 * forwarded to stderr.
 *
 * @param env  `NULL`-terminated list of `NAME=value` strings added to the environment.
 * @param out  Receives how the child ended.
 * @return `false` when the child could not be started.
 */
bool
test_spawn_self(const char* const* env, test_exit_t* out);

/**
 * Create one directory level. Succeeds when it already exists.
 */
bool
test_mkdir(const char* path);

/**
 * Load address of this executable: the base a symbolizer subtracts
 * from an absolute address to get a module offset.
 */
uintptr_t
test_image_base(void);

#endif /* CW_TEST_PLATFORM_H */
