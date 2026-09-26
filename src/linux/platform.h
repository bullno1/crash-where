/**
 * @file linux/platform.h
 * Linux part of the shared region and the Linux process state.
 */
#ifndef CW_LINUX_PLATFORM_H
#define CW_LINUX_PLATFORM_H

#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <ucontext.h>

#include "internal.h"

#define CW_STACK_CAP (256u * 1024u)

/**
 * Crash-time record written by the signal handler in the game and
 * read by the supervisor.
 *
 * `state` is 0 while idle, 2 while a handler is filling the record, and
 * 1 once it is complete.
 */
typedef struct {
	_Atomic uint32_t state;
	int signo;
	pid_t tid;
	siginfo_t si;
	ucontext_t uc;
	uintptr_t sp;              /**< Stack pointer of the crashing thread. */
	uintptr_t stack_top;       /**< Upper bound the copy was clamped to. */
	size_t stack_len;          /**< Bytes copied into `stack`, starting at `sp`. */
	uint8_t stack[CW_STACK_CAP];
} cw_crash_t;

/**
 * Full layout of the memfd region on Linux.
 */
typedef struct {
	cw_shared_t common;
	cw_crash_t crash;
} cw_region_t;

/**
 * Message types on the game/watcher socket.
 */
typedef enum {
	CW_MSG_CRASH    = 'C',     /**< Game: crash record is complete. */
	CW_MSG_SHUTDOWN = 'S',     /**< Game: exiting on purpose, `value` is the result. */
	CW_MSG_AUTH     = 'A',     /**< Game: token file refreshed. */
	CW_MSG_READY    = 'R',     /**< Watcher: region mapped, watching. */
	CW_MSG_DONE     = 'D',     /**< Watcher: report written, game may die. */
} cw_msg_type_t;

typedef struct {
	uint8_t type;
	uint8_t pad[3];
	int32_t value;
} cw_msg_t;

typedef struct {
	cw_region_t* region;
	int sock;                  /**< Game's end of the socketpair. */
	pid_t watcher;
	uintptr_t main_stack_lo;
	uintptr_t main_stack_hi;
} cw_linux_t;

extern cw_linux_t cw_linux;

/** Send one message. Async-signal-safe. */
bool
cw_send_msg(int sock, uint8_t type, int32_t value);

/**
 * Wait up to `timeout_ms` for one message. Async-signal-safe.
 *
 * @return `false` on timeout, EOF, or error.
 */
bool
cw_recv_msg(int sock, cw_msg_t* msg, int timeout_ms);

/** Install the crash signal handlers in the game process. */
void
cw_signal_install(void);

/**
 * Build the module table and frame list for a parked child.
 *
 * Reads `/proc/<pid>/maps` while the child is blocked in the crash
 * protocol and walks frame pointers over the copied stack.
 *
 * @return `true` when at least one frame was produced.
 */
bool
cw_unwind(pid_t pid, const cw_crash_t* crash, cw_crash_info_t* out);

#endif /* CW_LINUX_PLATFORM_H */
