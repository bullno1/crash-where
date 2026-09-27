/**
 * @file windows/exception.c
 * Crash handlers for the game process.
 *
 * The unhandled exception filter and the abort handler copy what the
 * watcher needs into the shared region, set one event, wait for the
 * watcher to finish, then end the process. They use no heap and no
 * locks; after a stack overflow they run with a single page of stack.
 */
#include <intrin.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "windows/platform.h"

#define CW_REPLY_TIMEOUT 30000

/**
 * Publish the record, hand over to the watcher, end the process.
 */
static void
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
	if (SetEvent(cw_win.h.ev_crash)) {
		WaitForSingleObject(cw_win.h.ev_done, CW_REPLY_TIMEOUT);
	}
	TerminateProcess(GetCurrentProcess(), status);
}

static LONG WINAPI
on_exception(EXCEPTION_POINTERS* ep) {
	report_and_die(ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord, ep->ContextRecord, (uintptr_t)ep);
	return EXCEPTION_CONTINUE_SEARCH;
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
	SetUnhandledExceptionFilter(on_exception);
	signal(SIGABRT, on_abort);
	/* No error dialogs: the filter must run unattended. */
	SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
