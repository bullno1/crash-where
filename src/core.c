/**
 * @file core.c
 * Public entry points that do not depend on the platform.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

cw_ctx_t cw_ctx;

/**
 * Accept only the slug alphabet the header promises, since the
 * value becomes a directory name and a server key.
 */
static bool
cw_validate_appname(const char* app) {
	if (app == NULL) {
		return false;
	}
	size_t i = 0;
	for (; app[i] != '\0'; ++i) {
		char c = app[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		if (!ok || i >= 63) {
			return false;
		}
	}
	return i > 0;
}

/**
 * Bounded copy that always terminates `dst`. Never reads past the
 * first NUL of `src` or past `cap - 1` bytes.
 */
static void
copy_str(char* dst, size_t cap, const char* src) {
	size_t i = 0;
	if (src != NULL) {
		for (; i + 1 < cap && src[i] != '\0'; ++i) {
			dst[i] = src[i];
		}
	}
	dst[i] = '\0';
}

void
cw_log(cw_log_level_t level, const char* fmt, ...) {
	if (cw_ctx.cfg.log == NULL) {
		return;
	}
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	cw_ctx.cfg.log(level, buf);
}

void
cw_init(const cw_config_t* cfg) {
	if (cw_ctx.initialized) {
		return;
	}
	cw_ctx.initialized = true;

	if (cfg == NULL) {
		return;
	}

	if (!cw_validate_appname(cfg->app)) {
		if (cfg->log != NULL) {
			cfg->log(CW_LOG_ERROR, "cw_config_t::app missing or not a valid slug, library inactive");
		}
		return;
	}

	cw_ctx.cfg = *cfg;
	copy_str(cw_ctx.app, sizeof(cw_ctx.app), cfg->app);
	copy_str(cw_ctx.version, sizeof(cw_ctx.version), cfg->version);
	copy_str(cw_ctx.channel, sizeof(cw_ctx.channel), cfg->channel);
	copy_str(cw_ctx.endpoint, sizeof(cw_ctx.endpoint), cfg->endpoint);
	copy_str(cw_ctx.report_dir, sizeof(cw_ctx.report_dir), cfg->report_dir);
	cw_ctx.cfg.app = cw_ctx.app;
	cw_ctx.cfg.version = cw_ctx.version;
	cw_ctx.cfg.channel = cw_ctx.channel;
	cw_ctx.cfg.endpoint = cw_ctx.endpoint;
	cw_ctx.cfg.report_dir = cfg->report_dir != NULL ? cw_ctx.report_dir : NULL;
	if (cfg->transport != NULL) {
		cw_ctx.transport = *cfg->transport;
		cw_ctx.cfg.transport = &cw_ctx.transport;
	}
	if (cw_ctx.cfg.hang_timeout_ms == 0) {
		cw_ctx.cfg.hang_timeout_ms = CW_HANG_DEFAULT_MS;
	}

	const char* disable = getenv("CW_DISABLE");
	if (disable != NULL && strcmp(disable, "1") == 0) {
		cw_log(CW_LOG_INFO, "disabled by CW_DISABLE");
		return;
	}

	/* Role detection is cross-platform; only the payload is per platform. */
	const char* spec = getenv(CW_ENV_WATCHER);
	if (spec != NULL) {
		cw_platform_run_watcher(spec);
	}
	cw_ctx.active = cw_platform_run_game();
}

cw_status_t
cw_auth_refresh(void) {
	return CW_DROP;
}

void
cw_heartbeat(void) {
	if (!cw_ctx.active) {
		return;
	}
	/* The first tick is the earliest point where every middleware has had its say. */
	static _Atomic bool handler_checked;
	if (!atomic_exchange_explicit(&handler_checked, true, memory_order_relaxed)) {
		cw_platform_check_handlers();
	}
	atomic_store_explicit(&cw_ctx.shared->heartbeat_tid, cw_platform_tid(), memory_order_relaxed);
	atomic_fetch_add_explicit(&cw_ctx.shared->heartbeat, 1, memory_order_relaxed);
}

void
cw_breadcrumb(const char* category, const char* msg) {
	if (!cw_ctx.active) {
		return;
	}
	cw_shared_t* shared = cw_ctx.shared;
	uint64_t n = atomic_fetch_add_explicit(&shared->crumb_next, 1, memory_order_relaxed);
	cw_crumb_t* crumb = &shared->crumbs[n % CW_CRUMB_COUNT];
	atomic_store_explicit(&crumb->seq, 0, memory_order_release);
	crumb->t_ms = cw_platform_now_ms();
	crumb->tid = cw_platform_tid();
	copy_str(crumb->cat, sizeof(crumb->cat), category);
	copy_str(crumb->msg, sizeof(crumb->msg), msg);
	atomic_store_explicit(&crumb->seq, n + 1, memory_order_release);
}

void
cw_set_state(const char* key, const char* value) {
	if (!cw_ctx.active || key == NULL) {
		return;
	}

	cw_shared_t* shared = cw_ctx.shared;
	cw_state_slot_t* slot = NULL;
	cw_state_slot_t* free_slot = NULL;
	cw_state_slot_t* oldest = NULL;
	uint64_t oldest_seq = UINT64_MAX;
	for (int i = 0; i < CW_STATE_COUNT; ++i) {
		cw_state_slot_t* s = &shared->state[i];
		uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
		if (seq == 0) {
			if (free_slot == NULL) {
				free_slot = s;
			}
			continue;
		}
		if (strncmp(s->key, key, sizeof(s->key)) == 0) {
			slot = s;
			break;
		}
		if (seq < oldest_seq) {
			oldest_seq = seq;
			oldest = s;
		}
	}
	if (slot == NULL) {
		slot = free_slot != NULL ? free_slot : oldest;
	}

	uint64_t stamp = atomic_fetch_add_explicit(&shared->state_seq, 1, memory_order_relaxed) + 1;
	atomic_store_explicit(&slot->seq, 0, memory_order_release);
	copy_str(slot->key, sizeof(slot->key), key);
	copy_str(slot->value, sizeof(slot->value), value);
	atomic_store_explicit(&slot->seq, stamp, memory_order_release);
}

void
cw_set_snapshot(const char* name, const void* data, size_t len) {
	(void)data;
	if (!cw_ctx.active) {
		return;
	}
	cw_log(CW_LOG_WARN, "snapshot '%s' (%zu bytes) ignored, not implemented", name, len);
}

void
cw_append_log(const char* name, const void* rec, size_t len) {
	(void)rec;
	if (!cw_ctx.active) {
		return;
	}
	cw_log(CW_LOG_WARN, "log ring '%s' (%zu bytes) ignored, not implemented", name, len);
}

void
cw_shutdown(int result) {
	if (!cw_ctx.active) {
		return;
	}
	cw_platform_shutdown(result);
}
