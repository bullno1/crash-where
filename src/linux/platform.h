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

#define CW_STACK_CAP   (256u * 1024u)
#define CW_MAX_THREADS 256

/**
 * Stack bounds of an attached thread, for the signal handler's copy.
 * `tid` is 0 while the slot is free.
 */
typedef struct {
	_Atomic uint32_t tid;
	uintptr_t lo;
	uintptr_t hi;
} cw_thread_t;

/**
 * Crash-time record written by the signal handler in the game and
 * read by the supervisor.
 *
 * `state` holds a cw_crash_state_t.
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
	cw_thread_t threads[CW_MAX_THREADS];
	cw_crash_t crash;
	cw_crash_t report;         /**< Thread captured by cw_report(); its handshake is cw_shared_t::report. */
} cw_region_t;

/**
 * Message types on the game/watcher socket.
 */
typedef enum {
	CW_MSG_CRASH    = 'C',     /**< Game: crash record is complete. */
	CW_MSG_REPORT   = 'E',     /**< Game: report record is complete; answered by idling cw_shared_t::report. */
	CW_MSG_SHUTDOWN = 'S',     /**< Game: exiting on purpose. */
	CW_MSG_AUTH     = 'A',     /**< Game: authentication files rewritten, `value` holds cw_auth_event_t bits. */
	CW_MSG_CONSENT  = 'N',     /**< Game: the player decided, `value` is a cw_consent_t. */
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
	size_t page_size;          /**< Cached at init; the handler must not call `sysconf`. */
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
cw_install_signal_handler(void);

/** Name of a crash signal such as "SIGSEGV", or "SIGNAL" for any other. */
const char*
cw_signal_name(int signo);

/**
 * Build the module table and frame list for a parked child.
 *
 * Reads `/proc/<pid>/maps` while the child is blocked in the crash
 * protocol and unwinds the copied stack with the `.eh_frame` of each
 * module on disk, falling back to frame pointers where there is none.
 *
 * @return `true` when at least one frame was produced.
 */
bool
cw_unwind(pid_t pid, const cw_crash_t* crash, cw_crash_info_t* out);

/**
 * Write a small ELF core of the game to a temporary file in `pending/`:
 * every thread it can stop, their used stacks, what the stacks point
 * at, the executable's data, and the loader's link map.
 *
 * @param tid    Thread listed first, which a debugger selects.
 * @param crash  Fault-time context and stack copy of `tid`, or `NULL` to use its live registers.
 * @param out    Receives the file's path, or an empty string on failure.
 * @return `true` when the file was written.
 */
bool
cw_write_core(pid_t game, pid_t tid, const cw_crash_t* crash, char* out, size_t cap);

#define CW_REG_COUNT   32
#define CW_EH_MAX_LOAD 16

#if defined(__x86_64__)
#	define CW_REG_SP 7
#	define CW_REG_FP 6
#elif defined(__aarch64__)
#	define CW_REG_SP 31
#	define CW_REG_FP 29
#else
#	error "unsupported architecture"
#endif

/**
 * Register file of one frame, indexed by DWARF register number.
 */
typedef struct {
	uint64_t regs[CW_REG_COUNT];
	uint32_t valid;            /**< Bit `i` set when `regs[i]` is known. */
} cw_regs_t;

/**
 * Stack bytes copied at crash time: `data` holds `[lo, lo + len)`.
 */
typedef struct {
	const uint8_t* data;
	uint64_t lo;
	size_t len;
} cw_stack_t;

typedef struct {
	uint64_t vaddr;
	uint64_t offset;
	uint64_t filesz;
} cw_eh_load_t;

/**
 * Unwind tables of one ELF file, mapped read-only from disk.
 */
typedef struct {
	int state;                 /**< 0 not opened, 1 usable, -1 unusable. */
	const uint8_t* file;
	size_t file_len;
	uint64_t bias;             /**< Load address minus link-time address. */
	cw_eh_load_t load[CW_EH_MAX_LOAD];
	int load_count;
	uint64_t hdr_vaddr;        /**< Link-time address of `.eh_frame_hdr`. */
	const uint8_t* table;      /**< Sorted lookup table in the header, or NULL. */
	uint64_t table_count;
	uint8_t table_enc;
	const uint8_t* eh_frame;   /**< Start of `.eh_frame`, or NULL when unknown. */
	const uint8_t* eh_frame_end;
} cw_eh_module_t;

/**
 * Open the unwind tables of the file at `path`, which the game maps
 * at `map_start` from file offset `map_offset`. `pc` must be a code
 * address inside that mapping: it selects the segment the load bias
 * is taken from, and the first page of a mapping can be padding that
 * belongs to another segment. On failure `m` is left unusable but
 * must still be closed.
 */
bool
cw_eh_open(cw_eh_module_t* m, const char* path, uint64_t map_start, uint64_t map_offset, uint64_t pc);

void
cw_eh_close(cw_eh_module_t* m);

/**
 * Recover the caller's registers from the frame at `pc`.
 *
 * `pc` must already be adjusted into the call instruction for a
 * return address. On success `ret` holds the return address, or 0 in
 * the outermost frame, and `signal_frame` tells whether this frame is
 * a signal trampoline, whose return address needs no adjustment.
 *
 * @return `false` when the module has no entry for `pc` or the rules
 * need memory outside the copied stack; `regs` is then unchanged.
 */
bool
cw_eh_step(
	const cw_eh_module_t* m, const cw_stack_t* stack,
	uint64_t pc, cw_regs_t* regs, uint64_t* ret, bool* signal_frame
);

#endif /* CW_LINUX_PLATFORM_H */
