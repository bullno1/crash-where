/**
 * @file windows/watcher.c
 * Watcher side: adopt the section and events, watch the game, write reports.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windows/platform.h"

static bool
mkdir_p(const char* path) {
	char buf[CW_STR_CAP];
	size_t len = strnlen(path, sizeof(buf));
	if (len == 0 || len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, path, len + 1);

	/* A drive or a UNC share is not a directory to create. */
	size_t i = 1;
	if (len >= 2 && buf[1] == ':') {
		i = 3;
	} else if (len >= 2 && buf[0] == '\\' && buf[1] == '\\') {
		for (int seps = 0; i < len && seps < 2; ++i) {
			seps += buf[i] == '\\' || buf[i] == '/';
		}
	}
	for (; i <= len; ++i) {
		if (buf[i] == '\\' || buf[i] == '/' || buf[i] == '\0') {
			char saved = buf[i];
			buf[i] = '\0';
			if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
				return false;
			}
			buf[i] = saved;
		}
	}
	return true;
}

/**
 * Resolve the report directory: the configured one, or
 * `%LOCALAPPDATA%\<app>\crash`.
 */
static bool
resolve_report_dir(char* out, size_t cap) {
	int len;
	if (cw_ctx.cfg.report_dir != NULL) {
		len = snprintf(out, cap, "%s", cw_ctx.cfg.report_dir);
	} else {
		const char* local = getenv("LOCALAPPDATA");
		if (local == NULL || local[0] == '\0') {
			return false;
		}
		len = snprintf(out, cap, "%s\\%s\\crash", local, cw_ctx.cfg.app);
	}
	return len > 0 && (size_t)len < cap;
}

/**
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_crash_report(const char* report_dir, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){ .main_module = -1 };
	if (!cw_unwind(cw_win.h.game, &cw_win.region->crash, &info)) {
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
 */
static void
write_killed_report(const char* report_dir) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){
		.type = "KILLED",
		.message_raw = "game ended without cw_shutdown",
		.main_module = -1,
	};
	char path[CW_STR_CAP + 64];
	if (cw_write_envelope(report_dir, &info, &cw_win.region->common, path, sizeof(path))) {
		cw_log(CW_LOG_INFO, "report written to %s", path);
		cw_upload_report(path);
	}
}

/**
 * Wait for events until the game is gone.
 *
 * The game sets an event before it ends, and a wait that finds several
 * objects signaled reports the lowest index, so the events come first.
 */
static void
cw_watch(const char* report_dir) {
	bool crashed = false;
	bool shutdown = false;
	int result = 0;

	for (;;) {
		HANDLE objects[3] = { cw_win.h.ev_crash, cw_win.h.ev_shutdown, cw_win.h.game };
		DWORD which = WaitForMultipleObjects(3, objects, FALSE, INFINITE);
		if (which == WAIT_OBJECT_0) {
			/* Reply first: the game is parked until it hears back. */
			char path[CW_STR_CAP + 64];
			bool written = write_crash_report(report_dir, path, sizeof(path));
			crashed = true;
			SetEvent(cw_win.h.ev_done);
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
	cw_win.h = (cw_handles_t){
		.section = (HANDLE)(uintptr_t)v[0],
		.game = (HANDLE)(uintptr_t)v[1],
		.ev_crash = (HANDLE)(uintptr_t)v[2],
		.ev_done = (HANDLE)(uintptr_t)v[3],
		.ev_ready = (HANDLE)(uintptr_t)v[4],
		.ev_shutdown = (HANDLE)(uintptr_t)v[5],
	};

	cw_region_t* region = MapViewOfFile(cw_win.h.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cw_region_t));
	CloseHandle(cw_win.h.section);
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

	char report_dir[CW_STR_CAP];
	if (!resolve_report_dir(report_dir, sizeof(report_dir))) {
		cw_log(CW_LOG_ERROR, "cannot resolve report directory");
		exit(1);
	}

	char sub_dir[CW_STR_CAP + 16];
	snprintf(sub_dir, sizeof(sub_dir), "%s\\pending", report_dir);
	if (!mkdir_p(sub_dir)) {
		cw_log(CW_LOG_ERROR, "cannot create %s", sub_dir);
		exit(1);
	}

	SetEvent(cw_win.h.ev_ready);
	cw_log(CW_LOG_INFO, "watching game pid %lu", game);
	cw_watch(report_dir);
	exit(0);
}
