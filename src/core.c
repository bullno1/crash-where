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
	cw_ctx.cfg.app = cw_ctx.app;
	cw_ctx.cfg.version = cw_ctx.version;
	cw_ctx.cfg.channel = cw_ctx.channel;
	cw_ctx.cfg.endpoint = cw_ctx.endpoint;
	if (cfg->transport != NULL) {
		cw_ctx.transport = *cfg->transport;
		cw_ctx.cfg.transport = &cw_ctx.transport;
	}
	if (cfg->consent_dialog != NULL) {
		cw_ctx.dialog = *cfg->consent_dialog;
		cw_ctx.cfg.consent_dialog = &cw_ctx.dialog;
	}
	if (cfg->collect_at_init != NULL) {
		cw_ctx.collect_at_init = *cfg->collect_at_init;
		cw_ctx.cfg.collect_at_init = &cw_ctx.collect_at_init;
	}
	if (cfg->collect_at_report != NULL) {
		cw_ctx.collect_at_report = *cfg->collect_at_report;
		cw_ctx.cfg.collect_at_report = &cw_ctx.collect_at_report;
	}
	if (cw_ctx.cfg.hang_timeout_ms == 0) {
		cw_ctx.cfg.hang_timeout_ms = CW_HANG_DEFAULT_MS;
	}

	/* Both processes need the directory: the game for its decision and token, the watcher for reports. */
	bool have_dir = cfg->report_dir != NULL
		? (size_t)snprintf(cw_ctx.report_dir, sizeof(cw_ctx.report_dir), "%s", cfg->report_dir) < sizeof(cw_ctx.report_dir)
		: cw_platform_default_report_dir(cw_ctx.report_dir, sizeof(cw_ctx.report_dir));
	if (!have_dir) {
		cw_ctx.report_dir[0] = '\0';
		cw_log(CW_LOG_ERROR, "cannot resolve the report directory, library inactive");
		return;
	}
	cw_ctx.cfg.report_dir = cw_ctx.report_dir;

	/* Role detection is cross-platform; only the payload is per platform. */
	const char* spec = getenv(CW_ENV_WATCHER);
	if (spec != NULL) {
		cw_platform_run_watcher(spec);
	}

	const char* disable = getenv("CW_DISABLE");
	if (disable != NULL && strcmp(disable, "1") == 0) {
		cw_log(CW_LOG_INFO, "disabled by CW_DISABLE");
		return;
	}

	/* Only the game asks: a debugger that follows the spawn must not send the watcher back into game code. */
	bool forced = disable != NULL && strcmp(disable, "0") == 0;
	if (!forced && cw_platform_debugger_present()) {
		cw_log(CW_LOG_INFO, "disabled by an attached debugger, CW_DISABLE=0 overrides");
		return;
	}

	cw_ctx.active = cw_platform_run_game();
	if (cw_ctx.active && cw_ctx.collect_at_init.collect != NULL) {
		cw_ctx.collect_at_init.collect(cw_ctx.collect_at_init.user);
	}
}

void
cw_attach_thread(void) {
	if (!cw_ctx.active) {
		return;
	}
	cw_platform_attach_thread();
}

bool
cw_auth_proof(const char* store, const void* proof, size_t len) {
	if (!cw_ctx.active || store == NULL || proof == NULL) {
		return false;
	}
	if (cw_ctx.cfg.transport == NULL) {
		cw_log(CW_LOG_WARN, "no transport configured, proof cannot be exchanged");
		return false;
	}
	if (!cw_proof_store(store, proof, len)) {
		cw_log(CW_LOG_ERROR, "proof for store '%s' (%zu bytes) not stored", store, len);
		return false;
	}
	cw_platform_notify_auth(CW_AUTH_PROOF);
	return true;
}

bool
cw_auth_token(const char* token, int64_t expires) {
	if (!cw_ctx.active || token == NULL || !cw_token_store(token, expires)) {
		return false;
	}
	cw_platform_notify_auth(CW_AUTH_TOKEN);
	return true;
}

bool
cw_consent_pending(cw_consent_summary_t* summary) {
	cw_consent_summary_t local;
	if (summary == NULL) {
		summary = &local;
	}
	*summary = (cw_consent_summary_t){ 0 };
	if (cw_ctx.report_dir[0] == '\0' || cw_consent_load() != CW_CONSENT_ASK) {
		return false;
	}
	cw_pending_summary(summary);
	return summary->count > 0;
}

void
cw_consent_set(cw_consent_t choice) {
	if (cw_ctx.report_dir[0] == '\0') {
		return;
	}
	if (choice == CW_CONSENT_NEVER) {
		cw_pending_purge();
	}
	if (choice != CW_CONSENT_ONCE) {
		cw_consent_store(choice);
	}
	if (cw_ctx.active) {
		cw_platform_notify_consent(choice);
	}
}

cw_consent_t
cw_consent_get(void) {
	return cw_ctx.report_dir[0] != '\0' ? cw_consent_load() : CW_CONSENT_ASK;
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
cw_set_env(const char* key, const char* value) {
	/* The watcher writes here too, from the report collector, so the gate is the mapping, not `active`. */
	cw_shared_t* shared = cw_ctx.shared;
	if (shared == NULL || key == NULL) {
		return;
	}

	cw_env_slot_t* slot = NULL;
	cw_env_slot_t* free_slot = NULL;
	for (int i = 0; i < CW_ENV_COUNT; ++i) {
		cw_env_slot_t* s = &shared->env[i];
		if (atomic_load_explicit(&s->seq, memory_order_acquire) == 0) {
			if (free_slot == NULL) {
				free_slot = s;
			}
			continue;
		}
		if (strncmp(s->key, key, sizeof(s->key)) == 0) {
			slot = s;
			break;
		}
	}
	if (slot == NULL) {
		slot = free_slot;
	}
	if (slot == NULL) {
		cw_log(CW_LOG_WARN, "env '%s' dropped, all %d slots in use", key, CW_ENV_COUNT);
		return;
	}

	uint64_t stamp = atomic_fetch_add_explicit(&shared->env_seq, 1, memory_order_relaxed) + 1;
	atomic_store_explicit(&slot->seq, 0, memory_order_release);
	copy_str(slot->key, sizeof(slot->key), key);
	copy_str(slot->value, sizeof(slot->value), value);
	atomic_store_explicit(&slot->seq, stamp, memory_order_release);
}

bool
cw_env_has(const char* key) {
	cw_shared_t* shared = cw_ctx.shared;
	if (shared == NULL || key == NULL) {
		return false;
	}
	for (int i = 0; i < CW_ENV_COUNT; ++i) {
		cw_env_slot_t* s = &shared->env[i];
		if (
			atomic_load_explicit(&s->seq, memory_order_acquire) != 0
			&& strncmp(s->key, key, sizeof(s->key)) == 0
		) {
			return true;
		}
	}
	return false;
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
cw_shutdown(void) {
	if (!cw_ctx.active) {
		return;
	}
	cw_platform_shutdown();
}
