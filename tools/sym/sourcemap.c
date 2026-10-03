/**
 * @file sourcemap.c
 * Reader of the source map Emscripten writes with `-gsource-map`: a JSON
 * object whose `mappings` string encodes, segment by segment, a file
 * offset of the module and the source line it came from. Only the keys
 * the reader uses are decoded; every other value is skipped by shape,
 * which keeps `sourcesContent` out of memory.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reader.h"

#define MAX_DEPTH 32

/** A cursor over the JSON text; the first error sticks. */
typedef struct {
	const char* p;
	const char* end;
	bool bad;
} json_t;

static void
skip_ws(json_t* j) {
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
		++j->p;
	}
}

static bool
peek(json_t* j, char c) {
	skip_ws(j);
	return j->p < j->end && *j->p == c;
}

static bool
take(json_t* j, char c) {
	if (peek(j, c)) {
		++j->p;
		return true;
	}
	j->bad = true;
	return false;
}

static int
hex_value(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/** The four hex digits after `\u`, or -1. */
static int
read_hex4(json_t* j) {
	if (j->end - j->p < 4) {
		return -1;
	}
	int v = 0;
	for (int i = 0; i < 4; ++i) {
		int d = hex_value(j->p[i]);
		if (d < 0) {
			return -1;
		}
		v = v * 16 + d;
	}
	j->p += 4;
	return v;
}

static size_t
put_utf8(char* out, uint32_t cp) {
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/** A string, decoded into a fresh buffer the caller frees, or `NULL`. */
static char*
read_string(json_t* j) {
	if (!take(j, '"')) {
		return NULL;
	}
	size_t cap = 64;
	size_t n = 0;
	char* out = malloc(cap);
	while (out != NULL && j->p < j->end && *j->p != '"') {
		if (n + 8 > cap) {
			cap *= 2;
			char* grown = realloc(out, cap);
			if (grown == NULL) {
				free(out);
				out = NULL;
				break;
			}
			out = grown;
		}
		char c = *j->p++;
		if (c != '\\') {
			out[n++] = c;
			continue;
		}
		if (j->p >= j->end) {
			break;
		}
		char e = *j->p++;
		switch (e) {
		case '"': case '\\': case '/':
			out[n++] = e;
			break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case 'n': out[n++] = '\n'; break;
		case 'r': out[n++] = '\r'; break;
		case 't': out[n++] = '\t'; break;
		case 'u': {
			int cp = read_hex4(j);
			if (cp < 0) {
				j->bad = true;
				break;
			}
			if (cp >= 0xd800 && cp < 0xdc00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
				j->p += 2;
				int low = read_hex4(j);
				if (low < 0xdc00 || low >= 0xe000) {
					j->bad = true;
					break;
				}
				cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
			}
			n += put_utf8(out + n, (uint32_t)cp);
			break;
		}
		default:
			j->bad = true;
			break;
		}
		if (j->bad) {
			break;
		}
	}
	if (out == NULL || !take(j, '"')) {
		free(out);
		return NULL;
	}
	out[n] = '\0';
	return out;
}

/** Skip any value: a string, number, literal, array, or object. */
static void
skip_value(json_t* j, int depth) {
	skip_ws(j);
	if (j->p >= j->end || depth > MAX_DEPTH) {
		j->bad = true;
		return;
	}
	char c = *j->p;
	if (c == '"') {
		free(read_string(j));
	} else if (c == '[' || c == '{') {
		char close = c == '[' ? ']' : '}';
		++j->p;
		if (peek(j, close)) {
			++j->p;
			return;
		}
		for (;;) {
			if (c == '{') {
				free(read_string(j));
				take(j, ':');
			}
			skip_value(j, depth + 1);
			if (j->bad) {
				return;
			}
			if (peek(j, ',')) {
				++j->p;
				continue;
			}
			take(j, close);
			return;
		}
	} else {
		while (j->p < j->end && strchr(",]} \t\r\n", *j->p) == NULL) {
			++j->p;
		}
	}
}

/** A string value, or `NULL` for `null`; the string's absence sets `bad`. */
static char*
read_string_or_null(json_t* j) {
	if (peek(j, 'n')) {
		skip_value(j, 0);
		return NULL;
	}
	return read_string(j);
}

/** An array of strings; a `null` entry becomes `""`. */
static char**
read_strings(json_t* j, size_t* count) {
	*count = 0;
	if (!take(j, '[')) {
		return NULL;
	}
	size_t cap = 16;
	char** out = calloc(cap, sizeof(*out));
	if (out == NULL) {
		j->bad = true;
		return NULL;
	}
	if (peek(j, ']')) {
		++j->p;
		return out;
	}
	for (;;) {
		char* s = peek(j, 'n') ? NULL : read_string(j);
		if (s == NULL) {
			if (j->bad) {
				break;
			}
			skip_value(j, 0);
			s = calloc(1, 1);
		}
		if (*count == cap) {
			cap *= 2;
			char** grown = realloc(out, cap * sizeof(*out));
			if (grown == NULL) {
				free(s);
				j->bad = true;
				break;
			}
			out = grown;
		}
		out[(*count)++] = s;
		if (j->bad) {
			break;
		}
		if (peek(j, ',')) {
			++j->p;
			continue;
		}
		take(j, ']');
		break;
	}
	return out;
}

/** What the reader keeps of the map. */
typedef struct {
	char** sources;
	size_t source_count;
	char* source_root;
	char* mappings;
	char* debug_id;
} map_t;

static void
free_map(map_t* m) {
	for (size_t i = 0; i < m->source_count; ++i) {
		free(m->sources[i]);
	}
	free(m->sources);
	free(m->source_root);
	free(m->mappings);
	free(m->debug_id);
}

/** The top-level object; `bad` on anything that is not JSON. */
static bool
parse_map(json_t* j, map_t* m) {
	if (!take(j, '{')) {
		return false;
	}
	if (peek(j, '}')) {
		++j->p;
		return true;
	}
	for (;;) {
		char* key = read_string(j);
		if (key == NULL || !take(j, ':')) {
			free(key);
			return false;
		}
		if (strcmp(key, "sources") == 0) {
			m->sources = read_strings(j, &m->source_count);
		} else if (strcmp(key, "mappings") == 0) {
			m->mappings = read_string_or_null(j);
		} else if (strcmp(key, "debugId") == 0) {
			m->debug_id = read_string_or_null(j);
		} else if (strcmp(key, "sourceRoot") == 0) {
			m->source_root = read_string_or_null(j);
		} else {
			skip_value(j, 0);
		}
		free(key);
		if (j->bad) {
			return false;
		}
		if (peek(j, ',')) {
			++j->p;
			continue;
		}
		return take(j, '}');
	}
}

static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** One base64 VLQ value; `false` at the end of a segment or on a character that is not one. */
static bool
read_vlq(const char** p, int64_t* out) {
	int64_t value = 0;
	int shift = 0;
	for (;;) {
		const char* at = **p != '\0' ? strchr(base64, **p) : NULL;
		if (at == NULL) {
			return false;
		}
		int digit = (int)(at - base64);
		++*p;
		value |= (int64_t)(digit & 31) << shift;
		shift += 5;
		if ((digit & 32) == 0) {
			break;
		}
		if (shift > 60) {
			return false;
		}
	}
	*out = (value & 1) ? -(value >> 1) : (value >> 1);
	return true;
}

/** Whether `hex` spells `id`, in either case. */
static bool
hex_equals(const char* hex, const uint8_t* id, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		int hi = hex_value(hex[2 * i]);
		int lo = hex[2 * i] != '\0' ? hex_value(hex[2 * i + 1]) : -1;
		if (hi < 0 || lo < 0 || (hi * 16 + lo) != id[i]) {
			return false;
		}
	}
	return hex[2 * len] == '\0';
}

static char*
read_text(const char* path, size_t* len, const cwsym_log_t* log) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		cwsym_logf(log, "%s: %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = size >= 0 ? malloc((size_t)size + 1) : NULL;
	if (text == NULL || fread(text, 1, (size_t)size, f) != (size_t)size) {
		cwsym_logf(log, "%s: %s", path, text == NULL ? "out of memory" : "short read");
		free(text);
		text = NULL;
	}
	fclose(f);
	if (text != NULL) {
		text[size] = '\0';
		*len = (size_t)size;
	}
	return text;
}

cwsym_status_t
cwsym_read_source_map(
	const char* path, const uint8_t* build_id, size_t build_id_len,
	const cwsym_map_sink_t* sink, const cwsym_log_t* log
) {
	size_t len;
	char* text = read_text(path, &len, log);
	if (text == NULL) {
		return CWSYM_ERR_IO;
	}
	cwsym_status_t status = CWSYM_ERR_FORMAT;
	map_t m = { 0 };
	char** files = NULL;
	size_t root_len = 0;
	json_t j = { .p = text, .end = text + len };
	if (!parse_map(&j, &m) || m.mappings == NULL || m.sources == NULL) {
		cwsym_logf(log, "%s: not a source map", path);
		goto done;
	}
	if (m.debug_id != NULL && !hex_equals(m.debug_id, build_id, build_id_len)) {
		cwsym_logf(log, "%s: source map is of another build", path);
		status = CWSYM_ERR_NO_DEBUG;
		goto done;
	}

	/* Paths as the map means them: under the root, when it names one. */
	files = calloc(m.source_count > 0 ? m.source_count : 1, sizeof(*files));
	if (files == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}
	root_len = m.source_root != NULL ? strlen(m.source_root) : 0;
	for (size_t i = 0; i < m.source_count; ++i) {
		if (root_len == 0) {
			files[i] = m.sources[i];
			continue;
		}
		bool slash = m.source_root[root_len - 1] == '/';
		size_t cap = root_len + 1 + strlen(m.sources[i]) + 1;
		files[i] = malloc(cap);
		if (files[i] == NULL) {
			status = CWSYM_ERR_NOMEM;
			goto done;
		}
		snprintf(files[i], cap, "%s%s%s", m.source_root, slash ? "" : "/", m.sources[i]);
	}

	status = CWSYM_OK;
	/* The fields are deltas; the source column and the name index are tracked only by the reader that wants them. */
	int64_t column = 0;
	int64_t source = 0;
	int64_t line = 0;
	int64_t last = -1;
	unsigned out_of_order = 0;
	bool stopped = false;
	for (const char* p = m.mappings; *p != '\0' && !stopped;) {
		if (*p == ';') {
			column = 0;
			++p;
			continue;
		}
		if (*p == ',') {
			++p;
			continue;
		}
		int64_t field[5];
		int n = 0;
		while (n < 5 && read_vlq(&p, &field[n])) {
			++n;
		}
		if ((n != 1 && n != 4 && n != 5) || (*p != '\0' && *p != ',' && *p != ';')) {
			cwsym_logf(log, "%s: malformed mappings", path);
			status = CWSYM_ERR_FORMAT;
			goto done;
		}
		column += field[0];
		cwsym_map_segment_t seg = { .offset = (uint32_t)column };
		if (n >= 4) {
			source += field[1];
			line += field[2];
			seg.file = source >= 0 && (size_t)source < m.source_count ? files[source] : NULL;
			seg.line = line >= 0 && line < UINT32_MAX ? (uint32_t)line + 1 : 0;
		}
		if (column < 0 || column > UINT32_MAX || column < last) {
			++out_of_order;
			continue;
		}
		last = column;
		stopped = !sink->segment(sink->user, &seg);
	}
	if (out_of_order > 0) {
		cwsym_logf(log, "%s: %u segments out of order; skipped", path, out_of_order);
	}
	if (sink->end != NULL) {
		sink->end(sink->user);
	}

done:
	if (files != NULL && root_len > 0) {
		for (size_t i = 0; i < m.source_count; ++i) {
			free(files[i]);
		}
	}
	free(files);
	free_map(&m);
	free(text);
	return status;
}
