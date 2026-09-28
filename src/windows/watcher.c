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
	info = (cw_crash_info_t){ .main_module = -1 };
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
 * Report a game that vanished without a crash or a shutdown.
 *
 * The exit status tells a fast-fail such as `0xC0000409` from a launcher
 * kill or a bare `_exit`.
 */
static void
write_killed_report(const char* report_dir) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){
		.type = "KILLED",
		.message_raw = "game ended without cw_shutdown",
		.main_module = -1,
	};
	DWORD code;
	if (GetExitCodeProcess(cw_win.handles.game, &code)) {
		snprintf(info.message_raw, sizeof(info.message_raw), "game ended without cw_shutdown, exit code 0x%lx", code);
	}
	char path[CW_STR_CAP + 64];
	if (cw_write_envelope(report_dir, &info, &cw_win.region->common, path, sizeof(path))) {
		cw_log(CW_LOG_INFO, "report written to %s", path);
		cw_upload_report(path);
	}
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
	info = (cw_crash_info_t){ .main_module = -1 };
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
check_hang(cw_hang_t* hang, const char* report_dir) {
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
		if (write_hang_report(report_dir, silent_ms, path, sizeof(path))) {
			cw_upload_report(path);
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
 * The game sets an event before it ends, and a wait that finds several
 * objects signaled reports the lowest index, so the events come first.
 */
static void
cw_watch(const char* report_dir) {
	bool crashed = false;
	bool shutdown = false;
	int result = 0;
	cw_hang_t hang = { 0 };
	DWORD interval_ms = (DWORD)cw_hang_poll_ms(cw_ctx.cfg.hang_timeout_ms);

	for (;;) {
		HANDLE objects[3] = { cw_win.handles.ev_crash, cw_win.handles.ev_shutdown, cw_win.handles.game };
		DWORD which = WaitForMultipleObjects(3, objects, FALSE, crashed ? INFINITE : interval_ms);
		if (!crashed) {
			check_hang(&hang, report_dir);
		}
		if (which == WAIT_TIMEOUT) {
			continue;
		}
		if (which == WAIT_OBJECT_0) {
			/* Reply first: the game is parked until it hears back. */
			char path[CW_STR_CAP + 64];
			bool written = write_crash_report(report_dir, path, sizeof(path));
			crashed = true;
			SetEvent(cw_win.handles.ev_done);
			if (written) {
				cw_upload_report(path);
			}
		} else if (which == WAIT_OBJECT_0 + 1) {
			shutdown = true;
			result = cw_win.region->shutdown_result;
		} else {
			break;
		}
	}

	if (crashed) {
		return;
	}
	if (shutdown) {
		cw_log(CW_LOG_INFO, "game exited cleanly with result %d", result);
		return;
	}
	cw_log(CW_LOG_WARN, "game ended without cw_shutdown");
	write_killed_report(report_dir);
}

/**
 * Watcher path: adopt the inherited section and events, report ready,
 * watch, exit.
 */
_Noreturn void
cw_platform_run_watcher(const char* spec) {
	_putenv_s(CW_ENV_WATCHER, "");
	unsigned long game;
	unsigned long long v[6];
	if (sscanf(spec, "%lu,%llx,%llx,%llx,%llx,%llx,%llx", &game, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7) {
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

	const char* report_dir = cw_ctx.report_dir;
	char sub_dir[CW_STR_CAP + 16];
	snprintf(sub_dir, sizeof(sub_dir), "%s\\pending", report_dir);
	if (!cw_platform_mkdir_p(sub_dir)) {
		cw_log(CW_LOG_ERROR, "cannot create %s", sub_dir);
		exit(1);
	}

	SetEvent(cw_win.handles.ev_ready);
	cw_log(CW_LOG_INFO, "watching game pid %lu", game);
	cw_watch(report_dir);
	exit(0);
}
