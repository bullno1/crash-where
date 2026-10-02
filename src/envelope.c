/**
 * @file envelope.c
 * Writes the JSON envelope for one crash into the pending store.
 *
 * The envelope has a fixed shape and is never parsed by the client, so it
 * is emitted directly with `fprintf`. Every string read from the shared
 * region is bounded by its slot size; the region was written by a process
 * that was crashing and cannot be trusted to have terminated anything.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "vendor/chibihash64-stream.h"

/**
 * Emit a JSON string literal from at most `cap` bytes of `s`.
 *
 * Escapes quotes, backslashes, and control bytes. Well-formed UTF-8
 * passes through. An incomplete sequence at the end of the buffer is
 * dropped, since slots are truncated at a byte boundary; any other
 * malformed byte becomes `?`.
 */
static void
put_str(FILE* f, const char* s, size_t cap) {
	size_t len = 0;
	while (len < cap && s[len] != '\0') {
		++len;
	}
	fputc('"', f);
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"') {
			fputs("\\\"", f);
		} else if (c == '\\') {
			fputs("\\\\", f);
		} else if (c < 0x20) {
			fprintf(f, "\\u%04x", c);
		} else if (c < 0x80) {
			fputc(c, f);
		} else {
			size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
			if (n != 0 && i + n > len) {
				break;
			}
			bool ok = n != 0;
			for (size_t k = 1; ok && k < n; ++k) {
				ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
			}
			if (ok) {
				fwrite(s + i, 1, n, f);
				i += n;
				continue;
			}
			fputc('?', f);
		}
		++i;
	}
	fputc('"', f);
}

/**
 * Emit `message_raw` with hex addresses replaced by `<ADDR>` and
 * other integers by `<N>`, the first two normalization rules.
 */
static void
put_norm(FILE* f, const char* s, size_t cap) {
	size_t len = 0;
	while (len < cap && s[len] != '\0') {
		++len;
	}
	fputc('"', f);
	size_t i = 0;
	while (i < len) {
		if (i + 2 < len && s[i] == '0' && s[i + 1] == 'x' && isxdigit((unsigned char)s[i + 2])) {
			i += 2;
			while (i < len && isxdigit((unsigned char)s[i])) {
				++i;
			}
			fputs("<ADDR>", f);
		} else if (isdigit((unsigned char)s[i])) {
			while (i < len && isdigit((unsigned char)s[i])) {
				++i;
			}
			fputs("<N>", f);
		} else {
			char c = s[i++];
			if (c == '"' || c == '\\') {
				fputc('\\', f);
			}
			if ((unsigned char)c < 0x20) {
				c = ' ';
			}
			fputc(c, f);
		}
	}
	fputc('"', f);
}

/**
 * Placeholder fingerprint over the exception type and the
 * (build id, offset) list.
 *
 * The design's SHA-256 `fp` is recomputed by the server and is
 * authoritative; this value only names the file and lets a later local
 * dedup step compare reports from the same build.
 */
static uint64_t
fingerprint(const cw_crash_info_t* info) {
	ChibiHash64Ctx ctx = chibihash64_init(0);
	chibihash64_append(&ctx, (void*)info->type, (ptrdiff_t)strnlen(info->type, sizeof(info->type)));
	for (int i = 0; i < info->frame_count; ++i) {
		const cw_frame_t* fr = &info->frames[i];
		if (fr->module >= 0) {
			const cw_module_t* m = &info->modules[fr->module];
			chibihash64_append(&ctx, (void*)m->build_id, (ptrdiff_t)strnlen(m->build_id, sizeof(m->build_id)));
		}
		chibihash64_append(&ctx, (void*)&fr->offset, (ptrdiff_t)sizeof(fr->offset));
	}
	return chibihash64_finish(&ctx);
}

static void
make_uuid(char out[CW_UUID_CAP]) {
	unsigned char b[16] = { 0 };
	if (!cw_platform_random(b, sizeof(b))) {
		uint64_t t = (uint64_t)time(NULL);
		memcpy(b, &t, sizeof(t));
	}
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	snprintf(
		out, CW_UUID_CAP,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
		b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]
	);
}

static int
crumb_cmp(const void* a, const void* b) {
	const cw_crumb_t* x = a;
	const cw_crumb_t* y = b;
	uint64_t sx = atomic_load_explicit(&x->seq, memory_order_relaxed);
	uint64_t sy = atomic_load_explicit(&y->seq, memory_order_relaxed);
	return sx < sy ? -1 : sx > sy ? 1 : 0;
}

static void
put_crumbs(FILE* f, const cw_shared_t* shared) {
	static cw_crumb_t sorted[CW_CRUMB_COUNT];
	int n = 0;
	for (int i = 0; i < CW_CRUMB_COUNT; ++i) {
		if (atomic_load_explicit(&shared->crumbs[i].seq, memory_order_acquire) != 0) {
			memcpy(&sorted[n++], &shared->crumbs[i], sizeof(cw_crumb_t));
		}
	}
	qsort(sorted, (size_t)n, sizeof(cw_crumb_t), crumb_cmp);
	fputs("[", f);
	for (int i = 0; i < n; ++i) {
		const cw_crumb_t* c = &sorted[i];
		fprintf(f, "%s{\"t\":%" PRIu64 ",\"th\":%" PRIu32 ",\"c\":", i ? "," : "", c->t_ms, c->tid);
		put_str(f, c->cat, sizeof(c->cat));
		fputs(",\"m\":", f);
		put_str(f, c->msg, sizeof(c->msg));
		fputs("}", f);
	}
	fputs("]", f);
}

static void
put_state(FILE* f, const cw_shared_t* shared) {
	fputs("{", f);
	bool first = true;
	for (int i = 0; i < CW_STATE_COUNT; ++i) {
		const cw_state_slot_t* s = &shared->state[i];
		if (atomic_load_explicit(&s->seq, memory_order_acquire) == 0) {
			continue;
		}
		if (!first) {
			fputs(",", f);
		}
		first = false;
		put_str(f, s->key, sizeof(s->key));
		fputs(":", f);
		put_str(f, s->value, sizeof(s->value));
	}
	fputs("}", f);
}

static void
put_env(FILE* f, const cw_shared_t* shared) {
	fputs("{", f);
	bool first = true;
	for (int i = 0; i < CW_ENV_COUNT; ++i) {
		const cw_env_slot_t* s = &shared->env[i];
		if (atomic_load_explicit(&s->seq, memory_order_acquire) == 0) {
			continue;
		}
		if (!first) {
			fputs(",", f);
		}
		first = false;
		put_str(f, s->key, sizeof(s->key));
		fputs(":", f);
		put_str(f, s->value, sizeof(s->value));
	}
	fputs("}", f);
}

/**
 * Emit the file name of a module without its directory; the full
 * path can contain the user name.
 */
static void
put_module_name(FILE* f, const cw_module_t* m) {
	size_t len = strnlen(m->path, sizeof(m->path));
	size_t start = len;
	while (start > 0 && m->path[start - 1] != '/' && m->path[start - 1] != '\\') {
		--start;
	}
	put_str(f, m->path + start, len - start);
}

static void
put_modules(FILE* f, const cw_crash_info_t* info) {
	fputs("[", f);
	for (int i = 0; i < info->module_count; ++i) {
		const cw_module_t* m = &info->modules[i];
		fprintf(f, "%s{\"name\":", i ? "," : "");
		put_module_name(f, m);
		fputs(",\"build_id\":", f);
		put_str(f, m->build_id, sizeof(m->build_id));
		fprintf(f, ",\"base\":\"0x%" PRIx64 "\",\"size\":%" PRIu64 "}", m->base, m->size);
	}
	fputs("]", f);
}

static void
put_frames(FILE* f, const cw_crash_info_t* info) {
	fputs("[", f);
	for (int i = 0; i < info->frame_count; ++i) {
		const cw_frame_t* fr = &info->frames[i];
		fprintf(f, "%s{\"module\":", i ? "," : "");
		if (fr->module >= 0) {
			const cw_module_t* m = &info->modules[fr->module];
			put_module_name(f, m);
			fputs(",\"build_id\":", f);
			put_str(f, m->build_id, sizeof(m->build_id));
		} else {
			fputs("null,\"build_id\":null", f);
		}
		fprintf(f, ",\"offset\":%" PRIu64 "}", fr->offset);
	}
	fputs("]", f);
}

bool
cw_write_envelope(
	const char* report_dir, const cw_crash_info_t* info,
	const cw_shared_t* shared, char* out_path, size_t cap
) {
	if (cw_ctx.collect_at_report.collect != NULL) {
		cw_ctx.collect_at_report.collect(cw_ctx.collect_at_report.user);
	}

	const char* exe_build_id = info->main_module >= 0 ? info->modules[info->main_module].build_id : "";
	uint64_t fp = fingerprint(info);
	long long now = (long long)time(NULL);
	char uuid[CW_UUID_CAP];
	make_uuid(uuid);
	snprintf(
		out_path, cap, "%s/pending/%lld_%c_%016" PRIx64 "_%s.json",
		report_dir, now, cw_report_kind_letter(info->kind), fp, uuid
	);
	char tmp_path[CW_STR_CAP + 64];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);

	FILE* f = fopen(tmp_path, "w");
	if (f == NULL) {
		cw_log(CW_LOG_ERROR, "cannot open %s", tmp_path);
		return false;
	}

	fputs("{\"schema\":2,\"report_id\":", f);
	put_str(f, uuid, sizeof(uuid));
	fprintf(f, ",\"sent_at\":%lld,\"app\":{\"name\":", now);
	put_str(f, cw_ctx.app, sizeof(cw_ctx.app));
	fputs(",\"version\":", f);
	put_str(f, cw_ctx.version, sizeof(cw_ctx.version));
	fputs(",\"build_id\":", f);
	put_str(f, exe_build_id, 41);
	fputs(",\"channel\":", f);
	put_str(f, cw_ctx.channel, sizeof(cw_ctx.channel));
	fputs("},\"env\":", f);
	put_env(f, shared);
	fputs(",\"exception\":{\"type\":", f);
	put_str(f, info->type, sizeof(info->type));
	fputs(",\"message_norm\":", f);
	put_norm(f, info->message_raw, sizeof(info->message_raw));
	fputs(",\"message_raw\":", f);
	put_str(f, info->message_raw, sizeof(info->message_raw));
	fprintf(f, ",\"thread\":%" PRIu32 "},\"modules\":", info->tid);
	put_modules(f, info);
	fputs(",\"frames\":", f);
	put_frames(f, info);
	fprintf(f, ",\"client_fp\":\"%016" PRIx64 "\",\"breadcrumbs\":", fp);
	put_crumbs(f, shared);
	fputs(",\"state\":", f);
	put_state(f, shared);
	fputs(",\"attachments\":{\"log_tail\":false,\"minidump\":false,\"snapshot\":false}}\n", f);

	if (fclose(f) != 0 || !cw_platform_replace(tmp_path, out_path)) {
		cw_log(CW_LOG_ERROR, "cannot finish %s", out_path);
		cw_platform_remove(tmp_path);
		return false;
	}
	return true;
}
