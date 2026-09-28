/**
 * @file store.c
 * Files under the report directory: the cached token, the proof awaiting
 * exchange, and the pending store whose envelopes are described by
 * their names alone so none has to be parsed.
 *
 * Every file is written to a `.tmp` name and renamed into place, so a
 * reader in the other process never sees a partial file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"

/** Build `<report_dir>/<name>`. */
static bool
store_path(char* out, size_t cap, const char* name) {
	if (cw_ctx.report_dir[0] == '\0') {
		return false;
	}
	int len = snprintf(out, cap, "%s/%s", cw_ctx.report_dir, name);
	return len > 0 && (size_t)len < cap;
}

bool
cw_store_write(const char* name, const void* data, size_t len) {
	char path[CW_STR_CAP + 64];
	char tmp[sizeof(path) + 4];
	if (!store_path(path, sizeof(path), name) || !cw_platform_mkdir_p(cw_ctx.report_dir)) {
		return false;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (f == NULL) {
		cw_log(CW_LOG_ERROR, "cannot open %s", tmp);
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fclose(f) == 0 && ok;
	if (!ok || !cw_platform_replace(tmp, path)) {
		cw_log(CW_LOG_ERROR, "cannot finish %s", path);
		remove(tmp);
		return false;
	}
	return true;
}

bool
cw_store_read(const char* name, void* buf, size_t cap, size_t* len) {
	char path[CW_STR_CAP + 64];
	if (!store_path(path, sizeof(path), name)) {
		return false;
	}
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	size_t n = fread(buf, 1, cap - 1, f);
	bool whole = fgetc(f) == EOF;
	fclose(f);
	((char*)buf)[n] = '\0';
	*len = n;
	return whole;
}

void
cw_store_remove(const char* name) {
	char path[CW_STR_CAP + 64];
	if (store_path(path, sizeof(path), name)) {
		remove(path);
	}
}

bool
cw_token_load(char* token, size_t cap) {
	char buf[CW_TOKEN_CAP + 64];
	size_t len;
	if (!cw_store_read("token", buf, sizeof(buf), &len)) {
		return false;
	}
	char expires[24];
	if (!cw_reply_get(buf, "token", token, cap) || !cw_reply_get(buf, "expires", expires, sizeof(expires))) {
		return false;
	}
	if (strtoll(expires, NULL, 10) <= (long long)time(NULL)) {
		cw_log(CW_LOG_DEBUG, "cached token has expired");
		return false;
	}
	return true;
}

bool
cw_token_store(const char* token, int64_t expires) {
	char buf[CW_TOKEN_CAP + 64];
	int len = snprintf(buf, sizeof(buf), "token %s\nexpires %lld\n", token, (long long)expires);
	return len > 0 && (size_t)len < sizeof(buf) && cw_store_write("token", buf, (size_t)len);
}

static size_t
base64(const uint8_t* in, size_t len, char* out) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t n = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= in[i + 2];
		}
		out[n++] = alphabet[(v >> 18) & 63];
		out[n++] = alphabet[(v >> 12) & 63];
		out[n++] = i + 1 < len ? alphabet[(v >> 6) & 63] : '=';
		out[n++] = i + 2 < len ? alphabet[v & 63] : '=';
	}
	out[n] = '\0';
	return n;
}

bool
cw_proof_store(const char* store, const void* proof, size_t len) {
	size_t store_len = 0;
	for (; store[store_len] != '\0'; ++store_len) {
		char c = store[store_len];
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		if (!ok || store_len >= 32) {
			return false;
		}
	}
	if (store_len == 0 || len > CW_PROOF_CAP) {
		return false;
	}
	static char body[CW_PROOF_CAP * 4 / 3 + 128];
	int head = snprintf(body, sizeof(body), "{\"store\":\"%s\",\"proof\":\"", store);
	size_t n = (size_t)head + base64(proof, len, body + head);
	memcpy(body + n, "\"}", 2);
	return cw_store_write("proof", body, n + 2);
}

/* Pending store {{{ */

static const char kind_letters[] = { 'c', 'h', 'a' };

char
cw_report_kind_letter(cw_report_kind_t kind) {
	return (unsigned)kind < sizeof(kind_letters) ? kind_letters[kind] : 'c';
}

bool
cw_pending_parse(const char* name, cw_pending_t* out) {
	static const char ext[] = ".json";
	size_t len = strlen(name);
	if (len >= sizeof(out->name) || len <= sizeof(ext) || strcmp(name + len - (sizeof(ext) - 1), ext) != 0) {
		return false;
	}
	char* end;
	long long ts = strtoll(name, &end, 10);
	if (end == name || end[0] != '_' || end[1] == '\0' || end[2] != '_') {
		return false;
	}
	cw_report_kind_t kind = CW_REPORT_CRASH;
	bool known = false;
	for (size_t i = 0; i < sizeof(kind_letters); ++i) {
		if (end[1] == kind_letters[i]) {
			kind = (cw_report_kind_t)i;
			known = true;
		}
	}
	if (!known) {
		return false;
	}
	*out = (cw_pending_t){ .ts = ts, .kind = kind };
	memcpy(out->name, name, len + 1);
	return true;
}

void
cw_pending_remove(const cw_pending_t* p) {
	static const char* const sidecars[] = { ".json", ".dmp", ".log", ".snap" };
	size_t stem = strlen(p->name) - 5;
	for (size_t i = 0; i < sizeof(sidecars) / sizeof(sidecars[0]); ++i) {
		char path[CW_STR_CAP + 128];
		snprintf(path, sizeof(path), "%s/pending/%.*s%s", cw_ctx.report_dir, (int)stem, p->name, sidecars[i]);
		remove(path);
	}
}

/* }}} */
