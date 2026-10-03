/**
 * @file wasm.c
 * Wasm reader. Function bodies come from the code section. Names, lines
 * and inline sites come from the DWARF custom sections through libdw
 * when the build carries them, in the module or in the file its
 * `external_debug_info` section names; names alone come from the name
 * section or the Emscripten symbol map. Identity is the `build_id`
 * custom section. The module is a sequence of length-prefixed vectors,
 * so nothing is loaded to walk it.
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

#define MAX_DEBUG_SECTIONS 16
#define MAX_SECTION_NAME   32

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

static bool
span_starts_with(span_t s, const char* prefix) {
	size_t n = strlen(prefix);
	return s.len >= n && memcmp(s.s, prefix, n) == 0;
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

/** What one module file holds that the reader uses. */
typedef struct {
	const char* path;
	char* own_path;          /**< `path`, when this module was located through another's link. */
	uint8_t* data;
	size_t len;
	uint32_t imports;        /**< Function imports; they come first in the index space. */
	barray(body_t) bodies;
	uint32_t code_start;     /**< File offset of the code section's payload, which DWARF addresses count from. */
	span_t function_names;   /**< The function subsection of the name section, when present. */
	bool has_function_names;
	span_t build_id;
	bool has_build_id;
	span_t debug_link;       /**< `external_debug_info`: the file that holds the DWARF. */
	bool has_debug_link;
	span_t source_map_url;   /**< `sourceMappingURL`: the file that holds the lines when there is no DWARF. */
	bool has_source_map_url;
	cwsym_dwarf_section_t debug[MAX_DEBUG_SECTIONS];
	char debug_names[MAX_DEBUG_SECTIONS][MAX_SECTION_NAME];
	int debug_count;
} module_t;

/**
 * Walk the sections once. Only the imports, the code section, and the
 * custom sections the reader needs are decoded; the rest are skipped by
 * their size.
 */
static bool
parse_sections(module_t* m, const cwsym_log_t* log) {
	cursor_t c = { .data = m->data, .len = m->len, .pos = 8 };
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
						m->function_names = payload;
						m->has_function_names = true;
					}
				}
			} else if (span_equals(name, "build_id")) {
				m->build_id = read_vector(&c);
				m->has_build_id = !c.bad;
			} else if (span_equals(name, "external_debug_info")) {
				m->debug_link = read_vector(&c);
				m->has_debug_link = !c.bad;
			} else if (span_equals(name, "sourceMappingURL")) {
				m->source_map_url = read_vector(&c);
				m->has_source_map_url = !c.bad;
			} else if (span_starts_with(name, ".debug_") && !span_equals(name, ".debug_aranges")) {
				/* The linker tombstones .debug_aranges, and libdw does not need it. */
				if (!c.bad && m->debug_count < MAX_DEBUG_SECTIONS && name.len < MAX_SECTION_NAME) {
					char* dst = m->debug_names[m->debug_count];
					memcpy(dst, name.s, name.len);
					dst[name.len] = '\0';
					m->debug[m->debug_count] = (cwsym_dwarf_section_t){
						.name = dst, .data = c.data + c.pos, .len = end - c.pos,
					};
					++m->debug_count;
				}
			}
		} else if (id == SECTION_IMPORT) {
			for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
				read_vector(&c);
				read_vector(&c);
				uint32_t kind = read_u8(&c);
				switch (kind) {
				case IMPORT_FUNC:
					read_u32(&c);
					++m->imports;
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
					cwsym_logf(log, "%s: import kind %" PRIu32 " is not known", m->path, kind);
					return false;
				}
			}
		} else if (id == SECTION_CODE) {
			m->code_start = (uint32_t)start;
			for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
				uint32_t body_size = read_u32(&c);
				body_t body = { .start = (uint32_t)c.pos, .size = body_size };
				read_span(&c, body_size);
				if (!c.bad) {
					barray_push(m->bodies, body, NULL);
				}
			}
		}
		if (c.pos > end) {
			c.bad = true;
		}
		c.pos = end;
	}
	if (c.bad) {
		cwsym_logf(log, "%s: malformed at offset %#zx", m->path, c.pos);
		return false;
	}
	return true;
}

/** Read and parse the file at `m->path`. */
static cwsym_status_t
load_module(module_t* m, const cwsym_log_t* log) {
	FILE* f = fopen(m->path, "rb");
	if (f == NULL) {
		cwsym_logf(log, "%s: %s", m->path, strerror(errno));
		return CWSYM_ERR_IO;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	m->data = size >= 0 ? malloc((size_t)size + 1) : NULL;
	if (m->data == NULL || fread(m->data, 1, (size_t)size, f) != (size_t)size) {
		cwsym_logf(log, "%s: %s", m->path, m->data == NULL ? "out of memory" : "short read");
		fclose(f);
		return m->data == NULL ? CWSYM_ERR_NOMEM : CWSYM_ERR_IO;
	}
	fclose(f);
	m->len = (size_t)size;

	if (m->len < 8 || memcmp(m->data, "\0asm\x01\0\0\0", 8) != 0) {
		cwsym_logf(log, "%s: not a version 1 Wasm module", m->path);
		return CWSYM_ERR_FORMAT;
	}
	return parse_sections(m, log) ? CWSYM_OK : CWSYM_ERR_FORMAT;
}

static void
free_module(module_t* m) {
	barray_free(m->bodies, NULL);
	free(m->data);
	free(m->own_path);
}

/**
 * The path of the file a link names, by the link's last path component,
 * beside `path`. The caller frees it; `NULL` when out of memory.
 */
static char*
path_beside(const char* path, span_t link) {
	size_t base = 0;
	for (size_t i = 0; i < link.len; ++i) {
		if (link.s[i] == '/' || link.s[i] == '\\') {
			base = i + 1;
		}
	}
	size_t dir_len = 0;
	for (size_t i = 0; path[i] != '\0'; ++i) {
		if (path[i] == '/' || path[i] == '\\') {
			dir_len = i + 1;
		}
	}
	size_t name_len = link.len - base;
	char* out = malloc(dir_len + name_len + 1);
	if (out != NULL) {
		memcpy(out, path, dir_len);
		memcpy(out + dir_len, link.s + base, name_len);
		out[dir_len + name_len] = '\0';
	}
	return out;
}

/**
 * The symbol map emcc writes beside its output when none was named:
 * `<module>.symbols`, or `<stem>.js.symbols` and `<stem>.html.symbols`
 * for `-o <stem>.js` and `-o <stem>.html`. The first that exists, as a
 * path the caller frees; `NULL` when none does.
 */
static char*
guess_symbol_map(const char* path) {
	static const char* const suffixes[] = { ".symbols", ".js.symbols", ".html.symbols" };
	size_t len = strlen(path);
	size_t stem = len >= 5 && strcmp(path + len - 5, ".wasm") == 0 ? len - 5 : len;
	for (size_t k = 0; k < sizeof(suffixes) / sizeof(suffixes[0]); ++k) {
		size_t base = k == 0 ? len : stem;
		char* candidate = malloc(base + strlen(suffixes[k]) + 1);
		if (candidate == NULL) {
			return NULL;
		}
		memcpy(candidate, path, base);
		strcpy(candidate + base, suffixes[k]);
		FILE* f = fopen(candidate, "rb");
		if (f != NULL) {
			fclose(f);
			return candidate;
		}
		free(candidate);
	}
	return NULL;
}

/**
 * Load the file `m`'s `external_debug_info` names, looked up by its last
 * path component beside `m`. It must carry `m`'s build id.
 */
static cwsym_status_t
follow_debug_link(const module_t* m, module_t* debug, const cwsym_log_t* log) {
	debug->own_path = path_beside(m->path, m->debug_link);
	if (debug->own_path == NULL) {
		return CWSYM_ERR_NOMEM;
	}
	debug->path = debug->own_path;

	cwsym_status_t status = load_module(debug, log);
	if (status == CWSYM_ERR_NOMEM) {
		return status;
	}
	if (status != CWSYM_OK) {
		cwsym_logf(log, "%s: its debug file %s cannot be read", m->path, debug->path);
		return CWSYM_ERR_NO_DEBUG;
	}
	if (!debug->has_build_id || debug->build_id.len != m->build_id.len
		|| memcmp(debug->build_id.s, m->build_id.s, m->build_id.len) != 0) {
		cwsym_logf(log, "%s: debug file %s is of another build", m->path, debug->path);
		return CWSYM_ERR_NO_DEBUG;
	}
	return CWSYM_OK;
}

/**
 * Names from the function subsection: a count, then pairs of function
 * index and name. An index outside the module is ignored.
 */
static bool
names_from_section(const module_t* m, span_t* names, uint32_t total, const cwsym_log_t* log) {
	cursor_t c = { .data = m->function_names.s, .len = m->function_names.len };
	for (uint32_t n = read_u32(&c); n > 0 && !c.bad; --n) {
		uint32_t index = read_u32(&c);
		span_t name = read_vector(&c);
		if (!c.bad && index < total) {
			names[index] = name;
		}
	}
	if (c.bad) {
		cwsym_logf(log, "%s: malformed name section", m->path);
		return false;
	}
	return true;
}

/**
 * Names from the symbol map Emscripten writes with `--emit-symbol-map`:
 * one `index:name` per line, imports included. Lines of another shape
 * are skipped. The caller frees the returned text, which the names
 * point into.
 */
static cwsym_status_t
names_from_map(const char* path, span_t* names, uint32_t total, char** text, const cwsym_log_t* log) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		cwsym_logf(log, "%s: %s", path, strerror(errno));
		return CWSYM_ERR_IO;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* map = size >= 0 ? malloc((size_t)size + 1) : NULL;
	if (map == NULL) {
		fclose(f);
		return CWSYM_ERR_NOMEM;
	}
	size_t len = fread(map, 1, (size_t)size, f);
	fclose(f);
	map[len] = '\0';
	*text = map;

	for (char* line = map; *line != '\0';) {
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

/** Index of the body holding file offset `at`, or -1. Bodies are sorted and disjoint. */
static long
body_at(const module_t* m, uint32_t at) {
	size_t lo = 0;
	size_t hi = barray_len(m->bodies);
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (m->bodies[mid].start <= at) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo > 0 && at < m->bodies[lo - 1].start + m->bodies[lo - 1].size ? (long)lo - 1 : -1;
}

/**
 * Passes DWARF rows on, noting which bodies DWARF described. A row that
 * starts in no body is dropped: the post-link optimizer relocates the
 * ranges of code it removed onto offset 0, where they would overlap the
 * first function.
 */
typedef struct {
	const cwsym_sink_t* sink;
	const module_t* m;
	bool* covered;
	bool stopped;
} forward_t;

static bool
forward_symbol(void* user, const cwsym_symbol_t* sym) {
	forward_t* f = user;
	long body = body_at(f->m, sym->start);
	if (body < 0) {
		return true;
	}
	f->covered[body] = true;
	f->stopped = !f->sink->symbol(f->sink->user, sym);
	return !f->stopped;
}

static bool
forward_line(void* user, const cwsym_line_t* line) {
	forward_t* f = user;
	if (body_at(f->m, line->start) < 0) {
		return true;
	}
	f->stopped = !f->sink->line(f->sink->user, line);
	return !f->stopped;
}

static bool
forward_site(void* user, const cwsym_site_t* site) {
	forward_t* f = user;
	if (body_at(f->m, site->start) < 0) {
		return true;
	}
	f->stopped = !f->sink->site(f->sink->user, site);
	return !f->stopped;
}

/**
 * Turns source map segments into line rows: each segment reaches to the
 * next one, clipped to the body it starts in, and adjacent rows of one
 * `file:line` merge. A segment outside every body opens no row.
 */
typedef struct {
	const cwsym_sink_t* sink;
	const module_t* m;
	bool stopped;
	bool open;           /**< A segment waits for the next one to end it. */
	uint32_t start;
	uint32_t body_end;
	const char* file;
	uint32_t line;
	bool have;           /**< A finished row that may still grow. */
	cwsym_line_t done;
} map_rows_t;

static void
flush_row(map_rows_t* r) {
	if (r->have) {
		r->have = false;
		r->stopped = !r->sink->line(r->sink->user, &r->done);
	}
}

static void
close_row(map_rows_t* r, uint32_t at) {
	if (!r->open) {
		return;
	}
	r->open = false;
	uint32_t end = at < r->body_end ? at : r->body_end;
	if (end <= r->start) {
		return;
	}
	if (r->have && r->done.line == r->line && r->done.start + r->done.len == r->start
		&& strcmp(r->done.file, r->file) == 0) {
		r->done.len += end - r->start;
		return;
	}
	flush_row(r);
	if (!r->stopped) {
		r->done = (cwsym_line_t){ .start = r->start, .len = end - r->start, .file = r->file, .line = r->line };
		r->have = true;
	}
}

static bool
map_segment(void* user, const cwsym_map_segment_t* seg) {
	map_rows_t* r = user;
	close_row(r, seg->offset);
	if (r->stopped) {
		return false;
	}
	long body = seg->file != NULL ? body_at(r->m, seg->offset) : -1;
	if (body >= 0) {
		r->open = true;
		r->start = seg->offset;
		r->body_end = r->m->bodies[body].start + r->m->bodies[body].size;
		r->file = seg->file;
		r->line = seg->line;
	}
	return true;
}

static void
map_end(void* user) {
	map_rows_t* r = user;
	close_row(r, UINT32_MAX);
	if (!r->stopped) {
		flush_row(r);
	}
}

cwsym_status_t
cwsym_read_wasm(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	module_t m = { .path = path };
	module_t debug = { 0 };
	span_t* names = NULL;
	char* map = NULL;
	char* guessed_map = NULL;
	char* source_map = NULL;
	char* name = NULL;
	bool* covered = NULL;

	cwsym_status_t status = load_module(&m, log);
	if (status != CWSYM_OK) {
		goto done;
	}
	if (!m.has_build_id) {
		cwsym_logf(log, "%s: no build_id section; link with -Wl,--build-id=sha1", path);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}
	if (m.build_id.len == 0 || m.build_id.len > CWSYM_BUILD_ID_CAP) {
		cwsym_logf(
			log, "%s: build id is %zu bytes; at most %d fit, link with -Wl,--build-id=sha1",
			path, m.build_id.len, CWSYM_BUILD_ID_CAP
		);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}

	/* DWARF lives in the module or in the file it points at. */
	const module_t* dwarf = m.debug_count > 0 ? &m : NULL;
	if (dwarf == NULL && m.has_debug_link) {
		status = follow_debug_link(&m, &debug, log);
		if (status != CWSYM_OK) {
			goto done;
		}
		dwarf = debug.debug_count > 0 ? &debug : NULL;
	}

	uint32_t body_count = (uint32_t)barray_len(m.bodies);
	uint32_t total = m.imports + body_count;
	names = calloc(total > 0 ? total : 1, sizeof(*names));
	covered = calloc(body_count > 0 ? body_count : 1, sizeof(*covered));
	if (names == NULL || covered == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}
	bool has_names = false;
	if (m.has_function_names) {
		if (!names_from_section(&m, names, total, log)) {
			status = CWSYM_ERR_FORMAT;
			goto done;
		}
		has_names = true;
	} else {
		const char* map_path = opts->symbol_map;
		if (map_path == NULL) {
			guessed_map = guess_symbol_map(path);
			map_path = guessed_map;
			if (map_path != NULL) {
				cwsym_logf(log, "%s: names from %s", path, map_path);
			}
		}
		if (map_path != NULL) {
			status = names_from_map(map_path, names, total, &map, log);
			if (status != CWSYM_OK) {
				goto done;
			}
			has_names = true;
		}
	}
	if (dwarf == NULL && !has_names) {
		cwsym_logf(
			log, "%s: neither DWARF nor a name section; build with -g, keep the names with -g2 or "
			"--profiling-funcs, or pass --symbol-map with the .symbols file of --emit-symbol-map", path
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

	cwsym_module_t mod = { .arch = CWSYM_ARCH_WASM32, .build_id_len = (uint8_t)m.build_id.len };
	memcpy(mod.build_id, m.build_id.s, m.build_id.len);
	if (sink->begin != NULL) {
		sink->begin(sink->user, &mod);
	}

	bool stopped = false;
	bool dwarf_lines = false;
	if (dwarf != NULL) {
		forward_t f = { .sink = sink, .m = &m, .covered = covered };
		cwsym_sink_t forward = {
			.symbol = forward_symbol,
			.line = sink->line != NULL ? forward_line : NULL,
			.site = sink->site != NULL ? forward_site : NULL,
			.user = &f,
		};
		status = cwsym_read_dwarf(dwarf->path, dwarf->debug, dwarf->debug_count, (int64_t)m.code_start, &forward, log);
		dwarf_lines = status == CWSYM_OK;
		if (status == CWSYM_ERR_UNSUPPORTED && has_names) {
			cwsym_logf(log, "%s: its DWARF is skipped on this host; names only", path);
			status = CWSYM_OK;
		}
		stopped = f.stopped;
	}

	/* What DWARF did not describe is named by the name section, else by index. */
	uint32_t unnamed = 0;
	for (uint32_t i = 0; status == CWSYM_OK && !stopped && i < body_count; ++i) {
		if (covered[i]) {
			continue;
		}
		uint32_t index = m.imports + i;
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
			.start = m.bodies[i].start,
			.size = m.bodies[i].size,
			.scope = scope,
			.scope_len = 1,
			/* A demangled spelling carries the parameter list, which is the display column. */
			.display = strchr(name, '(') != NULL ? name : NULL,
		};
		stopped = !sink->symbol(sink->user, &sym);
	}
	if (unnamed > 0) {
		cwsym_logf(log, "%s: %" PRIu32 " functions have no name and are listed by index", path, unnamed);
	}

	/* Without DWARF lines, the source map: the one asked for, else the one the module names. */
	if (status == CWSYM_OK && !stopped && !dwarf_lines && sink->line != NULL) {
		if (opts->source_map == NULL && m.has_source_map_url) {
			source_map = path_beside(path, m.source_map_url);
		}
		const char* map_path = opts->source_map != NULL ? opts->source_map : source_map;
		if (map_path != NULL) {
			map_rows_t rows = { .sink = sink, .m = &m };
			cwsym_map_sink_t segments = { .segment = map_segment, .end = map_end, .user = &rows };
			cwsym_status_t lines = cwsym_read_source_map(map_path, m.build_id.s, m.build_id.len, &segments, log);
			if (lines != CWSYM_OK && opts->source_map != NULL) {
				status = lines;
			} else if (lines != CWSYM_OK) {
				cwsym_logf(log, "%s: no lines; its source map %s was not used", path, map_path);
			}
			stopped = rows.stopped;
		}
	}
	if (sink->end != NULL) {
		sink->end(sink->user, status);
	}

done:
	free(covered);
	free(name);
	free(source_map);
	free(guessed_map);
	free(map);
	free(names);
	free_module(&debug);
	free_module(&m);
	return status;
}
