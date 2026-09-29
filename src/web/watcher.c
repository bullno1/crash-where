/**
 * @file web/watcher.c
 * Watcher side: own the store, write the report when the game traps, drain.
 *
 * Unlike a watcher process this one never polls and never exits. It
 * parks with its runtime alive, and the shim calls the handlers below
 * as the game's messages arrive.
 */
#include <emscripten.h>
#include <stdio.h>

#include "web/platform.h"

static cw_drain_t drain;

static void
on_report(const char* message, const char* stack, cw_shared_t* region) {
	if (region->magic != CW_REGION_MAGIC) {
		cw_log(CW_LOG_ERROR, "region header mismatch, trap not reported");
		return;
	}

	static cw_crash_info_t info;
	info = (cw_crash_info_t){
		.kind = CW_REPORT_CRASH,
		.tid = 1,
		.main_module = 0,
		.module_count = 1,
	};
	cw_web_trap_type(message, info.type, sizeof(info.type));
	snprintf(info.message_raw, sizeof(info.message_raw), "%s", message);

	cw_module_t* module = &info.modules[0];
	cw_web_module_name(module->path, sizeof(module->path));
	uint8_t id[CW_WEB_BUILD_ID_CAP];
	size_t id_len = cw_web_build_id(id, sizeof(id));
	for (size_t i = 0; i < id_len; ++i) {
		snprintf(module->build_id + 2 * i, 3, "%02x", id[i]);
	}

	info.frame_count = cw_web_parse_stack(stack, info.frames, CW_MAX_FRAMES);
	if (info.frame_count == 0) {
		cw_log(CW_LOG_WARN, "stack text holds no frames");
	}

	/* The report collector writes its keys here. */
	cw_ctx.shared = region;
	char path[CW_STR_CAP + 64];
	bool written = cw_drain_accepts(&drain)
		&& cw_write_envelope(cw_ctx.report_dir, &info, region, path, sizeof(path));
	cw_ctx.shared = NULL;
	if (written) {
		cw_log(CW_LOG_INFO, "report written to %s", path);
		cw_drain_report(&drain, path);
	}
	/* A trapped game never runs again, so the backlog may go out. */
	cw_drain_finish(&drain);
}

static void
on_consent(int choice) {
	/* The game's copy of the decision is lost on reload; this one is not. */
	if (choice != CW_CONSENT_ONCE) {
		cw_consent_store((cw_consent_t)choice);
	}
	cw_drain_consent(&drain, (cw_consent_t)choice);
}

static void
on_shutdown(int result) {
	cw_log(CW_LOG_INFO, "game exited cleanly with result %d", result);
	cw_drain_finish(&drain);
}

_Noreturn void
cw_platform_run_watcher(const char* spec) {
	(void)spec;
	char dir[CW_STR_CAP + 16];
	if (!cw_store_path(dir, sizeof(dir), "pending") || !cw_platform_mkdir_p(dir)) {
		cw_log(CW_LOG_ERROR, "cannot create the pending directory under %s", cw_ctx.report_dir);
	}

	cw_drain_init(&drain);

	static cw_pending_t list[CW_PENDING_CAP];
	int count = cw_pending_list(list, CW_PENDING_CAP);
	for (int i = 0; i < count; ++i) {
		cw_web_watcher_pending(list[i].name, list[i].approved);
	}
	cw_web_watcher_ready((int)drain.consent, on_report, on_consent, on_shutdown);
	cw_log(CW_LOG_INFO, "watching the game");
	emscripten_exit_with_live_runtime();
}
