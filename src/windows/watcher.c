/**
 * @file windows/watcher.c
 * Watcher side: adopt the section and events, watch the game, write reports.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windows/platform.h"

/**
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_crash_report(const char* report_dir, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){ .kind = CW_REPORT_CRASH, .main_module = -1 };
	if (!cw_unwind(cw_win.handles.game, &cw_win.region->crash, &info)) {
		cw_log(CW_LOG_WARN, "unwind produced no frames");
	}
	if (!cw_write_envelope(report_dir, &info, &cw_win.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Report what the game handed over with cw_report().
 *
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_error_report(const char* report_dir, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){ .kind = CW_REPORT_ERROR, .main_module = -1 };
	if (!cw_unwind(cw_win.handles.game, &cw_win.region->report, &info)) {
		cw_log(CW_LOG_WARN, "unwind produced no frames");
	}
	snprintf(info.type, sizeof(info.type), "ERROR");
	info.fault_addr = 0;
	cw_cause_apply(&info, &cw_win.region->common.report);
	if (!cw_write_envelope(report_dir, &info, &cw_win.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Report a game that vanished without a crash or a shutdown.
 *
 * The exit status tells a fast-fail such as `0xC0000409` from a launcher
 * kill or a bare `_exit`.
 *
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_killed_report(const char* report_dir, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){
		.kind = CW_REPORT_ABNORMAL_EXIT,
		.type = "KILLED",
		.message_raw = "game ended without cw_shutdown",
		.main_module = -1,
	};
	DWORD code;
	if (GetExitCodeProcess(cw_win.handles.game, &code)) {
		snprintf(info.message_raw, sizeof(info.message_raw), "game ended without cw_shutdown, exit code 0x%lx", code);
	}
	if (!cw_write_envelope(report_dir, &info, &cw_win.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Suspend one thread of the game, walk its stack from its current
 * context, and let it run again.
 *
 * @return `true` when `info` was filled from a usable context.
 */
static bool
snapshot_thread(HANDLE game, DWORD tid, cw_crash_info_t* info) {
	HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
	if (thread == NULL) {
		cw_log(CW_LOG_WARN, "cannot open thread %lu (error %lu)", tid, GetLastError());
		return false;
	}
	bool ok = false;
	if (SuspendThread(thread) != (DWORD)-1) {
		static cw_crash_t snap;
		snap = (cw_crash_t){ .tid = tid, .context = { .ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER } };
		if (GetThreadContext(thread, &snap.context)) {
			ok = true;
			if (!cw_unwind(game, &snap, info)) {
				cw_log(CW_LOG_WARN, "unwind produced no frames");
			}
		}
		ResumeThread(thread);
	}
	CloseHandle(thread);
	return ok;
}

/**
 * Report a game whose heartbeat has been silent for `silent_ms`.
 *
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_hang_report(const char* report_dir, uint64_t silent_ms, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){ .kind = CW_REPORT_HANG, .main_module = -1 };
	DWORD tid = atomic_load_explicit(&cw_win.region->common.heartbeat_tid, memory_order_relaxed);
	snapshot_thread(cw_win.handles.game, tid, &info);
	snprintf(info.type, sizeof(info.type), "HANG");
	snprintf(info.message_raw, sizeof(info.message_raw), "no heartbeat for %" PRIu64 " ms", silent_ms);
	info.fault_addr = 0;
	info.tid = tid;
	if (!cw_write_envelope(report_dir, &info, &cw_win.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Whether a debugger holds the game.
 */
static bool
game_stopped(void) {
	BOOL present = FALSE;
	return CheckRemoteDebuggerPresent(cw_win.handles.game, &present) && present;
}

/**
 * Sample the heartbeat once and act on what the detector says.
 */
static void
check_hang(cw_hang_t* hang, cw_drain_t* drain, const char* report_dir) {
	uint64_t now = cw_platform_now_ms();
	uint64_t count = atomic_load_explicit(&cw_win.region->common.heartbeat, memory_order_relaxed);
	if (count == hang->count && game_stopped()) {
		cw_hang_reset(hang, now);
		return;
	}
	switch (cw_hang_step(hang, count, now, cw_ctx.cfg.hang_timeout_ms)) {
	case CW_HANG_REPORT: {
		uint64_t silent_ms = now - hang->since_ms;
		cw_log(CW_LOG_WARN, "no heartbeat for %" PRIu64 " ms", silent_ms);
		char path[CW_STR_CAP + 64];
		if (cw_drain_accepts(drain) && write_hang_report(report_dir, silent_ms, path, sizeof(path))) {
			cw_drain_report(drain, path);
		}
		break;
	}
	case CW_HANG_RECOVERED:
		cw_log(CW_LOG_INFO, "heartbeat resumed");
		break;
	case CW_HANG_NONE:
		break;
	}
}

/**
 * Wait for events until the game is gone, sampling the heartbeat at a
 * quarter of the hang timeout in between.
 *
 * A wait that finds several objects signaled reports the lowest index.
 * The game sets an event before it ends, so the events come before the
 * process handle; and it decides, authenticates, or reports before it
 * crashes, so those events come before the crash, which keeps the
 * game's order.
 */
static void
cw_watch(const char* report_dir, cw_drain_t* drain) {
	bool crashed = false;
	bool shutdown = false;
	cw_hang_t hang = { 0 };
	DWORD interval_ms = (DWORD)cw_hang_poll_ms(cw_ctx.cfg.hang_timeout_ms);

	for (;;) {
		HANDLE objects[6] = {
			cw_win.handles.ev_consent, cw_win.handles.ev_auth, cw_win.handles.ev_report,
			cw_win.handles.ev_crash, cw_win.handles.ev_shutdown,
			cw_win.handles.game,
		};
		DWORD which = WaitForMultipleObjects(6, objects, FALSE, crashed ? INFINITE : interval_ms);
		if (!crashed) {
			check_hang(&hang, drain, report_dir);
			cw_drain_tick(drain, cw_platform_now_ms());
		}
		if (which == WAIT_TIMEOUT) {
			continue;
		}
		if (which == WAIT_OBJECT_0) {
			cw_drain_consent(drain, (cw_consent_t)atomic_load_explicit(&cw_win.region->consent, memory_order_acquire));
		} else if (which == WAIT_OBJECT_0 + 1) {
			cw_drain_auth(drain, atomic_exchange_explicit(&cw_win.region->auth, 0, memory_order_acq_rel));
		} else if (which == WAIT_OBJECT_0 + 2) {
			/* Idle the slot first: the reporting thread is parked until then. */
			char path[CW_STR_CAP + 64];
			bool written = cw_drain_accepts(drain) && write_error_report(report_dir, path, sizeof(path));
			atomic_store_explicit(&cw_win.region->common.report.state, CW_CRASH_IDLE, memory_order_release);
			if (written) {
				cw_drain_report(drain, path);
			}
		} else if (which == WAIT_OBJECT_0 + 3) {
			/* Reply first: the game is parked until it hears back. */
			char path[CW_STR_CAP + 64];
			bool written = cw_drain_accepts(drain) && write_crash_report(report_dir, path, sizeof(path));
			crashed = true;
			SetEvent(cw_win.handles.ev_done);
			if (written) {
				cw_drain_report(drain, path);
			}
		} else if (which == WAIT_OBJECT_0 + 4) {
			shutdown = true;
		} else {
			break;
		}
	}

	if (shutdown) {
		cw_log(CW_LOG_INFO, "game exited cleanly");
	} else if (!crashed) {
		cw_log(CW_LOG_WARN, "game ended without cw_shutdown");
		char path[CW_STR_CAP + 64];
		if (cw_drain_accepts(drain) && write_killed_report(report_dir, path, sizeof(path))) {
			cw_drain_report(drain, path);
		}
	}
	/* The game is gone, so the backlog may go out. */
	cw_drain_finish(drain);
}

/**
 * Watcher path: adopt the inherited section and events, report ready,
 * watch, exit.
 */
_Noreturn void
cw_platform_run_watcher(const char* spec) {
	_putenv_s(CW_ENV_WATCHER, "");
	unsigned long game;
	unsigned long long v[9];
	int parsed = sscanf(
		spec, "%lu,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx",
		&game, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]
	);
	if (parsed != 10) {
		cw_log(CW_LOG_ERROR, "malformed " CW_ENV_WATCHER);
		exit(1);
	}
	cw_win.handles = (cw_handles_t){
		.section = (HANDLE)(uintptr_t)v[0],
		.game = (HANDLE)(uintptr_t)v[1],
		.ev_crash = (HANDLE)(uintptr_t)v[2],
		.ev_done = (HANDLE)(uintptr_t)v[3],
		.ev_ready = (HANDLE)(uintptr_t)v[4],
		.ev_shutdown = (HANDLE)(uintptr_t)v[5],
		.ev_auth = (HANDLE)(uintptr_t)v[6],
		.ev_consent = (HANDLE)(uintptr_t)v[7],
		.ev_report = (HANDLE)(uintptr_t)v[8],
	};

	cw_region_t* region = MapViewOfFile(cw_win.handles.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cw_region_t));
	CloseHandle(cw_win.handles.section);
	if (region == NULL) {
		cw_log(CW_LOG_ERROR, "cannot map shared region");
		exit(1);
	}
	if (region->common.magic != CW_REGION_MAGIC) {
		cw_log(CW_LOG_ERROR, "shared region header mismatch");
		exit(1);
	}
	cw_win.region = region;
	cw_ctx.shared = &region->common;
	cw_ctx.game_pid = (uint32_t)game;

	const char* report_dir = cw_ctx.report_dir;
	char sub_dir[CW_STR_CAP + 16];
	snprintf(sub_dir, sizeof(sub_dir), "%s\\pending", report_dir);
	if (!cw_platform_mkdir_p(sub_dir)) {
		cw_log(CW_LOG_ERROR, "cannot create %s", sub_dir);
		exit(1);
	}

	/* Read the stored decision before the game can run and change it. */
	cw_drain_t drain;
	cw_drain_init(&drain);

	SetEvent(cw_win.handles.ev_ready);
	cw_log(CW_LOG_INFO, "watching game pid %lu", game);
	cw_watch(report_dir, &drain);
	exit(0);
}
