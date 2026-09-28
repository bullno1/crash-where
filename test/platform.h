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

/* Exception types the library reports for the scenarios. */
#if defined(_WIN32)
#	define TEST_EXC_SEGV           "EXCEPTION_ACCESS_VIOLATION"
#	define TEST_EXC_STACK_OVERFLOW "EXCEPTION_STACK_OVERFLOW"
#	define TEST_EXC_ABORT          "ABORT"
#else
#	define TEST_EXC_SEGV           "SIGSEGV"
#	define TEST_EXC_STACK_OVERFLOW "SIGSEGV"
#	define TEST_EXC_ABORT          "SIGABRT"
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
 * Sleep for `ms` milliseconds.
 */
void
test_sleep_ms(unsigned ms);

/**
 * Size of a file in bytes, or -1 when it cannot be read.
 */
long
test_file_size(const char* path);

/**
 * Stop this process the way a job-control stop or a debugger break
 * would, and resume it after `ms` milliseconds.
 *
 * @return `false` when the stop could not be arranged.
 */
bool
test_stop_self(unsigned ms);

/**
 * Entry point of the helper process that test_stop_self() spawns on
 * platforms where only another process can stop this one.
 *
 * @param spec  Value of `CW_TEST_STOP` in the helper's environment.
 * @return The helper's exit status; 0 when the stop and resume happened.
 */
int
test_stop_helper_main(const char* spec);

/**
 * Load address of this executable: the base a symbolizer subtracts
 * from an absolute address to get a module offset.
 */
uintptr_t
test_image_base(void);

/**
 * Whether this is a Windows build running under Wine, which kills a
 * process instead of dispatching an exception once its stack is nearly
 * exhausted.
 */
bool
test_under_wine(void);

/**
 * Replace the library's crash handler with an inert one, as a middleware
 * SDK would.
 *
 * @return `false` when the platform has no way to recover from that,
 *         in which case a test relying on recovery should skip.
 */
bool
test_displace_crash_handler(void);

#endif /* CW_TEST_PLATFORM_H */
