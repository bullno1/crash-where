/**
 * @file store.c
 * Files under the report directory: the consent decision, the cached
 * token, the proof awaiting exchange, and the pending store whose
 * envelopes are described by their names alone so nothing is parsed to
 * list them.
 *
 * Every file is written to a `.tmp` name and renamed into place, so a
 * reader in the other process never sees a partial file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

bool
cw_store_path(char* out, size_t cap, const char* name) {
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
	if (!cw_store_path(path, sizeof(path), name) || !cw_platform_mkdir_p(cw_ctx.report_dir)) {
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
	if (!cw_store_path(path, sizeof(path), name)) {
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
	if (cw_store_path(path, sizeof(path), name)) {
		remove(path);
	}
}

/**
 * The first word of a small text file, or an empty string.
 */
static void
read_word(const char* name, char* out, size_t cap) {
	char buf[64];
	size_t len;
	out[0] = '\0';
	if (!cw_store_read(name, buf, sizeof(buf), &len)) {
		return;
	}
	size_t n = 0;
	while (n < len && n + 1 < cap && buf[n] > ' ') {
		out[n] = buf[n];
		++n;
	}
	out[n] = '\0';
}

cw_consent_t
cw_consent_load(void) {
	char word[16];
	read_word("consent", word, sizeof(word));
	if (strcmp(word, "always") == 0) {
		return CW_CONSENT_ALWAYS;
	}
	if (strcmp(word, "never") == 0) {
		return CW_CONSENT_NEVER;
	}
	return CW_CONSENT_ASK;
}

bool
cw_consent_store(cw_consent_t choice) {
	switch (choice) {
	case CW_CONSENT_ALWAYS:
		return cw_store_write("consent", "always\n", 7);
	case CW_CONSENT_NEVER:
		return cw_store_write("consent", "never\n", 6);
	default:
		cw_store_remove("consent");
		return true;
	}
}

bool
cw_token_load(char* token, size_t cap, int64_t* expires) {
	char buf[CW_TOKEN_CAP + 64];
	size_t len;
	if (!cw_store_read("token", buf, sizeof(buf), &len)) {
		return false;
	}
	char value[24];
	if (!cw_reply_get(buf, "token", token, cap) || !cw_reply_get(buf, "expires", value, sizeof(value))) {
		return false;
	}
	*expires = strtoll(value, NULL, 10);
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

	char ok[CW_STR_CAP + 128];
	int n = snprintf(ok, sizeof(ok), "%s/pending/%.*s.ok", cw_ctx.report_dir, (int)(len - (sizeof(ext) - 1)), name);
	if (n > 0 && (size_t)n < sizeof(ok)) {
		FILE* f = fopen(ok, "rb");
		if (f != NULL) {
			fclose(f);
			out->approved = true;
		}
	}
	return true;
}

typedef struct {
	cw_pending_t* out;
	int cap;
	int count;
} list_ctx_t;

static void
collect(void* user, const char* name) {
	list_ctx_t* ctx = user;
	cw_pending_t p;
	if (ctx->count < ctx->cap && cw_pending_parse(name, &p)) {
		ctx->out[ctx->count++] = p;
	}
}

static int
pending_cmp(const void* a, const void* b) {
	return strcmp(((const cw_pending_t*)a)->name, ((const cw_pending_t*)b)->name);
}

int
cw_pending_list(cw_pending_t* out, int cap) {
	char dir[CW_STR_CAP + 16];
	if (!cw_store_path(dir, sizeof(dir), "pending")) {
		return 0;
	}
	list_ctx_t ctx = { .out = out, .cap = cap };
	cw_platform_list_dir(dir, collect, &ctx);
	qsort(out, (size_t)ctx.count, sizeof(*out), pending_cmp);
	return ctx.count;
}

void
cw_pending_path(const cw_pending_t* p, char* out, size_t cap) {
	snprintf(out, cap, "%s/pending/%s", cw_ctx.report_dir, p->name);
}

void
cw_pending_remove(const cw_pending_t* p) {
	static const char* const sidecars[] = { ".json", ".ok", ".dmp", ".log", ".snap" };
	size_t stem = strlen(p->name) - 5;
	for (size_t i = 0; i < sizeof(sidecars) / sizeof(sidecars[0]); ++i) {
		char path[CW_STR_CAP + 128];
		snprintf(path, sizeof(path), "%s/pending/%.*s%s", cw_ctx.report_dir, (int)stem, p->name, sidecars[i]);
		remove(path);
	}
}

void
cw_pending_approve_all(void) {
	cw_pending_t list[CW_PENDING_CAP];
	int n = cw_pending_list(list, CW_PENDING_CAP);
	for (int i = 0; i < n; ++i) {
		if (list[i].approved) {
			continue;
		}
		char path[CW_STR_CAP + 128];
		snprintf(path, sizeof(path), "%s/pending/%.*s.ok", cw_ctx.report_dir, (int)(strlen(list[i].name) - 5), list[i].name);
		FILE* f = fopen(path, "wb");
		if (f == NULL) {
			cw_log(CW_LOG_ERROR, "cannot create %s", path);
			continue;
		}
		fclose(f);
	}
}

static void
remove_entry(void* user, const char* name) {
	const char* dir = user;
	char path[CW_STR_CAP + 128];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	remove(path);
}

void
cw_pending_purge(void) {
	char dir[CW_STR_CAP + 16];
	if (cw_store_path(dir, sizeof(dir), "pending")) {
		cw_platform_list_dir(dir, remove_entry, dir);
	}
}

void
cw_pending_summary(cw_consent_summary_t* out) {
	cw_pending_t list[CW_PENDING_CAP];
	int n = cw_pending_list(list, CW_PENDING_CAP);
	*out = (cw_consent_summary_t){ 0 };
	for (int i = 0; i < n; ++i) {
		if (list[i].approved) {
			continue;
		}
		/* Oldest first, so the last one seen is the newest. */
		out->count++;
		out->newest_kind = list[i].kind;
		out->newest_time = list[i].ts;
	}
}

/* }}} */
