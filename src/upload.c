/**
 * @file upload.c
 * The client side of the protocol: envelopes and proofs go out through
 * the transport and replies come back as `key value` lines.
 *
 * Everything here runs in the watcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/** Largest reply body the protocol ever needs to read. */
#define CW_REPLY_CAP 4096

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
name_id(const char* name, char out[37]) {
	static const char ext[] = ".json";
	const char* end = strrchr(name, '.');
	if (end == NULL || strcmp(end, ext) != 0 || end - name < 37 || end[-37] != '_') {
		return false;
	}
	memcpy(out, end - 36, 36);
	out[36] = '\0';
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

/** The waiting proof could not be exchanged; not retried until a new one arrives. */
static bool proof_failed;

/**
 * Post the auth request body the game left in the report directory and
 * cache the token it buys. A transient failure keeps the proof for the next
 * `auth refreshed` or the next launch, not for the next upload.
 */
static void
exchange_proof(void) {
	static char body[CW_PROOF_CAP * 4 / 3 + 128];
	size_t body_len;
	if (proof_failed || cw_ctx.cfg.transport == NULL || !cw_store_read("proof", body, sizeof(body), &body_len)) {
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
		}
		break;
	case CW_RETRY:
		cw_log(CW_LOG_WARN, "proof exchange failed, kept for a later try");
		proof_failed = true;
		break;
	case CW_DROP:
		cw_log(CW_LOG_WARN, "server refused the proof (%d), discarded", http);
		cw_store_remove("proof");
		break;
	}
}

void
cw_auth_refreshed(void) {
	cw_log(CW_LOG_DEBUG, "auth refreshed");
	proof_failed = false;
	exchange_proof();
}

/* }}} */

/**
 * A token the server refuses costs one bounce: the report is sent
 * again without it and lands unauthenticated.
 */
void
cw_upload_report(const char* path) {
	if (cw_ctx.cfg.transport == NULL) {
		cw_log(CW_LOG_WARN, "no transport configured, report kept at %s", path);
		return;
	}
	exchange_proof();

	const char* name = strrchr(path, '/');
	cw_pending_t p;
	char id[37];
	if (name == NULL || !cw_pending_parse(name + 1, &p) || !name_id(p.name, id)) {
		cw_log(CW_LOG_ERROR, "unexpected report name %s", path);
		return;
	}
	size_t len = 0;
	char* json = read_file(path, &len);
	if (json == NULL) {
		cw_log(CW_LOG_ERROR, "cannot read %s back for upload", path);
		return;
	}

	char token[CW_TOKEN_CAP];
	bool have_token = cw_token_load(token, sizeof(token));
	char reply[CW_REPLY_CAP];
	int http = 0;
	cw_status_t status = send_request("report", have_token ? token : NULL, json, len, reply, sizeof(reply), &http);
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
		cw_pending_remove(&p);
		break;
	case CW_RETRY:
		cw_log(CW_LOG_WARN, "upload of report %s failed, kept at %s", id, path);
		break;
	case CW_DROP:
		cw_log(CW_LOG_WARN, "report %s rejected, deleted", id);
		cw_pending_remove(&p);
		break;
	}
}
