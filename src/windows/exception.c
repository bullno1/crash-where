/**
 * @file windows/exception.c
 * Crash handlers for the game process.
 *
 * The unhandled exception filter and the abort handler copy what the
 * watcher needs into the shared region, set one event, wait for the
 * watcher to finish, then end the process. They use no heap and no
 * locks; after a stack overflow they run on the stack the thread's
 * guarantee set aside.
 *
 * A vectored handler keeps the filter installed: it runs before the
 * frame search on every exception and puts the filter back if another
 * module has taken the slot since init.
 */
#include <intrin.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "windows/platform.h"

#define CW_REPLY_TIMEOUT   30000
/** Stack kept free for the exception dispatch and the filter after a stack overflow. */
#define CW_STACK_GUARANTEE (64 * 1024)

/**
 * Publish the record, hand over to the watcher, end the process.
 */
_Noreturn static void
report_and_die(DWORD status, const EXCEPTION_RECORD* record, const CONTEXT* context, uintptr_t pointers) {
	cw_crash_t* crash = &cw_win.region->crash;

	/* A second crashing thread parks here; the first one owns the record. */
	uint32_t expected = CW_CRASH_IDLE;
	if (!atomic_compare_exchange_strong(&crash->state, &expected, CW_CRASH_WRITING)) {
		for (;;) {
			Sleep(INFINITE);
		}
	}

	crash->tid = GetCurrentThreadId();
	crash->pointers = pointers;
	crash->record = *record;
	crash->context = *context;
	atomic_store(&crash->state, CW_CRASH_DONE);

	/* A failed signal means the watcher is gone; do not wait for a reply. */
	if (SetEvent(cw_win.handles.ev_crash)) {
		WaitForSingleObject(cw_win.handles.ev_done, CW_REPLY_TIMEOUT);
	}
	TerminateProcess(GetCurrentProcess(), status);
}

static LONG WINAPI
on_exception(EXCEPTION_POINTERS* ep) {
	report_and_die(ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord, ep->ContextRecord, (uintptr_t)ep);
}

/**
 * Name the module containing `addr` into `out`, or "unknown module".
 */
static void
module_name(const void* addr, char* out, size_t cap) {
	HMODULE mod;
	DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
	if (addr == NULL || !GetModuleHandleExA(flags, addr, &mod) || GetModuleFileNameA(mod, out, (DWORD)cap) == 0) {
		snprintf(out, cap, "unknown module");
	}
}

/**
 * Runs first on every exception and only puts the filter back, so it is
 * in place if this exception turns out to be unhandled. Whoever took the
 * slot in the meantime is evicted; nothing is decided here.
 *
 * The first eviction is logged, once. The log sink may itself raise an
 * exception, for example through `OutputDebugString`, which would land
 * here again.
 */
static LONG CALLBACK
on_first_chance(EXCEPTION_POINTERS* ep) {
	(void)ep;
	cw_platform_check_handlers();
	return EXCEPTION_CONTINUE_SEARCH;
}

void
cw_platform_check_handlers(void) {
	static _Atomic bool warned;
	LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(on_exception);
	if (prev != on_exception && !atomic_exchange(&warned, true)) {
		char name[MAX_PATH];
		module_name((const void*)prev, name, sizeof(name));
		cw_log(CW_LOG_WARN, "exception filter was replaced by %s, restored", name);
	}
}

/**
 * `abort` ends the process with a fast-fail that no user-mode handler
 * sees, so it is caught one step earlier, in the CRT's SIGABRT handler.
 */
static void __cdecl
on_abort(int sig) {
	(void)sig;
	CONTEXT context;
	RtlCaptureContext(&context);
	EXCEPTION_RECORD record = {
		.ExceptionCode = CW_STATUS_ABORT,
		.ExceptionFlags = EXCEPTION_NONCONTINUABLE,
		.ExceptionAddress = _ReturnAddress(),
	};
	report_and_die(CW_STATUS_ABORT, &record, &context, 0);
}

void
cw_install_exception_handler(void) {
	/*
	 * By default an overflowing thread keeps one page, which the
	 * exception dispatch alone can use up before the filter runs; the
	 * process then dies unreported. The guarantee is per thread, so
	 * only the thread calling cw_init gets it.
	 */
	ULONG guarantee = CW_STACK_GUARANTEE;
	SetThreadStackGuarantee(&guarantee);
	SetUnhandledExceptionFilter(on_exception);
	AddVectoredExceptionHandler(1, on_first_chance);
	signal(SIGABRT, on_abort);
	/* No error dialogs: the filter must run unattended. */
	SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
