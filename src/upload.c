/**
 * @file upload.c
 * The client side of the protocol: envelopes and proofs go out through
 * the transport, replies come back as `key value` lines, and the
 * consent decision gates all of it.
 *
 * Everything here runs in the watcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"

/** Largest reply body the protocol ever needs to read. */
#define CW_REPLY_CAP 4096
/** How long the catch-up drain waits for the game to authenticate. */
#define CW_CATCHUP_TIMEOUT_MS 60000u
/** Age after which a pending report is given up on. */
#define CW_REPORT_MAX_AGE_S (14 * 24 * 3600)

/**
 * Read a whole file into a NUL-terminated heap buffer.
 *
 * @return The buffer, or `NULL` when the file cannot be read.
 */
static char*
read_file(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char* buf = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long size = ftell(f);
		if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
			buf = malloc((size_t)size + 1);
			if (buf != NULL) {
				*len = fread(buf, 1, (size_t)size, f);
				buf[*len] = '\0';
			}
		}
	}
	fclose(f);
	return buf;
}

/**
 * Take the report id from an envelope name, `<ts>_<kind>_<fp>_<id>.json`.
 */
static bool
name_id(const char* name, char out[CW_UUID_CAP]) {
	static const char ext[] = ".json";
	const char* end = strrchr(name, '.');
	if (end == NULL || strcmp(end, ext) != 0 || end - name < CW_UUID_CAP || end[-CW_UUID_CAP] != '_') {
		return false;
	}
	memcpy(out, end - (CW_UUID_CAP - 1), CW_UUID_CAP - 1);
	out[CW_UUID_CAP - 1] = '\0';
	return true;
}

/**
 * Map an HTTP status onto what happens to the report.
 *
 * Redirects count as rejections: they are never followed, and one means
 * the compiled-in endpoint is wrong, which retrying cannot fix.
 */
static cw_status_t
map_status(int status) {
	if (status >= 200 && status < 300) {
		return CW_OK;
	}
	if (status == 0 || status == 429 || status >= 500) {
		return CW_RETRY;
	}
	return CW_DROP;
}

bool
cw_reply_get(const char* text, const char* key, char* out, size_t cap) {
	size_t key_len = strlen(key);
	for (const char* line = text; *line != '\0';) {
		const char* end = strchr(line, '\n');
		size_t line_len = end != NULL ? (size_t)(end - line) : strlen(line);
		if (line_len > key_len && line[key_len] == ' ' && memcmp(line, key, key_len) == 0) {
			const char* value = line + key_len + 1;
			size_t value_len = line_len - key_len - 1;
			if (value_len > 0 && value[value_len - 1] == '\r') {
				--value_len;
			}
			if (value_len >= cap) {
				return false;
			}
			memcpy(out, value, value_len);
			out[value_len] = '\0';
			return true;
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
	return false;
}

/**
 * One request through the transport.
 *
 * @param reply   Receives the NUL-terminated reply body on ::CW_OK.
 * @param status  Receives the HTTP status on ::CW_OK.
 * @return What the transport returned.
 */
static cw_status_t
send_request(
	const char* route, const char* token,
	const void* body, size_t body_len,
	char* reply, size_t reply_cap, int* status
) {
	const cw_transport_t* tr = cw_ctx.cfg.transport;
	char url[CW_STR_CAP + 128];
	snprintf(url, sizeof(url), "%s/v1/%s/%s", cw_ctx.cfg.endpoint, cw_ctx.cfg.app, route);
	cw_request_t req = {
		.method = "POST",
		.url = url,
		.content_type = "application/json",
		.token = token,
		.body = body,
		.body_len = body_len,
		.reply = reply,
		.reply_cap = reply_cap - 1,
	};
	cw_response_t resp = { 0 };
	cw_status_t st = tr->send(tr->user, &req, &resp);
	if (st == CW_OK) {
		if (resp.reply_len > req.reply_cap) {
			resp.reply_len = req.reply_cap;
		}
		reply[resp.reply_len] = '\0';
		*status = resp.status;
	}
	return st;
}

/* Proof exchange {{{ */

static void
load_token(cw_drain_t* d) {
	if (!cw_token_load(d->token, sizeof(d->token), &d->token_expires)) {
		d->token[0] = '\0';
		d->token_expires = 0;
	}
}

/**
 * Post the auth request body the game left in the report directory and
 * cache the token it buys. A transient failure keeps the proof for the
 * next `auth refreshed` or the next launch, not for the next drain.
 */
static void
exchange_proof(cw_drain_t* d) {
	static char body[CW_PROOF_CAP * 4 / 3 + 128];
	size_t body_len;
	if (d->proof_failed || cw_ctx.cfg.transport == NULL || !cw_store_read("proof", body, sizeof(body), &body_len)) {
		return;
	}

	char reply[CW_REPLY_CAP];
	int http = 0;
	cw_status_t status = send_request("auth", NULL, body, body_len, reply, sizeof(reply), &http);
	char token[CW_TOKEN_CAP];
	char expires[24];
	if (status == CW_OK) {
		status = map_status(http);
		if (status == CW_OK && !cw_reply_get(reply, "token", token, sizeof(token))) {
			cw_log(CW_LOG_WARN, "auth reply carries no token");
			status = CW_RETRY;
		}
		if (status == CW_OK && !cw_reply_get(reply, "expires", expires, sizeof(expires))) {
			cw_log(CW_LOG_WARN, "auth reply carries no expiry");
			status = CW_RETRY;
		}
	}
	switch (status) {
	case CW_OK:
		if (cw_token_store(token, strtoll(expires, NULL, 10))) {
			cw_log(CW_LOG_INFO, "token refreshed, valid until %s", expires);
			cw_store_remove("proof");
			load_token(d);
		}
		break;
	case CW_RETRY:
		cw_log(CW_LOG_WARN, "proof exchange failed, kept for a later try");
		d->proof_failed = true;
		break;
	case CW_DROP:
		cw_log(CW_LOG_WARN, "server refused the proof (%d), discarded", http);
		cw_store_remove("proof");
		break;
	}
}

/* }}} */

/**
 * The one rule: a report may go out under `always`, or when a one-shot
 * approval released it.
 */
static bool
upload_allowed(const cw_drain_t* d, const cw_pending_t* p) {
	return d->consent == CW_CONSENT_ALWAYS || p->approved;
}

static bool
failed_this_run(const cw_drain_t* d, const char* id) {
	for (int i = 0; i < d->failed_count; ++i) {
		if (strcmp(d->failed[i], id) == 0) {
			return true;
		}
	}
	return false;
}

/**
 * Send one envelope with the cached token, if any.
 *
 * This is the only path to the report route, and it goes nowhere
 * without the player's decision and a transport. A token the server
 * refuses costs one bounce: the report is sent again without it and
 * lands unauthenticated. Delivered and rejected envelopes are deleted;
 * failed ones stay in `pending/` and are not tried again before the
 * next launch.
 */
static cw_status_t
upload_report(cw_drain_t* d, const cw_pending_t* p) {
	if (!upload_allowed(d, p)) {
		cw_log(CW_LOG_INFO, "report %s waits for consent", p->name);
		return CW_RETRY;
	}

	if (cw_ctx.cfg.transport == NULL) {
		return CW_RETRY;
	}

	char id[CW_UUID_CAP];
	if (!name_id(p->name, id)) {
		cw_log(CW_LOG_ERROR, "unexpected report name %s", p->name);
		return CW_DROP;
	}

	if (failed_this_run(d, id)) {
		return CW_RETRY;
	}

	char path[CW_STR_CAP + 128];
	cw_pending_path(p, path, sizeof(path));
	size_t len = 0;
	char* json = read_file(path, &len);
	if (json == NULL) {
		cw_log(CW_LOG_ERROR, "cannot read %s back for upload", path);
		return CW_RETRY;
	}

	bool have_token = d->token[0] != '\0' && d->token_expires > (int64_t)time(NULL);
	char reply[CW_REPLY_CAP];
	int http = 0;
	cw_status_t status = send_request("report", have_token ? d->token : NULL, json, len, reply, sizeof(reply), &http);
	if (status == CW_OK && http == 401 && have_token) {
		cw_log(CW_LOG_WARN, "server refused the token, sending report %s unauthenticated", id);
		status = send_request("report", NULL, json, len, reply, sizeof(reply), &http);
	}
	free(json);

	bool want_attachments = false;
	if (status == CW_OK) {
		status = map_status(http);
		if (status != CW_OK) {
			cw_log(CW_LOG_DEBUG, "server answered %d for report %s", http, id);
		}
		char value[8];
		want_attachments = status == CW_OK
			&& cw_reply_get(reply, "want_attachments", value, sizeof(value))
			&& strcmp(value, "1") == 0;
	}
	/* The prototype writes no attachments yet. */
	if (want_attachments) {
		cw_log(CW_LOG_DEBUG, "server wants attachments for report %s, none written", id);
	}

	switch (status) {
	case CW_OK:
		cw_log(CW_LOG_INFO, "report %s uploaded", id);
		cw_pending_remove(p);
		break;
	case CW_RETRY:
		cw_log(CW_LOG_WARN, "upload of report %s failed, kept at %s", id, path);
		if (d->failed_count < CW_PENDING_CAP) {
			memcpy(d->failed[d->failed_count++], id, sizeof(id));
		}
		break;
	case CW_DROP:
		cw_log(CW_LOG_WARN, "report %s rejected, deleted", id);
		cw_pending_remove(p);
		break;
	}
	return status;
}

/* Drain {{{ */

void
cw_drain_init(cw_drain_t* d) {
	*d = (cw_drain_t){
		.consent = cw_consent_load(),
		.start_ms = cw_platform_now_ms(),
	};
	load_token(d);
	static const char* const names[] = { "ask", "always", "never" };
	cw_log(CW_LOG_DEBUG, "consent decision on disk: %s", names[d->consent]);
	if (cw_ctx.cfg.transport == NULL) {
		cw_log(CW_LOG_WARN, "no transport configured, reports stay in %s", cw_ctx.report_dir);
	}
}

bool
cw_drain_accepts(const cw_drain_t* d) {
	return d->consent != CW_CONSENT_NEVER;
}

/**
 * Send every pending report the decision allows, oldest first, after
 * exchanging a waiting proof so they carry the token. Whoever holds
 * `lock` drains; another watcher skips.
 *
 * This is the only place the proof is exchanged.
 */
static void
drain_pending(cw_drain_t* d) {
	if (d->consent == CW_CONSENT_NEVER) {
		return;
	}

	if (!d->locked) {
		char lock[CW_STR_CAP + 16];
		d->locked = cw_store_path(lock, sizeof(lock), "lock") && cw_platform_lock(lock);
		if (!d->locked) {
			cw_log(CW_LOG_INFO, "another watcher is draining, skipping");
			return;
		}
	}

	cw_pending_t list[CW_PENDING_CAP];
	int n = cw_pending_list(list, CW_PENDING_CAP);

	// Under `always` the token must be ready before the next report
	bool any_allowed = d->consent == CW_CONSENT_ALWAYS;
	for (int i = 0; i < n; ++i) {
		any_allowed = any_allowed || upload_allowed(d, &list[i]);
	}
	if (any_allowed) {
		exchange_proof(d);
	}

	long long now = (long long)time(NULL);
	for (int i = 0; i < n; ++i) {
		if (now - list[i].ts > CW_REPORT_MAX_AGE_S) {
			cw_log(CW_LOG_INFO, "report %s is too old, deleted", list[i].name);
			cw_pending_remove(&list[i]);
		} else {
			upload_report(d, &list[i]);
		}
	}
}

static void
catch_up(cw_drain_t* d) {
	d->caught_up = true;
	drain_pending(d);
}

void
cw_drain_report(cw_drain_t* d, const char* path) {
	const char* name = strrchr(path, '/');
	cw_pending_t p;
	if (name == NULL || !cw_pending_parse(name + 1, &p)) {
		cw_log(CW_LOG_ERROR, "unexpected report name %s", path);
		return;
	}
	upload_report(d, &p);
}

void
cw_drain_auth(cw_drain_t* d) {
	cw_log(CW_LOG_DEBUG, "auth refreshed");
	d->auth_seen = true;
	d->proof_failed = false;
	load_token(d);
	catch_up(d);
}

void
cw_drain_consent(cw_drain_t* d, cw_consent_t choice) {
	static const char* const names[] = { "ask", "always", "never", "once" };
	cw_log(CW_LOG_INFO, "consent: %s", names[choice]);
	switch (choice) {
	case CW_CONSENT_ALWAYS:
		d->consent = CW_CONSENT_ALWAYS;
		catch_up(d);
		break;
	case CW_CONSENT_ONCE:
		cw_pending_approve_all();
		catch_up(d);
		break;
	case CW_CONSENT_NEVER:
		d->consent = CW_CONSENT_NEVER;
		cw_pending_purge();
		break;
	case CW_CONSENT_ASK:
		d->consent = CW_CONSENT_ASK;
		break;
	}
}

void
cw_drain_tick(cw_drain_t* d, uint64_t now_ms) {
	if (!d->caught_up && now_ms - d->start_ms >= CW_CATCHUP_TIMEOUT_MS) {
		cw_log(CW_LOG_DEBUG, "no authentication after %u ms, draining without it", CW_CATCHUP_TIMEOUT_MS);
		catch_up(d);
	}
}

void
cw_drain_finish(cw_drain_t* d) {
	if (!d->caught_up) {
		catch_up(d);
	}
}

/* }}} */
