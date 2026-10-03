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
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "linux/platform.h"

#define CW_ALT_STACK_SIZE (64u * 1024u)
#define CW_UNKNOWN_STACK  (64u * 1024u)
#define CW_REPLY_TIMEOUT  30000

/**
 * What an attached thread owns: the first page of a private mapping
 * whose remainder is its alternate signal stack. Reached through
 * `attach_key`, whose destructor undoes the attachment at thread exit.
 */
typedef struct {
	cw_thread_t* slot;
	size_t len;                /**< Length of the whole mapping. */
} cw_attach_t;

static pthread_key_t attach_key;
static bool attach_key_ok;

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

/**
 * Upper bound of the copy for the thread `tid` crashing with `sp`: the
 * top of its attached stack when `sp` is on it or just below it, in the
 * guard after an overflow; otherwise a fixed distance above `sp`.
 */
static uintptr_t
stack_top_of(uint32_t tid, uintptr_t sp) {
	const cw_thread_t* threads = cw_linux.region->threads;
	for (int i = 0; i < CW_MAX_THREADS; ++i) {
		if (atomic_load(&threads[i].tid) != tid) {
			continue;
		}
		if (sp < threads[i].hi && sp + CW_UNKNOWN_STACK >= threads[i].lo) {
			return threads[i].hi;
		}
		break;
	}
	return sp + CW_UNKNOWN_STACK;
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

	uintptr_t sp = context_sp(&crash->uc);
	copy_stack(crash, sp, stack_top_of((uint32_t)crash->tid, sp));
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

/**
 * Game side of cw_report(): the calling thread records itself the way
 * the handler does, with `getcontext` for the kernel's context, then
 * waits for the watcher to idle the slot.
 */
void
cw_platform_report(void) {
	cw_crash_t* rec = &cw_linux.region->report;
	cw_cause_t* slot = &cw_linux.region->common.report;
	rec->signo = 0;
	rec->tid = gettid();
	rec->si = (siginfo_t){ 0 };
	getcontext(&rec->uc);
	uintptr_t sp = context_sp(&rec->uc);
	copy_stack(rec, sp, stack_top_of((uint32_t)rec->tid, sp));
	atomic_store_explicit(&slot->state, CW_CRASH_DONE, memory_order_release);

	uint64_t deadline = cw_platform_now_ms() + CW_REPLY_TIMEOUT;
	bool sent = cw_send_msg(cw_linux.sock, CW_MSG_REPORT, 0);
	while (sent && atomic_load_explicit(&slot->state, memory_order_acquire) != CW_CRASH_IDLE) {
		if (cw_platform_now_ms() >= deadline) {
			cw_log(CW_LOG_WARN, "watcher did not write the report in time");
			break;
		}
		nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
	}
	/* Without a watcher, or after giving up, the slot must not stay taken. */
	atomic_store_explicit(&slot->state, CW_CRASH_IDLE, memory_order_release);
}

static void
detach_thread(void* p) {
	cw_attach_t* a = p;
	stack_t ss = { .ss_flags = SS_DISABLE };
	sigaltstack(&ss, NULL);
	atomic_store(&a->slot->tid, 0);
	munmap(a, a->len);
}

void
cw_platform_attach_thread(void) {
	if (!attach_key_ok || pthread_getspecific(attach_key) != NULL) {
		return;
	}
	uint32_t tid = (uint32_t)gettid();

	cw_thread_t* slot = NULL;
	cw_thread_t* threads = cw_linux.region->threads;
	for (int i = 0; i < CW_MAX_THREADS && slot == NULL; ++i) {
		uint32_t expected = 0;
		if (atomic_compare_exchange_strong(&threads[i].tid, &expected, tid)) {
			slot = &threads[i];
		}
	}
	if (slot == NULL) {
		cw_log(CW_LOG_WARN, "thread %u not attached: %d threads are attached already", tid, CW_MAX_THREADS);
		return;
	}
	/* Only this thread's handler reads the slot, so the bounds may follow the claim. */
	slot->lo = 0;
	slot->hi = 0;
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void* addr;
		size_t size;
		if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
			slot->lo = (uintptr_t)addr;
			slot->hi = (uintptr_t)addr + size;
		}
		pthread_attr_destroy(&attr);
	}

	size_t len = cw_linux.page_size + CW_ALT_STACK_SIZE;
	cw_attach_t* a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED) {
		cw_log(CW_LOG_WARN, "thread %u not attached: cannot map its signal stack (%s)", tid, strerror(errno));
		atomic_store(&slot->tid, 0);
		return;
	}
	*a = (cw_attach_t){ .slot = slot, .len = len };
	stack_t ss = { .ss_sp = (uint8_t*)a + cw_linux.page_size, .ss_size = CW_ALT_STACK_SIZE };
	if (sigaltstack(&ss, NULL) != 0 || pthread_setspecific(attach_key, a) != 0) {
		cw_log(CW_LOG_WARN, "thread %u not attached: cannot install its signal stack (%s)", tid, strerror(errno));
		detach_thread(a);
	}
}

void
cw_install_signal_handler(void) {
	struct sigaction sa = {
		.sa_sigaction = on_signal,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

	attach_key_ok = pthread_key_create(&attach_key, detach_thread) == 0;
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
