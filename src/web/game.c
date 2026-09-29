/**
 * @file web/game.c
 * Game side: the region, and the messages to the watcher.
 */
#include <stdio.h>
#include <string.h>

#include "web/platform.h"

static cw_shared_t region;

bool
cw_platform_debugger_present(void) {
	/*
	 * A page cannot tell whether developer tools are attached. The shim
	 * only listens for the error, which takes no trap away from them.
	 */
	return false;
}

static void
touch(const char* path) {
	FILE* f = fopen(path, "wb");
	if (f != NULL) {
		fclose(f);
	}
}

/**
 * Give the game the view of the store it had at load: the decision, and
 * the names of the waiting reports with their approvals. Names are all
 * the consent summary reads.
 */
static void
seed_store(void) {
	cw_consent_store((cw_consent_t)cw_web_game_consent());

	int count = cw_web_game_pending_count();
	if (count == 0) {
		return;
	}
	char dir[CW_STR_CAP + 16];
	if (!cw_store_path(dir, sizeof(dir), "pending") || !cw_platform_mkdir_p(dir)) {
		cw_log(CW_LOG_WARN, "cannot create %s, waiting reports not shown", dir);
		return;
	}
	for (int i = 0; i < count; ++i) {
		char name[sizeof(((cw_pending_t*)0)->name)];
		bool approved;
		if (!cw_web_game_pending(i, name, sizeof(name), &approved)) {
			continue;
		}
		char path[CW_STR_CAP + 128];
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		touch(path);
		if (approved) {
			int stem = (int)strlen(name) - (int)(sizeof(".json") - 1);
			snprintf(path, sizeof(path), "%s/%.*s.ok", dir, stem, name);
			touch(path);
		}
	}
}

bool
cw_platform_run_game(void) {
	region.magic = CW_REGION_MAGIC;
	if (!cw_web_game_start(&region, sizeof(region))) {
		cw_log(CW_LOG_WARN, "no watcher answered, library inactive");
		return false;
	}
	cw_ctx.shared = &region;
	seed_store();
	return true;
}

void
cw_platform_notify_consent(cw_consent_t choice) {
	cw_web_game_notify_consent((int)choice);
}

void
cw_platform_notify_auth(unsigned what) {
	(void)what;
	cw_log(CW_LOG_WARN, "token and proof do not reach the watcher on this platform, not implemented");
}

void
cw_platform_shutdown(int result) {
	cw_web_game_notify_shutdown(result);
}

void
cw_platform_attach_thread(void) {
}

void
cw_platform_check_handlers(void) {
}
