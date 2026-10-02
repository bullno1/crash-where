/**
 * @file wasm.c
 * Wasm reader: function bodies from the code section, names from the
 * name section or the Emscripten symbol map, identity from the
 * `build_id` custom section. The module is a sequence of
 * length-prefixed vectors, so nothing is loaded to read it.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reader.h"

#define BARRAY_API static inline
#define BARRAY_IMPLEMENTATION
#include "vendor/barray.h"

enum {
	SECTION_CUSTOM   = 0,
	SECTION_IMPORT   = 2,
	SECTION_CODE     = 10,
	NAMES_FUNCTIONS  = 1,  /**< Subsection of the name section that names functions. */
	IMPORT_FUNC      = 0,
	IMPORT_TABLE     = 1,
	IMPORT_MEMORY    = 2,
	IMPORT_GLOBAL    = 3,
	IMPORT_TAG       = 4,
	LIMITS_HAS_MAX   = 1,
};

/** Bytes of the file not yet consumed; the first bad read sticks. */
typedef struct {
	const uint8_t* data;
	size_t len;
	size_t pos;
	bool bad;
} cursor_t;

/** A span of bytes inside a buffer, not NUL-terminated. */
typedef struct {
	const uint8_t* s;
	size_t len;
} span_t;

/** One function body in the code section. */
typedef struct {
	uint32_t start; /**< File offset of the body, after its size prefix. */
	uint32_t size;
} body_t;

static uint32_t
read_u8(cursor_t* c) {
	if (c->bad || c->pos >= c->len) {
		c->bad = true;
		return 0;
	}
	return c->data[c->pos++];
}

/** An unsigned LEB128 of at most `max_bytes`. */
static uint64_t
read_leb(cursor_t* c, int max_bytes) {
	uint64_t value = 0;
	for (int i = 0; i < max_bytes; ++i) {
		uint32_t byte = read_u8(c);
		if (c->bad) {
			return 0;
		}
		value |= (uint64_t)(byte & 0x7f) << (7 * i);
		if ((byte & 0x80) == 0) {
			return value;
		}
	}
	c->bad = true;
	return 0;
}

static uint32_t
read_u32(cursor_t* c) {
	uint64_t v = read_leb(c, 5);
	if (v > UINT32_MAX) {
		c->bad = true;
		return 0;
	}
	return (uint32_t)v;
}

static span_t
read_span(cursor_t* c, size_t n) {
	if (c->bad || n > c->len - c->pos) {
		c->bad = true;
		return (span_t){ 0 };
	}
	span_t s = { .s = c->data + c->pos, .len = n };
	c->pos += n;
	return s;
}

/** A length-prefixed byte vector: names in imports and custom sections. */
static span_t
read_vector(cursor_t* c) {
	uint32_t n = read_u32(c);
	return read_span(c, n);
}

static bool
span_equals(span_t s, const char* text) {
	return s.len == strlen(text) && memcmp(s.s, text, s.len) == 0;
}

/** A value type: one byte, or a reference type followed by its heap type. */
static void
skip_valtype(cursor_t* c) {
	uint32_t t = read_u8(c);
	if (t == 0x63 || t == 0x64) {
		read_leb(c, 10);
	}
}

/** Table and memory limits; a 64-bit memory's are still LEB128. */
static void
skip_limits(cursor_t* c) {
	uint32_t flags = read_u8(c);
	read_leb(c, 10);
	if (flags & LIMITS_HAS_MAX) {
		read_leb(c, 10);
	}
}

/** State of one read. */
typedef struct {
	const char* path;
	const cwsym_log_t* log;
	uint8_t* data;
	size_t len;
	uint32_t imports;        /**< Function imports; they come first in the index space. */
	barray(body_t) bodies;
	span_t function_names;   /**< The function subsection of the name section, when present. */
	bool has_function_names;
	span_t build_id;
	bool has_build_id;
	char* map;               /**< Contents of the symbol map, when read. */
} reader_t;

/**
 * Walk the sections once. Only the imports, the code section, and the
 * two custom sections the reader needs are decoded; the rest are skipped
 * by their size.
 */
static bool
parse_sections(reader_t* r) {
	cursor_t c = { .data = r->data, .len = r->len, .pos = 8 };
	while (!c.bad && c.pos < c.len) {
		uint32_t id = read_u8(&c);
		uint32_t size = read_u32(&c);
		size_t start = c.pos;
		if (c.bad || size > c.len - start) {
			c.bad = true;
			break;
		}
		size_t end = start + size;
		if (id == SECTION_CUSTOM) {
			span_t name = read_vector(&c);
			if (span_equals(name, "name")) {
				while (!c.bad && c.pos < end) {
					uint32_t sub = read_u8(&c);
					span_t payload = read_vector(&c);
					if (sub == NAMES_FUNCTIONS) {
						r->function_names = payload;
						r->has_function_names = true;
					}
				}
			} else if (span_equals(name, "build_id")) {
				r->build_id = read_vector(&c);
				r->has_build_id = !c.bad;
			}
		} else if (id == SECTION_IMPORT) {
			for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
				read_vector(&c);
				read_vector(&c);
				uint32_t kind = read_u8(&c);
				switch (kind) {
				case IMPORT_FUNC:
					read_u32(&c);
					++r->imports;
					break;
				case IMPORT_TABLE:
					skip_valtype(&c);
					skip_limits(&c);
					break;
				case IMPORT_MEMORY:
					skip_limits(&c);
					break;
				case IMPORT_GLOBAL:
					skip_valtype(&c);
					read_u8(&c);
					break;
				case IMPORT_TAG:
					read_u8(&c);
					read_u32(&c);
					break;
				default:
					cwsym_logf(r->log, "%s: import kind %" PRIu32 " is not known", r->path, kind);
					return false;
				}
			}
		} else if (id == SECTION_CODE) {
			for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
				uint32_t body_size = read_u32(&c);
				body_t body = { .start = (uint32_t)c.pos, .size = body_size };
				read_span(&c, body_size);
				if (!c.bad) {
					barray_push(r->bodies, body, NULL);
				}
			}
		}
		if (c.pos > end) {
			c.bad = true;
		}
		c.pos = end;
	}
	if (c.bad) {
		cwsym_logf(r->log, "%s: malformed at offset %#zx", r->path, c.pos);
		return false;
	}
	return true;
}

/**
 * Names from the function subsection: a count, then pairs of function
 * index and name. An index outside the module is ignored.
 */
static bool
names_from_section(reader_t* r, span_t* names, uint32_t total) {
	cursor_t c = { .data = r->function_names.s, .len = r->function_names.len };
	for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
		uint32_t index = read_u32(&c);
		span_t name = read_vector(&c);
		if (!c.bad && index < total) {
			names[index] = name;
		}
	}
	if (c.bad) {
		cwsym_logf(r->log, "%s: malformed name section", r->path);
		return false;
	}
	return true;
}

/**
 * Names from the symbol map Emscripten writes with `--emit-symbol-map`:
 * one `index:name` per line, imports included. Lines of another shape
 * are skipped.
 */
static cwsym_status_t
names_from_map(reader_t* r, const char* path, span_t* names, uint32_t total) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		cwsym_logf(r->log, "%s: %s", path, strerror(errno));
		return CWSYM_ERR_IO;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	r->map = size >= 0 ? malloc((size_t)size + 1) : NULL;
	if (r->map == NULL) {
		fclose(f);
		return CWSYM_ERR_NOMEM;
	}
	size_t len = fread(r->map, 1, (size_t)size, f);
	fclose(f);
	r->map[len] = '\0';

	for (char* line = r->map; *line != '\0';) {
		size_t line_len = strcspn(line, "\n");
		char* next = line + line_len + (line[line_len] == '\n');
		if (line_len > 0 && line[line_len - 1] == '\r') {
			--line_len;
		}
		char* colon;
		unsigned long index = strtoul(line, &colon, 10);
		if (colon != line && colon < line + line_len && *colon == ':' && index < total) {
			names[index] = (span_t){ .s = (const uint8_t*)colon + 1, .len = (size_t)(line + line_len - colon - 1) };
		}
		line = next;
	}
	return CWSYM_OK;
}

static uint8_t*
read_file(const char* path, size_t* len, const cwsym_log_t* log) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		cwsym_logf(log, "%s: %s", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = size >= 0 ? malloc((size_t)size + 1) : NULL;
	if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
		cwsym_logf(log, "%s: %s", path, data == NULL ? "out of memory" : "short read");
		free(data);
		data = NULL;
	}
	fclose(f);
	*len = data != NULL ? (size_t)size : 0;
	return data;
}

cwsym_status_t
cwsym_read_wasm(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	cwsym_status_t status = CWSYM_ERR_FORMAT;
	reader_t r = { .path = path, .log = log };
	span_t* names = NULL;
	char* name = NULL;

	r.data = read_file(path, &r.len, log);
	if (r.data == NULL) {
		return CWSYM_ERR_IO;
	}
	if (r.len < 8 || memcmp(r.data + 4, "\x01\x00\x00\x00", 4) != 0) {
		cwsym_logf(log, "%s: not a version 1 Wasm module", path);
		goto done;
	}
	if (!parse_sections(&r)) {
		goto done;
	}

	if (!r.has_build_id) {
		cwsym_logf(log, "%s: no build_id section; link with -Wl,--build-id=sha1", path);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}
	if (r.build_id.len == 0 || r.build_id.len > CWSYM_BUILD_ID_CAP) {
		cwsym_logf(
			log, "%s: build id is %zu bytes; at most %d fit, link with -Wl,--build-id=sha1",
			path, r.build_id.len, CWSYM_BUILD_ID_CAP
		);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}

	uint32_t body_count = (uint32_t)barray_len(r.bodies);
	uint32_t total = r.imports + body_count;
	names = calloc(total > 0 ? total : 1, sizeof(*names));
	if (names == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}
	if (r.has_function_names) {
		if (!names_from_section(&r, names, total)) {
			goto done;
		}
	} else if (opts->symbol_map != NULL) {
		status = names_from_map(&r, opts->symbol_map, names, total);
		if (status != CWSYM_OK) {
			goto done;
		}
	} else {
		cwsym_logf(
			log, "%s: no name section; keep it with -g2 or --profiling-funcs, "
			"or pass --symbol-map with the .symbols file of --emit-symbol-map", path
		);
		status = CWSYM_ERR_NO_DEBUG;
		goto done;
	}

	size_t longest = 0;
	for (uint32_t i = 0; i < total; ++i) {
		longest = names[i].len > longest ? names[i].len : longest;
	}
	name = malloc(longest + 32);
	if (name == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}

	cwsym_module_t mod = { .arch = CWSYM_ARCH_WASM32, .build_id_len = (uint8_t)r.build_id.len };
	memcpy(mod.build_id, r.build_id.s, r.build_id.len);
	if (sink->begin != NULL) {
		sink->begin(sink->user, &mod);
	}
	status = CWSYM_OK;
	uint32_t unnamed = 0;
	for (uint32_t i = 0; i < body_count; ++i) {
		uint32_t index = r.imports + i;
		span_t n = names[index];
		if (n.len > 0) {
			memcpy(name, n.s, n.len);
			name[n.len] = '\0';
		} else {
			/* What every engine prints for it. */
			snprintf(name, longest + 32, "wasm-function[%" PRIu32 "]", index);
			++unnamed;
		}
		const char* scope[] = { name };
		cwsym_symbol_t sym = {
			.start = r.bodies[i].start,
			.size = r.bodies[i].size,
			.scope = scope,
			.scope_len = 1,
			/* A demangled spelling carries the parameter list, which is the display column. */
			.display = strchr(name, '(') != NULL ? name : NULL,
		};
		if (!sink->symbol(sink->user, &sym)) {
			break;
		}
	}
	if (unnamed > 0) {
		cwsym_logf(log, "%s: %" PRIu32 " functions have no name and are listed by index", path, unnamed);
	}
	if (sink->end != NULL) {
		sink->end(sink->user, status);
	}

done:
	free(name);
	free(names);
	free(r.map);
	barray_free(r.bodies, NULL);
	free(r.data);
	return status;
}
