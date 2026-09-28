/**
 * @file table.c
 * The `cwsym` table: builder, writer, parser, lookup, symbolizer, dump.
 *
 * The builder collects rows in dynamic arrays and interns strings in a
 * hash table whose keys point into a bump arena, so a string's address
 * never moves while more are added. Rows refer to strings by table index
 * until the end, when offsets are assigned in order of first use by the
 * sorted sections, so the file does not depend on the order the reader
 * produced things in. Ending the builder sorts and checks every section,
 * serializes into one little-endian buffer, and parses that buffer back,
 * so a table that was just built has passed the same validation as one
 * read from disk.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format.h"
#include "reader.h"

/* Vendored containers, with internal linkage so nothing leaks from the library. */
#define BARRAY_API static inline
#define BHASH_API static inline
#include "vendor/barray.h"
#include "vendor/bhash.h"

#define MAX_NAME  4096
#define MAX_DEPTH 128

/* Strings {{{ */

#define NO_STRING   UINT32_MAX /**< A row's reference to the empty string, offset 0 in the file. */
#define UNASSIGNED  UINT32_MAX
#define CHUNK_FIRST 4096u
#define CHUNK_MAX   (16u << 20)

/**
 * One chunk of the bump arena. Strings are copied in and never move;
 * a new chunk, twice the size, is chained on when one runs out.
 */
typedef struct {
	void* prev;
	char* cur;
	char* end;
	char data[];
} chunk_t;

typedef struct {
	chunk_t* head;
	size_t next_size;
} arena_t;

static char*
arena_add(arena_t* a, const char* s, size_t need) {
	if (a->head == NULL || (size_t)(a->head->end - a->head->cur) < need) {
		size_t size = a->next_size != 0 ? a->next_size : CHUNK_FIRST;
		if (size < need) {
			size = need;
		}
		chunk_t* c = malloc(sizeof(*c) + size);
		if (c == NULL) {
			return NULL;
		}
		c->prev = a->head;
		c->cur = c->data;
		c->end = c->data + size;
		a->head = c;
		a->next_size = size < CHUNK_MAX ? size * 2 : CHUNK_MAX;
	}
	char* dst = a->head->cur;
	memcpy(dst, s, need);
	a->head->cur += need;
	return dst;
}

static void
arena_free(arena_t* a) {
	for (chunk_t* c = a->head; c != NULL;) {
		chunk_t* prev = c->prev;
		free(c);
		c = prev;
	}
	a->head = NULL;
}

/**
 * A string in the arena: the key of the intern set. Only `s` and `len`
 * take part in hashing and equality; `offset` is the string's place in
 * the file, assigned at the end.
 */
typedef struct {
	const char* s;
	uint32_t len;
	uint32_t offset;
} str_t;

typedef BHASH_SET(str_t) str_set_t;

/**
 * One deduplicated string section under construction. The empty string
 * is never stored; it is offset 0 of every section.
 */
typedef struct {
	arena_t arena;
	str_set_t set;
	barray(uint32_t) order; /**< Indices in the order their offsets were assigned. */
	uint32_t len;           /**< Bytes assigned so far, starting past the empty string. */
	bool failed;
} strings_t;

/** The table's own hash over the string's bytes rather than over the key struct. */
static bhash_hash_t
hash_str(const void* key, size_t size) {
	(void)size;
	const str_t* k = key;
	return bhash_hash(k->s, k->len);
}

static bool
eq_str(const void* lhs, const void* rhs, size_t size) {
	(void)size;
	const str_t* a = lhs;
	const str_t* b = rhs;
	return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
}

static void
strings_init(strings_t* st) {
	bhash_config_t config = bhash_config_default();
	config.hash = hash_str;
	config.eq = eq_str;
	config.put_only = true;
	*st = (strings_t){ .len = 1 };
	bhash_init_set(&st->set, &config);
}

static void
strings_free(strings_t* st) {
	bhash_cleanup(&st->set);
	barray_free(st->order, NULL);
	arena_free(&st->arena);
}

/** Table index of `s`, adding it when new; ::NO_STRING for `NULL` or empty. */
static uint32_t
intern(strings_t* st, const char* s) {
	if (s == NULL || s[0] == '\0' || st->failed) {
		return NO_STRING;
	}
	size_t n = strlen(s);
	if (n >= UINT32_MAX / 2) {
		st->failed = true;
		return NO_STRING;
	}
	str_t key = { .s = s, .len = (uint32_t)n };
	bhash_alloc_result_t r = bhash_alloc(&st->set, key);
	if (!r.is_new) {
		return (uint32_t)r.index;
	}
	char* copy = arena_add(&st->arena, s, n + 1);
	if (copy == NULL) {
		st->set.keys[r.index] = (str_t){ .s = "", .len = 0, .offset = UNASSIGNED };
		st->failed = true;
		return NO_STRING;
	}
	st->set.keys[r.index] = (str_t){ .s = copy, .len = (uint32_t)n, .offset = UNASSIGNED };
	return (uint32_t)r.index;
}

/** The string behind an index, for messages; empty for ::NO_STRING. */
static const char*
str_at(const strings_t* st, uint32_t index) {
	return index == NO_STRING ? "" : st->set.keys[index].s;
}

/**
 * Turn a row's index into its file offset, assigning the next free
 * offset on first use. Called in section order so the blob is laid out
 * by first reference.
 */
static void
assign(strings_t* st, uint32_t* ref) {
	if (*ref == NO_STRING) {
		*ref = 0;
		return;
	}
	str_t* entry = &st->set.keys[*ref];
	if (entry->offset == UNASSIGNED) {
		entry->offset = st->len;
		st->len += entry->len + 1;
		barray_push(st->order, *ref, NULL);
	}
	*ref = entry->offset;
}

/** The finished section: every assigned string at its offset. */
static char*
strings_blob(const strings_t* st) {
	char* blob = malloc(st->len);
	if (blob == NULL) {
		return NULL;
	}
	blob[0] = '\0';
	for (size_t i = 0; i < barray_len(st->order); ++i) {
		const str_t* entry = &st->set.keys[st->order[i]];
		memcpy(blob + entry->offset, entry->s, entry->len + 1u);
	}
	return blob;
}

/* }}} */

/* Builder {{{ */

typedef struct {
	uint32_t start;
	uint32_t size;
	uint32_t name;
	uint32_t disp;
	bool is_static;
} row_t;

typedef struct {
	uint32_t start;
	uint32_t len;
	uint32_t file;
	uint32_t line;
} line_row_t;

typedef struct {
	uint32_t start;
	uint32_t len;
	uint32_t callee;
	uint32_t call_file;
	uint32_t call_line;
	uint32_t parent;
	int depth;
} site_row_t;

struct cwsym_table_builder_s {
	cwsym_module_t module;
	bool has_module;
	barray(row_t) rows;
	barray(line_row_t) lines;
	barray(site_row_t) sites;
	strings_t strings;
	strings_t dstrings;
	cwsym_status_t error;
};

cwsym_table_builder_t*
cwsym_table_begin(void) {
	cwsym_table_builder_t* b = calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	strings_init(&b->strings);
	strings_init(&b->dstrings);
	return b;
}

static void
builder_free(cwsym_table_builder_t* b) {
	barray_free(b->rows, NULL);
	barray_free(b->lines, NULL);
	barray_free(b->sites, NULL);
	strings_free(&b->strings);
	strings_free(&b->dstrings);
	free(b);
}

/** Remember the first failure; later calls report it and change nothing. */
static cwsym_status_t
fail(cwsym_table_builder_t* b, cwsym_status_t status) {
	if (b->error == CWSYM_OK) {
		b->error = status;
	}
	return b->error;
}

void
cwsym_table_set_module(cwsym_table_builder_t* b, const cwsym_module_t* mod) {
	b->module = *mod;
	b->has_module = true;
}

cwsym_status_t
cwsym_table_add(cwsym_table_builder_t* b, const cwsym_symbol_t* sym) {
	if (b->error != CWSYM_OK) {
		return b->error;
	}
	char name[MAX_NAME];
	if (sym->scope_len < 1 || !cwsym_normalize(sym, name, sizeof(name)) || name[0] == '\0') {
		return fail(b, CWSYM_ERR_INVALID);
	}
	row_t row = {
		.start = sym->start,
		.size = sym->size,
		.name = intern(&b->strings, name),
		.disp = intern(&b->dstrings, sym->display),
		.is_static = sym->is_static,
	};
	if (b->strings.failed || b->dstrings.failed) {
		return fail(b, CWSYM_ERR_NOMEM);
	}
	if (row.name == NO_STRING) {
		return fail(b, CWSYM_ERR_INVALID);
	}
	barray_push(b->rows, row, NULL);
	return CWSYM_OK;
}

cwsym_status_t
cwsym_table_add_line(cwsym_table_builder_t* b, const cwsym_line_t* line) {
	if (b->error != CWSYM_OK) {
		return b->error;
	}
	if (line->file == NULL) {
		return fail(b, CWSYM_ERR_INVALID);
	}
	line_row_t row = {
		.start = line->start,
		.len = line->len,
		.file = intern(&b->dstrings, line->file),
		.line = line->line,
	};
	if (b->dstrings.failed) {
		return fail(b, CWSYM_ERR_NOMEM);
	}
	barray_push(b->lines, row, NULL);
	return CWSYM_OK;
}

cwsym_status_t
cwsym_table_add_site(cwsym_table_builder_t* b, const cwsym_site_t* site) {
	if (b->error != CWSYM_OK) {
		return b->error;
	}
	if (site->callee == NULL || site->depth < 0 || site->depth >= MAX_DEPTH) {
		return fail(b, CWSYM_ERR_INVALID);
	}
	site_row_t row = {
		.start = site->start,
		.len = site->len,
		.callee = intern(&b->dstrings, site->callee),
		.call_file = intern(&b->dstrings, site->call_file),
		.call_line = site->call_line,
		.parent = CWSYM_NO_PARENT,
		.depth = site->depth,
	};
	if (b->dstrings.failed) {
		return fail(b, CWSYM_ERR_NOMEM);
	}
	barray_push(b->sites, row, NULL);
	return CWSYM_OK;
}

static void
sink_begin(void* user, const cwsym_module_t* mod) {
	cwsym_table_set_module(user, mod);
}

static bool
sink_symbol(void* user, const cwsym_symbol_t* sym) {
	return cwsym_table_add(user, sym) == CWSYM_OK;
}

static bool
sink_line(void* user, const cwsym_line_t* line) {
	return cwsym_table_add_line(user, line) == CWSYM_OK;
}

static bool
sink_site(void* user, const cwsym_site_t* site) {
	return cwsym_table_add_site(user, site) == CWSYM_OK;
}

static void
sink_end(void* user, cwsym_status_t status) {
	if (status != CWSYM_OK) {
		fail(user, status);
	}
}

cwsym_sink_t
cwsym_table_sink(cwsym_table_builder_t* b, bool lines) {
	return (cwsym_sink_t){
		.begin = sink_begin,
		.symbol = sink_symbol,
		.line = lines ? sink_line : NULL,
		.site = lines ? sink_site : NULL,
		.end = sink_end,
		.user = b,
	};
}

/* }}} */

/* Finishing {{{ */

/** The name strings, for the row comparator; `qsort` carries no context. */
static const strings_t* sort_names;

/** Rows by start; at one start the external one, then the larger, then the name. */
static int
compare_rows(const void* pa, const void* pb) {
	const row_t* a = pa;
	const row_t* b = pb;
	if (a->start != b->start) {
		return a->start < b->start ? -1 : 1;
	}
	if (a->is_static != b->is_static) {
		return a->is_static ? 1 : -1;
	}
	if (a->size != b->size) {
		return a->size > b->size ? -1 : 1;
	}
	return strcmp(str_at(sort_names, a->name), str_at(sort_names, b->name));
}

static int
compare_lines(const void* pa, const void* pb) {
	const line_row_t* a = pa;
	const line_row_t* b = pb;
	return a->start < b->start ? -1 : a->start > b->start ? 1 : 0;
}

static int
compare_sites(const void* pa, const void* pb) {
	const site_row_t* a = pa;
	const site_row_t* b = pb;
	if (a->start != b->start) {
		return a->start < b->start ? -1 : 1;
	}
	return a->depth < b->depth ? -1 : a->depth > b->depth ? 1 : 0;
}

/** Index of the last row whose start is not above `offset`, or -1. */
static ptrdiff_t
last_row_at(const row_t* rows, size_t count, uint32_t offset) {
	size_t lo = 0;
	size_t hi = count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (rows[mid].start <= offset) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return (ptrdiff_t)lo - 1;
}

static bool
row_contains(const row_t* r, uint32_t start, uint32_t len) {
	return r->start <= start && start + len <= r->start + r->size && len <= r->size;
}

/**
 * Sort rows, fold same-start rows, and refuse any other overlap.
 * Returns the surviving count.
 */
static size_t
finish_rows(cwsym_table_builder_t* b, const cwsym_log_t* log, cwsym_status_t* status) {
	const strings_t* names = &b->strings;
	sort_names = names;
	qsort(b->rows, barray_len(b->rows), sizeof(row_t), compare_rows);
	size_t n = 0;
	for (size_t i = 0; i < barray_len(b->rows); ++i) {
		if (n > 0 && b->rows[n - 1].start == b->rows[i].start) {
			continue;
		}
		if (n > 0 && b->rows[n - 1].start + b->rows[n - 1].size > b->rows[i].start) {
			cwsym_logf(
				log, "%s [%#" PRIx32 ", %#" PRIx32 ") overlaps %s at %#" PRIx32,
				str_at(names, b->rows[n - 1].name), b->rows[n - 1].start,
				b->rows[n - 1].start + b->rows[n - 1].size, str_at(names, b->rows[i].name), b->rows[i].start
			);
			*status = CWSYM_ERR_INVALID;
			return 0;
		}
		b->rows[n++] = b->rows[i];
	}
	return n;
}

/** Sort line rows, drop empty ones, and refuse overlaps. */
static size_t
finish_lines(cwsym_table_builder_t* b, const cwsym_log_t* log, cwsym_status_t* status) {
	const strings_t* dstrings = &b->dstrings;
	qsort(b->lines, barray_len(b->lines), sizeof(line_row_t), compare_lines);
	size_t n = 0;
	for (size_t i = 0; i < barray_len(b->lines); ++i) {
		const line_row_t* l = &b->lines[i];
		if (l->len == 0) {
			continue;
		}
		if (n > 0 && b->lines[n - 1].start + b->lines[n - 1].len > l->start) {
			cwsym_logf(
				log, "line rows overlap at %#" PRIx32 ": %s:%" PRIu32 " and %s:%" PRIu32,
				l->start, str_at(dstrings, b->lines[n - 1].file), b->lines[n - 1].line, str_at(dstrings, l->file), l->line
			);
			*status = CWSYM_ERR_INVALID;
			return 0;
		}
		b->lines[n++] = *l;
	}
	return n;
}

/**
 * Sort sites by start and depth, drop empty ones, place each inside a
 * function row and inside the most recent site one level up.
 */
static size_t
finish_sites(cwsym_table_builder_t* b, size_t row_count, const cwsym_log_t* log, cwsym_status_t* status) {
	const strings_t* dstrings = &b->dstrings;
	qsort(b->sites, barray_len(b->sites), sizeof(site_row_t), compare_sites);
	uint32_t last[MAX_DEPTH];
	for (int d = 0; d < MAX_DEPTH; ++d) {
		last[d] = CWSYM_NO_PARENT;
	}
	size_t n = 0;
	for (size_t i = 0; i < barray_len(b->sites); ++i) {
		site_row_t s = b->sites[i];
		if (s.len == 0) {
			continue;
		}
		ptrdiff_t r = last_row_at(b->rows, row_count, s.start);
		if (r < 0 || !row_contains(&b->rows[r], s.start, s.len)) {
			cwsym_logf(
				log, "inline site %s [%#" PRIx32 ", %#" PRIx32 ") lies outside every function",
				str_at(dstrings, s.callee), s.start, s.start + s.len
			);
			*status = CWSYM_ERR_INVALID;
			return 0;
		}
		if (s.depth > 0) {
			uint32_t p = last[s.depth - 1];
			const site_row_t* parent = p != CWSYM_NO_PARENT ? &b->sites[p] : NULL;
			if (parent == NULL || parent->start > s.start || s.start + s.len > parent->start + parent->len) {
				cwsym_logf(
					log, "inline site %s [%#" PRIx32 ", %#" PRIx32 ") at depth %d has no enclosing site",
					str_at(dstrings, s.callee), s.start, s.start + s.len, s.depth
				);
				*status = CWSYM_ERR_INVALID;
				return 0;
			}
			s.parent = p;
		}
		last[s.depth] = (uint32_t)n;
		for (int d = s.depth + 1; d < MAX_DEPTH; ++d) {
			last[d] = CWSYM_NO_PARENT;
		}
		b->sites[n++] = s;
	}
	return n;
}

/* }}} */

/* Serialization {{{ */

static void
put_u16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
put_u32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint16_t
get_u16(const uint8_t* p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
get_u32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t
align_up(size_t n) {
	return (n + CWSYM_SECTION_ALIGN - 1) & ~(size_t)(CWSYM_SECTION_ALIGN - 1);
}

/** Section offsets of a file with these counts, all 8-byte aligned. */
typedef struct {
	size_t starts, sizes, names, strings, disp;
	size_t line_starts, line_lens, line_files, line_lines;
	size_t site_starts, site_lens, site_callees, site_files, site_lines, site_parents;
	size_t dstrings;
	size_t total;
} layout_t;

static layout_t
layout(size_t count, size_t strings_len, size_t line_count, size_t site_count, size_t dstrings_len) {
	layout_t l = { 0 };
	size_t at = CWSYM_HDR_SIZE;
	size_t* words[] = {
		&l.starts, &l.sizes, &l.names, NULL, &l.disp,
		&l.line_starts, &l.line_lens, &l.line_files, &l.line_lines,
		&l.site_starts, &l.site_lens, &l.site_callees, &l.site_files, &l.site_lines, &l.site_parents,
	};
	size_t counts[] = {
		count, count, count, 0, count,
		line_count, line_count, line_count, line_count,
		site_count, site_count, site_count, site_count, site_count, site_count,
	};
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
		if (words[i] == NULL) {
			l.strings = at;
			at = align_up(at + strings_len);
			continue;
		}
		if (counts[i] == 0) {
			continue;
		}
		*words[i] = at;
		at = align_up(at + counts[i] * 4);
	}
	l.dstrings = at;
	l.total = at + dstrings_len;
	return l;
}

static void
put_words(uint8_t* buf, size_t off, const void* rows, size_t count, size_t stride, size_t field) {
	for (size_t i = 0; i < count; ++i) {
		put_u32(buf + off + i * 4, *(const uint32_t*)((const char*)rows + i * stride + field));
	}
}

static uint8_t*
serialize(
	const cwsym_table_builder_t* b, size_t count, const char* strings, size_t line_count,
	size_t site_count, const char* dstrings, size_t* out_len
) {
	layout_t l = layout(count, b->strings.len, line_count, site_count, b->dstrings.len);
	uint8_t* buf = calloc(1, l.total);
	if (buf == NULL) {
		return NULL;
	}
	bool has_lines = line_count > 0 || site_count > 0;

	memcpy(buf + CWSYM_HDR_MAGIC, "CWSYM\0", 6);
	put_u16(buf + CWSYM_HDR_VERSION, CWSYM_VERSION);
	put_u16(buf + CWSYM_HDR_RULES, CWSYM_RULES);
	put_u16(buf + CWSYM_HDR_ARCH, (uint16_t)b->module.arch);
	memcpy(buf + CWSYM_HDR_BUILD_ID, b->module.build_id, CWSYM_BUILD_ID_CAP);
	buf[CWSYM_HDR_BUILD_ID_LEN] = b->module.build_id_len;
	put_u32(buf + CWSYM_HDR_COUNT, (uint32_t)count);
	put_u32(buf + CWSYM_HDR_FLAGS, has_lines ? CWSYM_FLAG_HAS_LINES : 0);
	put_u32(buf + CWSYM_HDR_OFF_STARTS, (uint32_t)l.starts);
	put_u32(buf + CWSYM_HDR_OFF_SIZES, (uint32_t)l.sizes);
	put_u32(buf + CWSYM_HDR_OFF_NAMES, (uint32_t)l.names);
	put_u32(buf + CWSYM_HDR_OFF_STRINGS, (uint32_t)l.strings);
	put_u32(buf + CWSYM_HDR_LEN_STRINGS, b->strings.len);
	put_u32(buf + CWSYM_HDR_OFF_DISP, (uint32_t)l.disp);
	put_u32(buf + CWSYM_HDR_LINE_COUNT, (uint32_t)line_count);
	put_u32(buf + CWSYM_HDR_OFF_LINE_STARTS, (uint32_t)l.line_starts);
	put_u32(buf + CWSYM_HDR_OFF_LINE_LENS, (uint32_t)l.line_lens);
	put_u32(buf + CWSYM_HDR_OFF_LINE_FILES, (uint32_t)l.line_files);
	put_u32(buf + CWSYM_HDR_OFF_LINE_LINES, (uint32_t)l.line_lines);
	put_u32(buf + CWSYM_HDR_SITE_COUNT, (uint32_t)site_count);
	put_u32(buf + CWSYM_HDR_OFF_SITE_STARTS, (uint32_t)l.site_starts);
	put_u32(buf + CWSYM_HDR_OFF_SITE_LENS, (uint32_t)l.site_lens);
	put_u32(buf + CWSYM_HDR_OFF_SITE_CALLEES, (uint32_t)l.site_callees);
	put_u32(buf + CWSYM_HDR_OFF_SITE_FILES, (uint32_t)l.site_files);
	put_u32(buf + CWSYM_HDR_OFF_SITE_LINES, (uint32_t)l.site_lines);
	put_u32(buf + CWSYM_HDR_OFF_SITE_PARENTS, (uint32_t)l.site_parents);
	put_u32(buf + CWSYM_HDR_OFF_DSTRINGS, (uint32_t)l.dstrings);
	put_u32(buf + CWSYM_HDR_LEN_DSTRINGS, b->dstrings.len);

	put_words(buf, l.starts, b->rows, count, sizeof(row_t), offsetof(row_t, start));
	put_words(buf, l.sizes, b->rows, count, sizeof(row_t), offsetof(row_t, size));
	put_words(buf, l.names, b->rows, count, sizeof(row_t), offsetof(row_t, name));
	memcpy(buf + l.strings, strings, b->strings.len);
	put_words(buf, l.disp, b->rows, count, sizeof(row_t), offsetof(row_t, disp));
	put_words(buf, l.line_starts, b->lines, line_count, sizeof(line_row_t), offsetof(line_row_t, start));
	put_words(buf, l.line_lens, b->lines, line_count, sizeof(line_row_t), offsetof(line_row_t, len));
	put_words(buf, l.line_files, b->lines, line_count, sizeof(line_row_t), offsetof(line_row_t, file));
	put_words(buf, l.line_lines, b->lines, line_count, sizeof(line_row_t), offsetof(line_row_t, line));
	put_words(buf, l.site_starts, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, start));
	put_words(buf, l.site_lens, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, len));
	put_words(buf, l.site_callees, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, callee));
	put_words(buf, l.site_files, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, call_file));
	put_words(buf, l.site_lines, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, call_line));
	put_words(buf, l.site_parents, b->sites, site_count, sizeof(site_row_t), offsetof(site_row_t, parent));
	memcpy(buf + l.dstrings, dstrings, b->dstrings.len);

	*out_len = l.total;
	return buf;
}

cwsym_status_t
cwsym_table_end(cwsym_table_builder_t* b, cwsym_table_t* table, const cwsym_log_t* log) {
	*table = (cwsym_table_t){ 0 };
	cwsym_status_t status = b->error;
	char* strings = NULL;
	char* dstrings = NULL;
	uint8_t* buf = NULL;
	if (status != CWSYM_OK) {
		goto done;
	}
	if (!b->has_module) {
		cwsym_logf(log, "the table has no module identity");
		status = CWSYM_ERR_INVALID;
		goto done;
	}
	if (barray_len(b->rows) == 0) {
		cwsym_logf(log, "the table has no functions");
		status = CWSYM_ERR_INVALID;
		goto done;
	}
	size_t count = finish_rows(b, log, &status);
	size_t line_count = status == CWSYM_OK ? finish_lines(b, log, &status) : 0;
	size_t site_count = status == CWSYM_OK ? finish_sites(b, count, log, &status) : 0;
	if (status != CWSYM_OK) {
		goto done;
	}

	/* Offsets by first use, in the order the sections appear in the file. */
	for (size_t i = 0; i < count; ++i) {
		assign(&b->strings, &b->rows[i].name);
	}
	for (size_t i = 0; i < count; ++i) {
		assign(&b->dstrings, &b->rows[i].disp);
	}
	for (size_t i = 0; i < line_count; ++i) {
		assign(&b->dstrings, &b->lines[i].file);
	}
	for (size_t i = 0; i < site_count; ++i) {
		assign(&b->dstrings, &b->sites[i].callee);
	}
	for (size_t i = 0; i < site_count; ++i) {
		assign(&b->dstrings, &b->sites[i].call_file);
	}
	strings = strings_blob(&b->strings);
	dstrings = strings_blob(&b->dstrings);
	if (strings == NULL || dstrings == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}

	size_t len;
	buf = serialize(b, count, strings, line_count, site_count, dstrings, &len);
	if (buf == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}
	status = cwsym_table_parse(buf, len, table, log);
	if (status != CWSYM_OK) {
		cwsym_logf(log, "the writer produced a table its own parser rejects");
		*table = (cwsym_table_t){ 0 };
		goto done;
	}
	table->owned = true;
	buf = NULL;

done:
	free(buf);
	free(strings);
	free(dstrings);
	builder_free(b);
	return status;
}

/* }}} */

/* Parsing {{{ */

/** Section bounds in the buffer being parsed. */
typedef struct {
	const uint8_t* buf;
	size_t len;
	const cwsym_log_t* log;
	cwsym_status_t status;
} parser_t;

/** Point `*out` at `count` words at `off`, or fail with the section's name. */
static const uint32_t*
words_at(parser_t* p, size_t off, size_t count, const char* what) {
	if (p->status != CWSYM_OK) {
		return NULL;
	}
	if (count == 0) {
		return NULL;
	}
	if (off % CWSYM_SECTION_ALIGN != 0 || off < CWSYM_HDR_SIZE || count > (p->len - off) / 4) {
		cwsym_logf(p->log, "section %s at %zu (%zu entries) is misplaced or truncated", what, off, count);
		p->status = CWSYM_ERR_FORMAT;
		return NULL;
	}
	return (const uint32_t*)(p->buf + off);
}

/** Point `*out` at a string section, which must end in NUL. */
static const char*
strings_at(parser_t* p, size_t off, size_t len, const char* what) {
	if (p->status != CWSYM_OK) {
		return NULL;
	}
	if (off % CWSYM_SECTION_ALIGN != 0 || off < CWSYM_HDR_SIZE || len == 0 || len > p->len - off) {
		cwsym_logf(p->log, "section %s at %zu (%zu bytes) is misplaced or truncated", what, off, len);
		p->status = CWSYM_ERR_FORMAT;
		return NULL;
	}
	const char* s = (const char*)(p->buf + off);
	if (s[len - 1] != '\0') {
		cwsym_logf(p->log, "section %s does not end in NUL", what);
		p->status = CWSYM_ERR_INVALID;
		return NULL;
	}
	return s;
}

/** Every offset in `refs` lands inside a string section of `len` bytes. */
static void
check_refs(parser_t* p, const uint32_t* refs, size_t count, size_t len, const char* what) {
	if (p->status != CWSYM_OK || refs == NULL) {
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		if (refs[i] >= len) {
			cwsym_logf(p->log, "%s[%zu] = %" PRIu32 " is outside its string section", what, i, refs[i]);
			p->status = CWSYM_ERR_INVALID;
			return;
		}
	}
}

/** Ranges ascend by start and never overlap. */
static void
check_ranges(parser_t* p, const uint32_t* starts, const uint32_t* lens, size_t count, const char* what) {
	if (p->status != CWSYM_OK || starts == NULL) {
		return;
	}
	for (size_t i = 1; i < count; ++i) {
		if (starts[i] <= starts[i - 1] || starts[i - 1] + lens[i - 1] > starts[i]
			|| starts[i - 1] + lens[i - 1] < starts[i - 1]) {
			cwsym_logf(p->log, "%s[%zu] at %#" PRIx32 " is out of order or overlaps its predecessor", what, i, starts[i]);
			p->status = CWSYM_ERR_INVALID;
			return;
		}
	}
}

static bool
host_is_little_endian(void) {
	uint16_t one = 1;
	return *(const uint8_t*)&one == 1;
}

cwsym_status_t
cwsym_table_parse(const void* buf, size_t len, cwsym_table_t* t, const cwsym_log_t* log) {
	*t = (cwsym_table_t){ 0 };
	if (!host_is_little_endian()) {
		cwsym_logf(log, "cwsym tables are read on little-endian hosts only");
		return CWSYM_ERR_UNSUPPORTED;
	}
	if (((uintptr_t)buf & 3) != 0) {
		cwsym_logf(log, "the buffer must be 4-byte aligned");
		return CWSYM_ERR_FORMAT;
	}
	const uint8_t* b = buf;
	if (len < CWSYM_HDR_SIZE || memcmp(b + CWSYM_HDR_MAGIC, "CWSYM\0", 6) != 0) {
		cwsym_logf(log, "not a cwsym table");
		return CWSYM_ERR_FORMAT;
	}
	uint16_t version = get_u16(b + CWSYM_HDR_VERSION);
	if (version != CWSYM_VERSION) {
		cwsym_logf(log, "cwsym layout version %u; this build reads %u", version, CWSYM_VERSION);
		return CWSYM_ERR_FORMAT;
	}
	parser_t p = { .buf = b, .len = len, .log = log };

	t->data = buf;
	t->len = len;
	t->version = version;
	t->rules = get_u16(b + CWSYM_HDR_RULES);
	t->arch = (cwsym_arch_t)get_u16(b + CWSYM_HDR_ARCH);
	memcpy(t->build_id, b + CWSYM_HDR_BUILD_ID, CWSYM_BUILD_ID_CAP);
	t->build_id_len = b[CWSYM_HDR_BUILD_ID_LEN];
	t->count = get_u32(b + CWSYM_HDR_COUNT);
	t->flags = get_u32(b + CWSYM_HDR_FLAGS);
	if (t->arch < CWSYM_ARCH_X86_64 || t->arch > CWSYM_ARCH_WASM32 || t->build_id_len > CWSYM_BUILD_ID_CAP
		|| t->build_id_len == 0 || t->count == 0) {
		cwsym_logf(log, "bad header: arch %d, build id length %u, %" PRIu32 " functions", t->arch, t->build_id_len, t->count);
		return CWSYM_ERR_INVALID;
	}

	size_t off_strings = get_u32(b + CWSYM_HDR_OFF_STRINGS);
	t->strings_len = get_u32(b + CWSYM_HDR_LEN_STRINGS);
	t->starts = words_at(&p, get_u32(b + CWSYM_HDR_OFF_STARTS), t->count, "starts");
	t->sizes = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SIZES), t->count, "sizes");
	t->names = words_at(&p, get_u32(b + CWSYM_HDR_OFF_NAMES), t->count, "names");
	t->strings = strings_at(&p, off_strings, t->strings_len, "strings");
	check_ranges(&p, t->starts, t->sizes, t->count, "starts");
	check_refs(&p, t->names, t->count, t->strings_len, "names");
	if (p.status != CWSYM_OK) {
		return p.status;
	}

	/* Everything after `strings` is optional: the Worker reads only up to here. */
	size_t off_dstrings = get_u32(b + CWSYM_HDR_OFF_DSTRINGS);
	size_t len_dstrings = get_u32(b + CWSYM_HDR_LEN_DSTRINGS);
	if (len < off_dstrings + len_dstrings || off_dstrings + len_dstrings < off_dstrings) {
		return CWSYM_OK;
	}
	t->dstrings_len = (uint32_t)len_dstrings;
	t->dstrings = strings_at(&p, off_dstrings, len_dstrings, "dstrings");
	t->disp = words_at(&p, get_u32(b + CWSYM_HDR_OFF_DISP), t->count, "disp");
	check_refs(&p, t->disp, t->count, len_dstrings, "disp");
	if (t->flags & CWSYM_FLAG_HAS_LINES) {
		t->line_count = get_u32(b + CWSYM_HDR_LINE_COUNT);
		t->site_count = get_u32(b + CWSYM_HDR_SITE_COUNT);
		t->line_starts = words_at(&p, get_u32(b + CWSYM_HDR_OFF_LINE_STARTS), t->line_count, "line_starts");
		t->line_lens = words_at(&p, get_u32(b + CWSYM_HDR_OFF_LINE_LENS), t->line_count, "line_lens");
		t->line_files = words_at(&p, get_u32(b + CWSYM_HDR_OFF_LINE_FILES), t->line_count, "line_files");
		t->line_lines = words_at(&p, get_u32(b + CWSYM_HDR_OFF_LINE_LINES), t->line_count, "line_lines");
		t->site_starts = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_STARTS), t->site_count, "site_starts");
		t->site_lens = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_LENS), t->site_count, "site_lens");
		t->site_callees = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_CALLEES), t->site_count, "site_callees");
		t->site_files = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_FILES), t->site_count, "site_files");
		t->site_lines = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_LINES), t->site_count, "site_lines");
		t->site_parents = words_at(&p, get_u32(b + CWSYM_HDR_OFF_SITE_PARENTS), t->site_count, "site_parents");
		check_ranges(&p, t->line_starts, t->line_lens, t->line_count, "line_starts");
		check_refs(&p, t->line_files, t->line_count, len_dstrings, "line_files");
		check_refs(&p, t->site_callees, t->site_count, len_dstrings, "site_callees");
		check_refs(&p, t->site_files, t->site_count, len_dstrings, "site_files");
		for (size_t i = 0; p.status == CWSYM_OK && i < t->site_count; ++i) {
			uint32_t start = t->site_starts[i];
			uint32_t end = start + t->site_lens[i];
			uint32_t parent = t->site_parents[i];
			cwsym_hit_t fn;
			bool ok = end > start && cwsym_lookup(t, start, &fn) && end <= fn.start + fn.size
				&& (i == 0 || t->site_starts[i - 1] <= start);
			if (ok && parent != CWSYM_NO_PARENT) {
				ok = parent < i && t->site_starts[parent] <= start
					&& end <= t->site_starts[parent] + t->site_lens[parent];
			}
			if (!ok) {
				cwsym_logf(log, "site[%zu] at %#" PRIx32 " is out of order, outside its function, or outside its parent", i, start);
				p.status = CWSYM_ERR_INVALID;
			}
		}
	}
	if (p.status != CWSYM_OK) {
		*t = (cwsym_table_t){ 0 };
	}
	return p.status;
}

void
cwsym_table_free(cwsym_table_t* t) {
	if (t->owned) {
		free((void*)t->data);
	}
	*t = (cwsym_table_t){ 0 };
}

/* }}} */

/* Searches {{{ */

/** Index of the last entry of `starts` not above `offset`, or -1. */
static ptrdiff_t
last_at(const uint32_t* starts, size_t count, uint32_t offset) {
	size_t lo = 0;
	size_t hi = count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (starts[mid] <= offset) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return (ptrdiff_t)lo - 1;
}

bool
cwsym_lookup(const cwsym_table_t* t, uint32_t offset, cwsym_hit_t* hit) {
	ptrdiff_t i = last_at(t->starts, t->count, offset);
	if (i < 0 || offset - t->starts[i] >= t->sizes[i]) {
		return false;
	}
	*hit = (cwsym_hit_t){
		.start = t->starts[i],
		.size = t->sizes[i],
		.name = t->strings + t->names[i],
	};
	return true;
}

/** Display name of function row `i`: its own, or the normalized name. */
static const char*
display_of(const cwsym_table_t* t, size_t i) {
	if (t->disp != NULL && t->disp[i] != 0) {
		return t->dstrings + t->disp[i];
	}
	return t->strings + t->names[i];
}

int
cwsym_symbolize(const cwsym_table_t* t, uint32_t offset, const cwsym_location_sink_t* sink) {
	ptrdiff_t fn = last_at(t->starts, t->count, offset);
	if (fn < 0 || offset - t->starts[fn] >= t->sizes[fn]) {
		cwsym_location_t miss = { .offset = offset };
		sink->location(sink->user, &miss);
		return 0;
	}
	const char* file = NULL;
	uint32_t line = 0;
	if (t->line_starts != NULL) {
		ptrdiff_t l = last_at(t->line_starts, t->line_count, offset);
		if (l >= 0 && offset - t->line_starts[l] < t->line_lens[l]) {
			file = t->dstrings + t->line_files[l];
			line = t->line_lines[l];
		}
	}

	/*
	 * The innermost site covering the offset is the last covering one in
	 * (start, depth) order. Walk back from the last site starting at or
	 * before the offset, but not past the function's own start.
	 */
	ptrdiff_t site = -1;
	if (t->site_starts != NULL) {
		for (ptrdiff_t s = last_at(t->site_starts, t->site_count, offset); s >= 0; --s) {
			if (t->site_starts[s] < t->starts[fn]) {
				break;
			}
			if (offset - t->site_starts[s] < t->site_lens[s]) {
				site = s;
				break;
			}
		}
	}

	int depth = 0;
	cwsym_location_t loc = {
		.offset = offset,
		.depth = depth,
		.function = site >= 0 ? t->dstrings + t->site_callees[site] : display_of(t, (size_t)fn),
		.file = file,
		.line = line,
	};
	sink->location(sink->user, &loc);
	++depth;
	while (site >= 0) {
		uint32_t parent = t->site_parents[site];
		loc = (cwsym_location_t){
			.offset = offset,
			.depth = depth,
			.function = parent != CWSYM_NO_PARENT ? t->dstrings + t->site_callees[parent] : display_of(t, (size_t)fn),
			.file = t->site_files[site] != 0 ? t->dstrings + t->site_files[site] : NULL,
			.line = t->site_lines[site],
		};
		sink->location(sink->user, &loc);
		++depth;
		site = parent != CWSYM_NO_PARENT ? (ptrdiff_t)parent : -1;
	}
	return depth;
}

/* }}} */

void
cwsym_build_id_hex(const uint8_t* build_id, uint8_t len, char* out) {
	static const char digits[] = "0123456789abcdef";
	for (uint8_t i = 0; i < len && i < CWSYM_BUILD_ID_CAP; ++i) {
		out[2 * i] = digits[build_id[i] >> 4];
		out[2 * i + 1] = digits[build_id[i] & 0xf];
	}
	out[2 * (len < CWSYM_BUILD_ID_CAP ? len : CWSYM_BUILD_ID_CAP)] = '\0';
}

void
cwsym_dump(const cwsym_table_t* t, FILE* out) {
	char id[2 * CWSYM_BUILD_ID_CAP + 1];
	cwsym_build_id_hex(t->build_id, t->build_id_len, id);
	fprintf(out, "%s\n", id);
	fprintf(out, "version %u rules %u arch %d flags %#" PRIx32 "\n", t->version, t->rules, t->arch, t->flags);
	fprintf(out, "functions %" PRIu32 "\n", t->count);
	for (size_t i = 0; i < t->count; ++i) {
		fprintf(out, "0x%08" PRIx32 " 0x%08" PRIx32 " %s", t->starts[i], t->sizes[i], t->strings + t->names[i]);
		if (t->disp != NULL && t->disp[i] != 0) {
			fprintf(out, " | %s", t->dstrings + t->disp[i]);
		}
		fputc('\n', out);
	}
	fprintf(out, "lines %" PRIu32 "\n", t->line_count);
	for (size_t i = 0; i < t->line_count; ++i) {
		fprintf(
			out, "0x%08" PRIx32 " 0x%08" PRIx32 " %s:%" PRIu32 "\n",
			t->line_starts[i], t->line_lens[i], t->dstrings + t->line_files[i], t->line_lines[i]
		);
	}
	fprintf(out, "sites %" PRIu32 "\n", t->site_count);
	for (size_t i = 0; i < t->site_count; ++i) {
		fprintf(out, "0x%08" PRIx32 " 0x%08" PRIx32 " ", t->site_starts[i], t->site_lens[i]);
		if (t->site_parents[i] == CWSYM_NO_PARENT) {
			fputs("- ", out);
		} else {
			fprintf(out, "%" PRIu32 " ", t->site_parents[i]);
		}
		fprintf(out, "%s @ %s:%" PRIu32 "\n", t->dstrings + t->site_callees[i], t->dstrings + t->site_files[i], t->site_lines[i]);
	}
}

#define BARRAY_IMPLEMENTATION
#define BHASH_IMPLEMENTATION
#include "vendor/barray.h"
#include "vendor/bhash.h"
