/**
 * @file linux/signal.c
 * Crash signal handlers for the game process.
 *
 * The handler copies what the watcher needs into the shared region,
 * sends one message, waits for the watcher to finish, then lets the
 * signal kill the process. It uses no heap, no locks, and no library
 * calls beyond `memcpy` and raw syscalls.
 */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linux/platform.h"

#define CW_ALT_STACK_SIZE (64u * 1024u)
#define CW_UNKNOWN_STACK  (64u * 1024u)
#define CW_REPLY_TIMEOUT  30000

static uint8_t alt_stack[CW_ALT_STACK_SIZE];

static const int signals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP };

const char*
cw_signal_name(int signo) {
	switch (signo) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGFPE:  return "SIGFPE";
	case SIGILL:  return "SIGILL";
	case SIGABRT: return "SIGABRT";
	case SIGTRAP: return "SIGTRAP";
	default:      return "SIGNAL";
	}
}

static uintptr_t
context_sp(const ucontext_t* uc) {
#if defined(__x86_64__)
	return (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
	return (uintptr_t)uc->uc_mcontext.sp;
#else
#error "unsupported architecture"
#endif
}

/**
 * Copy `[sp, top)` into the record.
 *
 * `process_vm_readv` stops at the first unreadable page instead of
 * faulting, which matters twice: the copy of a small thread stack may
 * run past its top, and a stack overflow leaves `sp` inside the guard
 * page, in which case the copy restarts at the next page boundary where
 * the frames that matter still are. If the call is unavailable, a plain
 * copy from `sp` is the best that can be done.
 */
static void
copy_stack(cw_crash_t* crash, uintptr_t sp, uintptr_t top) {
	uintptr_t page = cw_linux.page_size;
	uintptr_t start = sp;
	size_t len = 0;
	for (int attempt = 0; attempt < 4 && start < top; ++attempt) {
		size_t want = top - start;
		if (want > CW_STACK_CAP) {
			want = CW_STACK_CAP;
		}
		struct iovec local = { .iov_base = crash->stack, .iov_len = want };
		struct iovec remote = { .iov_base = (void*)start, .iov_len = want };
		ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
		if (n > 0) {
			len = (size_t)n;
			break;
		}
		if (n < 0 && errno != EFAULT) {
			memcpy(crash->stack, (const void*)start, want);
			len = want;
			break;
		}
		start = (start + page) & ~(page - 1);
	}
	crash->sp = len > 0 ? start : sp;
	crash->stack_top = top;
	crash->stack_len = len;
}

static void
on_signal(int signo, siginfo_t* si, void* ctx) {
	cw_crash_t* crash = &cw_linux.region->crash;

	/* A second crashing thread parks here; the first one owns the record. */
	uint32_t expected = CW_CRASH_IDLE;
	if (!atomic_compare_exchange_strong(&crash->state, &expected, CW_CRASH_WRITING)) {
		for (;;) {
			pause();
		}
	}

	crash->signo = signo;
	crash->tid = gettid();
	memcpy(&crash->si, si, sizeof(*si));
	memcpy(&crash->uc, ctx, sizeof(ucontext_t));

	/*
	 * The main thread's bounds are known from init. For any other thread
	 * the prototype copies a fixed amount; per-thread bounds are a later
	 * addition.
	 */
	uintptr_t sp = context_sp(&crash->uc);
	bool on_main_stack = sp >= cw_linux.main_stack_lo && sp < cw_linux.main_stack_hi;
	uintptr_t top = on_main_stack ? cw_linux.main_stack_hi : sp + CW_UNKNOWN_STACK;
	copy_stack(crash, sp, top);
	atomic_store(&crash->state, CW_CRASH_DONE);

	/* A failed send means the watcher is gone; do not wait for a reply. */
	if (cw_send_msg(cw_linux.sock, CW_MSG_CRASH, 0)) {
		cw_msg_t reply;
		cw_recv_msg(cw_linux.sock, &reply, CW_REPLY_TIMEOUT);
	}

	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigaction(signo, &dfl, NULL);
	raise(signo);
}

void
cw_install_signal_handler(void) {
	stack_t ss = { .ss_sp = alt_stack, .ss_size = sizeof(alt_stack) };
	struct sigaction sa = {
		.sa_sigaction = on_signal,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

	sigaltstack(&ss, NULL);
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
		sigaction(signals[i], &sa, NULL);
	}
}

/**
 * Detection only. A managed runtime installs its own SIGSEGV handler
 * on top of ours and chains to it for faults outside managed code, so
 * putting ours back would turn its exceptions into crash reports.
 */
void
cw_platform_check_handlers(void) {
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
		struct sigaction cur;
		if (sigaction(signals[i], NULL, &cur) != 0) {
			continue;
		}
		bool ours = (cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_signal;
		if (ours) {
			continue;
		}
		void* handler = (cur.sa_flags & SA_SIGINFO) ? (void*)cur.sa_sigaction : (void*)cur.sa_handler;
		Dl_info info;
		const char* by = handler != NULL && handler != (void*)SIG_IGN && dladdr(handler, &info) && info.dli_fname != NULL
			? info.dli_fname
			: "unknown module";
		cw_log(CW_LOG_WARN, "%s handler was replaced by %s", cw_signal_name(signals[i]), by);
	}
}
