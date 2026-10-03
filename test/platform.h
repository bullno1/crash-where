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
#elif defined(__EMSCRIPTEN__)
#	define TEST_NOINLINE __attribute__((noinline))
/* A Wasm function cannot read its return address; frames are checked by other means. */
#	define TEST_RETURN_ADDRESS() ((uintptr_t)0)
#else
#	define TEST_NOINLINE __attribute__((noinline))
#	define TEST_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#endif

/*
 * Where a scenario stores to die of an access fault. Address 0 is
 * ordinary memory in Wasm; only a store past the end of memory traps.
 */
#if defined(__EMSCRIPTEN__)
#	define TEST_BAD_ADDRESS ((uintptr_t)0xfffffff0u)
#else
#	define TEST_BAD_ADDRESS ((uintptr_t)0)
#endif

/* Whether the watcher writes a minidump beside a crash or hang report. */
#if defined(_WIN32)
#	define TEST_HAS_MINIDUMP 1
#else
#	define TEST_HAS_MINIDUMP 0
#endif

/* Whether the transport names itself in `User-Agent`. A browser sends its own. */
#if defined(__EMSCRIPTEN__)
#	define TEST_HTTP_OWN_USER_AGENT 0
#else
#	define TEST_HTTP_OWN_USER_AGENT 1
#endif

/* Exception types the library reports for the scenarios. */
#if defined(_WIN32)
#	define TEST_EXC_SEGV           "EXCEPTION_ACCESS_VIOLATION"
#	define TEST_EXC_STACK_OVERFLOW "EXCEPTION_STACK_OVERFLOW"
#	define TEST_EXC_ABORT          "ABORT"
#elif defined(__EMSCRIPTEN__)
#	define TEST_EXC_SEGV           "OUT_OF_BOUNDS"
#	define TEST_EXC_STACK_OVERFLOW "STACK_OVERFLOW"
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
 * Run `fn` on a new thread with the platform's default stack and wait
 * for it to return.
 *
 * @return `false` when the thread could not be started.
 */
bool
test_run_thread(void (*fn)(void));

/**
 * Prepare this process for the tests.
 */
void
test_platform_init(void);

/**
 * Create one directory level. Succeeds when it already exists.
 */
bool
test_mkdir(const char* path);

/**
 * Delete a directory with everything in it. Succeeds when it is absent.
 */
bool
test_remove_tree(const char* path);

/**
 * Number of entries of a directory whose names end with `suffix`, or -1
 * when it cannot be read.
 */
int
test_count_files(const char* dir, const char* suffix);

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
 * Put this process under a debugger for the rest of its life, as when
 * it is launched from one.
 *
 * The debugger never resumes a process that a signal stopped, so the
 * caller must end by exiting.
 *
 * @return `false` when no debugger could be attached.
 */
bool
test_debug_self(void);

/**
 * Entry point of the helper process that test_stop_self() and
 * test_debug_self() spawn on platforms where only another process can
 * stop or debug this one.
 *
 * @param spec  Value of `CW_TEST_STOP` in the helper's environment.
 * @return The helper's exit status; 0 when it did what was asked.
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
 * Path of this executable, for tools that read it from disk.
 *
 * @return `false` when it does not fit `cap`.
 */
bool
test_self_path(char* buf, size_t cap);

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

/**
 * Whether the host language can throw through C frames, as a
 * JavaScript import does on the web.
 */
bool
test_host_can_throw(void);

/**
 * Throw a `TypeError` from the host language, with the message `host`.
 * Does not return where test_host_can_throw(); does nothing elsewhere.
 */
void
test_host_throw(void);

#endif /* CW_TEST_PLATFORM_H */
