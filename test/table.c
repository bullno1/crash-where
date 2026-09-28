/**
 * @file table.c
 * The table builder, writer, parser, and searches against a synthetic
 * table whose every offset and name is known: round trip, determinism,
 * the builder's rules, the edges of both searches, prefix reads, patched
 * files, and the golden fixture the Worker repository reads too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "format.h"
#include "platform.h"
#include "sym.h"

static btest_suite_t table = {
	.name = "table",
};

/* Helpers {{{ */

static char log_text[4096];

static void
on_log(void* user, const char* msg) {
	(void)user;
	BLOG_INFO("cwsym: %s", msg);
	size_t len = strlen(log_text);
	snprintf(log_text + len, sizeof(log_text) - len, "%s\n", msg);
}

static const cwsym_log_t logger = { .log = on_log };

static void
reset_log(void) {
	log_text[0] = '\0';
}

static const cwsym_module_t module = {
	.arch = CWSYM_ARCH_X86_64,
	.build_id = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 },
	.build_id_len = 20,
};

static cwsym_status_t
add(cwsym_table_builder_t* b, uint32_t start, uint32_t size, const char* name, bool is_static, const char* unit, const char* display) {
	const char* scope[] = { name };
	cwsym_symbol_t sym = {
		.start = start, .size = size, .scope = scope, .scope_len = 1,
		.display = display, .unit = unit, .is_static = is_static,
	};
	return cwsym_table_add(b, &sym);
}

static cwsym_status_t
add_line(cwsym_table_builder_t* b, uint32_t start, uint32_t len, const char* file, uint32_t line) {
	return cwsym_table_add_line(b, &(cwsym_line_t){ .start = start, .len = len, .file = file, .line = line });
}

static cwsym_status_t
add_site(cwsym_table_builder_t* b, uint32_t start, uint32_t len, int depth, const char* callee, const char* file, uint32_t line) {
	return cwsym_table_add_site(b, &(cwsym_site_t){
		.start = start, .len = len, .depth = depth, .callee = callee, .call_file = file, .call_line = line,
	});
}

/** One step of the synthetic table; `k` from 0 to `SYNTHETIC_STEPS - 1`, in any order. */
#define SYNTHETIC_STEPS 13

static void
synthetic_step(cwsym_table_builder_t* b, int k) {
	switch (k) {
	case 0: add(b, 0x1000, 0x40, "main", false, "a.c", NULL); break;
	case 1: add(b, 0x1040, 0x20, "helper", true, "../src/a.c", NULL); break;
	case 2: add(b, 0x2000, 0x100, "ns::Foo::bar", false, "foo.cpp", "ns::Foo::bar(int)"); break;
	case 3: add(b, 0x3000, 0x10, "ns::Foo::bar", false, "foo.cpp", "ns::Foo::bar(int)"); break;
	case 4: add(b, 0x4000, 0x8, "dup_a", true, "a.c", NULL); break;
	case 5: add(b, 0x4000, 0x8, "dup_b", false, "a.c", NULL); break;
	case 6: add(b, 0x6000, 0, "empty", false, "a.c", NULL); break;
	case 7: add_line(b, 0x1000, 0x10, "a.c", 10); break;
	case 8: add_line(b, 0x1010, 0x10, "a.c", 11); break;
	case 9: add_line(b, 0x2000, 0x20, "foo.cpp", 5); add_line(b, 0x2040, 0x20, "foo.cpp", 7); break;
	case 10: add_line(b, 0x2020, 0x20, "inl.h", 3); break;
	case 11: add_site(b, 0x2020, 0x20, 0, "inl", "foo.cpp", 6); break;
	case 12: add_site(b, 0x2028, 0x8, 1, "deep", "inl.h", 4); break;
	default: break;
	}
}

/** Build the synthetic table, feeding the steps in `order` when given. */
static cwsym_status_t
build_synthetic(cwsym_table_t* t, const int* order, bool with_lines) {
	cwsym_table_builder_t* b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	for (int i = 0; i < SYNTHETIC_STEPS; ++i) {
		int k = order != NULL ? order[i] : i;
		if (!with_lines && k >= 7) {
			continue;
		}
		synthetic_step(b, k);
	}
	return cwsym_table_end(b, t, &logger);
}

static size_t
offset_in(const cwsym_table_t* t, const void* p) {
	return (size_t)((const char*)p - (const char*)t->data);
}

/* }}} */

BTEST(table, round_trip) {
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, true), CWSYM_OK);
	BTEST_EXPECT(t.owned);
	BTEST_EXPECT_EQUAL("%u", t.version, 1u);
	BTEST_EXPECT_EQUAL("%u", t.rules, (unsigned)CWSYM_RULES);
	BTEST_EXPECT_EQUAL("%d", t.arch, CWSYM_ARCH_X86_64);
	BTEST_EXPECT_EQUAL("%u", t.build_id_len, 20u);
	BTEST_EXPECT_EQUAL("%u", t.flags, (unsigned)CWSYM_FLAG_HAS_LINES);
	BTEST_EXPECT_EQUAL("%u", t.count, 6u);
	BTEST_EXPECT_EQUAL("%u", t.line_count, 5u);
	BTEST_EXPECT_EQUAL("%u", t.site_count, 2u);

	/* Hashed sections first, then strings, then display, lines, dstrings; all aligned. */
	const void* order[] = {
		t.starts, t.sizes, t.names, t.strings, t.disp,
		t.line_starts, t.line_lens, t.line_files, t.line_lines,
		t.site_starts, t.site_lens, t.site_callees, t.site_files, t.site_lines, t.site_parents,
		t.dstrings,
	};
	size_t prev = 0;
	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
		size_t off = offset_in(&t, order[i]);
		BTEST_EXPECT_EX(off % CWSYM_SECTION_ALIGN == 0, "section %zu at %zu is misaligned", i, off);
		BTEST_EXPECT_EX(off > prev, "section %zu at %zu is out of order", i, off);
		prev = off;
	}
	BTEST_EXPECT_EQUAL("%zu", offset_in(&t, t.starts), (size_t)CWSYM_HDR_SIZE);
	BTEST_EXPECT_EQUAL("%zu", t.len, offset_in(&t, t.dstrings) + t.dstrings_len);

	cwsym_table_t back;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_parse(t.data, t.len, &back, &logger), CWSYM_OK);
	BTEST_EXPECT(!back.owned);
	BTEST_EXPECT_EQUAL("%u", back.count, t.count);
	BTEST_EXPECT_EQUAL("%u", back.line_count, t.line_count);
	BTEST_EXPECT_EQUAL("%u", back.site_count, t.site_count);
	BTEST_EXPECT(memcmp(back.build_id, t.build_id, 20) == 0);
	BTEST_EXPECT(memcmp(back.starts, t.starts, t.count * 4) == 0);
	BTEST_EXPECT(memcmp(back.names, t.names, t.count * 4) == 0);
	BTEST_EXPECT(memcmp(back.site_parents, t.site_parents, t.site_count * 4) == 0);
	BTEST_EXPECT(strcmp(back.strings + back.names[0], "main") == 0);
	cwsym_table_free(&back);
	cwsym_table_free(&t);
	BTEST_EXPECT(t.data == NULL);
}

BTEST(table, deterministic) {
	static const int shuffled[SYNTHETIC_STEPS] = { 12, 5, 9, 0, 11, 3, 7, 6, 1, 10, 4, 8, 2 };
	cwsym_table_t a;
	cwsym_table_t b;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&a, NULL, true), CWSYM_OK);
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&b, shuffled, true), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%zu", a.len, b.len);
	BTEST_EXPECT(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
	cwsym_table_free(&a);
	cwsym_table_free(&b);
}

BTEST(table, builder_rules) {
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, true), CWSYM_OK);

	/* A split function shares one name and one display string. */
	BTEST_EXPECT_EQUAL("%u", t.starts[2], 0x2000u);
	BTEST_EXPECT_EQUAL("%u", t.starts[3], 0x3000u);
	BTEST_EXPECT_EQUAL("%u", t.names[2], t.names[3]);
	BTEST_EXPECT_EQUAL("%u", t.disp[2], t.disp[3]);
	BTEST_EXPECT(t.disp[2] != 0);
	BTEST_EXPECT(strcmp(t.strings + t.names[2], "ns::Foo::bar") == 0);
	BTEST_EXPECT(strcmp(t.dstrings + t.disp[2], "ns::Foo::bar(int)") == 0);
	BTEST_EXPECT(strcmp(t.strings + t.names[1], "a:helper") == 0);
	/* Each distinct string once. */
	size_t hits = 0;
	for (size_t i = 0; i + 7 <= t.dstrings_len; ++i) {
		hits += memcmp(t.dstrings + i, "foo.cpp", 7) == 0;
	}
	BTEST_EXPECT_EQUAL("%zu", hits, (size_t)1);
	/* ICF: at one start the external row survives. */
	cwsym_hit_t hit;
	BTEST_ASSERT(cwsym_lookup(&t, 0x4000, &hit));
	BTEST_EXPECT(strcmp(hit.name, "dup_b") == 0);
	/* Sites resolved their parents from depth. */
	BTEST_EXPECT_EQUAL("%u", t.site_parents[0], CWSYM_NO_PARENT);
	BTEST_EXPECT_EQUAL("%u", t.site_parents[1], 0u);
	cwsym_table_free(&t);

	/* Overlapping functions. */
	reset_log();
	cwsym_table_builder_t* b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	add(b, 0x1000, 0x40, "first", false, NULL, NULL);
	add(b, 0x1020, 0x10, "second", false, NULL, NULL);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);
	BTEST_EXPECT_EX(strstr(log_text, "first") != NULL && strstr(log_text, "second") != NULL, "log: %s", log_text);
	BTEST_EXPECT(t.data == NULL);

	/* No functions; no module. */
	b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);
	b = cwsym_table_begin();
	add(b, 0x1000, 0x40, "main", false, NULL, NULL);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);

	/* A site outside every function; a nested site outside its parent. */
	reset_log();
	b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	add(b, 0x1000, 0x10, "main", false, NULL, NULL);
	add_site(b, 0x5000, 4, 0, "stray", NULL, 0);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);
	BTEST_EXPECT_EX(strstr(log_text, "stray") != NULL, "log: %s", log_text);
	b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	add(b, 0x1000, 0x20, "main", false, NULL, NULL);
	add_site(b, 0x1000, 0x8, 0, "outer", NULL, 0);
	add_site(b, 0x1008, 0x4, 1, "inner", NULL, 0);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);

	/* The first failure sticks. */
	b = cwsym_table_begin();
	cwsym_table_set_module(b, &module);
	cwsym_symbol_t bad = { .start = 0x1000, .size = 4, .scope_len = 0 };
	BTEST_EXPECT_EQUAL("%d", cwsym_table_add(b, &bad), CWSYM_ERR_INVALID);
	BTEST_EXPECT_EQUAL("%d", add(b, 0x2000, 0x10, "fine", false, NULL, NULL), CWSYM_ERR_INVALID);
	BTEST_EXPECT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_ERR_INVALID);
}

BTEST(table, lookup_edges) {
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, false), CWSYM_OK);
	static const struct {
		uint32_t offset;
		const char* name; /**< NULL for a miss. */
	} cases[] = {
		{ 0x0fff, NULL },
		{ 0x1000, "main" },
		{ 0x103f, "main" },
		{ 0x1040, "a:helper" },
		{ 0x105f, "a:helper" },
		{ 0x1060, NULL },
		{ 0x2080, "ns::Foo::bar" },
		{ 0x300f, "ns::Foo::bar" },
		{ 0x3010, NULL },
		{ 0x4007, "dup_b" },
		{ 0x6000, NULL },
		{ 0xffffffffu, NULL },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		cwsym_hit_t hit;
		bool found = cwsym_lookup(&t, cases[i].offset, &hit);
		if (cases[i].name == NULL) {
			BTEST_EXPECT_EX(!found, "%#x hit %s", cases[i].offset, found ? hit.name : "");
		} else {
			BTEST_EXPECT_EX(found && strcmp(hit.name, cases[i].name) == 0, "%#x -> %s, want %s", cases[i].offset, found ? hit.name : "(miss)", cases[i].name);
		}
	}
	BTEST_EXPECT(t.disp != NULL);
	BTEST_EXPECT(t.line_starts == NULL && t.line_count == 0);
	cwsym_table_free(&t);
}

/* Symbolize {{{ */

#define MAX_LOCATIONS 8

typedef struct {
	int count;
	struct {
		int depth;
		char function[128];
		char file[128];
		uint32_t line;
		bool has_function;
	} at[MAX_LOCATIONS];
} locations_t;

static void
on_location(void* user, const cwsym_location_t* loc) {
	locations_t* l = user;
	if (l->count >= MAX_LOCATIONS) {
		return;
	}
	l->at[l->count].depth = loc->depth;
	l->at[l->count].has_function = loc->function != NULL;
	snprintf(l->at[l->count].function, 128, "%s", loc->function != NULL ? loc->function : "");
	snprintf(l->at[l->count].file, 128, "%s", loc->file != NULL ? loc->file : "");
	l->at[l->count].line = loc->line;
	++l->count;
}

static int
symbolize(const cwsym_table_t* t, uint32_t offset, locations_t* l) {
	*l = (locations_t){ 0 };
	cwsym_location_sink_t sink = { .location = on_location, .user = l };
	return cwsym_symbolize(t, offset, &sink);
}

static void
expect_location(const locations_t* l, int i, const char* function, const char* file, uint32_t line) {
	BTEST_ASSERT_RELATION("%d", l->count, >, i);
	BTEST_EXPECT_EQUAL("%d", l->at[i].depth, i);
	BTEST_EXPECT_EX(strcmp(l->at[i].function, function) == 0, "depth %d function is %s", i, l->at[i].function);
	BTEST_EXPECT_EX(strcmp(l->at[i].file, file) == 0, "depth %d file is %s", i, l->at[i].file);
	BTEST_EXPECT_EQUAL("%u", l->at[i].line, line);
}

BTEST(table, symbolize_shapes) {
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, true), CWSYM_OK);
	locations_t l;

	/* Inside the nested site: innermost first, each parent's call site, then the function. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x2028, &l), 3);
	expect_location(&l, 0, "deep", "inl.h", 3);
	expect_location(&l, 1, "inl", "inl.h", 4);
	expect_location(&l, 2, "ns::Foo::bar(int)", "foo.cpp", 6);
	/* Inside the outer site only. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x2030, &l), 2);
	expect_location(&l, 0, "inl", "inl.h", 3);
	expect_location(&l, 1, "ns::Foo::bar(int)", "foo.cpp", 6);
	/* A line but no site. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x2040, &l), 1);
	expect_location(&l, 0, "ns::Foo::bar(int)", "foo.cpp", 7);
	/* A function with no line: the display name and no file. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x3004, &l), 1);
	expect_location(&l, 0, "ns::Foo::bar(int)", "", 0);
	/* No display name: the normalized one. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x1044, &l), 1);
	expect_location(&l, 0, "a:helper", "", 0);
	/* A miss: one call with no function, zero delivered. */
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x5000, &l), 0);
	BTEST_EXPECT_EQUAL("%d", l.count, 1);
	BTEST_EXPECT(!l.at[0].has_function);
	cwsym_table_free(&t);

	/* A table without line sections. */
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, false), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", symbolize(&t, 0x2028, &l), 1);
	expect_location(&l, 0, "ns::Foo::bar(int)", "", 0);
	cwsym_table_free(&t);
}

/* }}} */

/* Prefix and corruption {{{ */

static uint32_t
read_u32(const void* data, size_t off) {
	const uint8_t* p = (const uint8_t*)data + off;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
write_u32(void* data, size_t off, uint32_t v) {
	uint8_t* p = (uint8_t*)data + off;
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/** An aligned copy of the table's bytes to patch. */
static void*
copy_of(const cwsym_table_t* t) {
	void* copy = malloc(t->len);
	memcpy(copy, t->data, t->len);
	return copy;
}

static cwsym_status_t
parse_copy(const void* data, size_t len) {
	cwsym_table_t t;
	cwsym_status_t status = cwsym_table_parse(data, len, &t, &logger);
	if (status == CWSYM_OK) {
		cwsym_table_free(&t);
	}
	return status;
}

BTEST(table, prefix_and_corruption) {
	cwsym_table_t full;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&full, NULL, true), CWSYM_OK);
	size_t prefix_len = read_u32(full.data, CWSYM_HDR_OFF_STRINGS) + read_u32(full.data, CWSYM_HDR_LEN_STRINGS);

	/* The Worker's prefix: names resolve, nothing else is there. */
	cwsym_table_t prefix;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_parse(full.data, prefix_len, &prefix, &logger), CWSYM_OK);
	cwsym_hit_t hit;
	BTEST_EXPECT(cwsym_lookup(&prefix, 0x2080, &hit) && strcmp(hit.name, "ns::Foo::bar") == 0);
	BTEST_EXPECT(prefix.disp == NULL && prefix.dstrings == NULL && prefix.line_starts == NULL);
	BTEST_EXPECT_EQUAL("%u", prefix.line_count, 0u);
	locations_t l;
	BTEST_EXPECT_EQUAL("%d", symbolize(&prefix, 0x2028, &l), 1);
	expect_location(&l, 0, "ns::Foo::bar", "", 0);
	cwsym_table_free(&prefix);

	/* Cut inside an array. */
	BTEST_EXPECT_EQUAL("%d", parse_copy(full.data, prefix_len - 4), CWSYM_ERR_FORMAT);
	BTEST_EXPECT_EQUAL("%d", parse_copy(full.data, CWSYM_HDR_SIZE - 1), CWSYM_ERR_FORMAT);

	static const struct {
		const char* what;
		size_t field;         /**< Header offset of the u32 to patch, or the byte for `byte` patches. */
		uint32_t value;
		bool byte;            /**< Patch one byte instead of a u32. */
		bool in_array;        /**< `field` is an index into the array named by `array`. */
		size_t array;
		cwsym_status_t want;
	} patches[] = {
		{ .what = "magic", .field = 0, .value = 'X', .byte = true, .want = CWSYM_ERR_FORMAT },
		{ .what = "version", .field = CWSYM_HDR_VERSION, .value = 2, .byte = true, .want = CWSYM_ERR_FORMAT },
		{ .what = "count zero", .field = CWSYM_HDR_COUNT, .value = 0, .want = CWSYM_ERR_INVALID },
		{ .what = "arch", .field = CWSYM_HDR_ARCH, .value = 9, .byte = true, .want = CWSYM_ERR_INVALID },
		{ .what = "misaligned strings", .field = CWSYM_HDR_OFF_STRINGS, .value = 4, .want = CWSYM_ERR_FORMAT },
		{ .what = "overrun dstrings", .field = CWSYM_HDR_LEN_DSTRINGS, .value = 0x7fffffff, .want = CWSYM_OK },
		{ .what = "starts out of order", .field = 1, .value = 0x0800, .in_array = true, .array = CWSYM_HDR_OFF_STARTS, .want = CWSYM_ERR_INVALID },
		{ .what = "sizes overlap", .field = 0, .value = 0x1000, .in_array = true, .array = CWSYM_HDR_OFF_SIZES, .want = CWSYM_ERR_INVALID },
		{ .what = "name out of section", .field = 0, .value = 0xffff, .in_array = true, .array = CWSYM_HDR_OFF_NAMES, .want = CWSYM_ERR_INVALID },
		{ .what = "file out of section", .field = 0, .value = 0xffff, .in_array = true, .array = CWSYM_HDR_OFF_LINE_FILES, .want = CWSYM_ERR_INVALID },
		{ .what = "site parent out of range", .field = 1, .value = 99, .in_array = true, .array = CWSYM_HDR_OFF_SITE_PARENTS, .want = CWSYM_ERR_INVALID },
		{ .what = "site outside function", .field = 0, .value = 0x5000, .in_array = true, .array = CWSYM_HDR_OFF_SITE_STARTS, .want = CWSYM_ERR_INVALID },
	};
	for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
		void* copy = copy_of(&full);
		size_t at = patches[i].in_array ? read_u32(copy, patches[i].array) + patches[i].field * 4 : patches[i].field;
		if (patches[i].byte) {
			((uint8_t*)copy)[at] = (uint8_t)patches[i].value;
		} else if (patches[i].what[0] == 'm') {
			write_u32(copy, at, read_u32(copy, at) + patches[i].value);
		} else {
			write_u32(copy, at, patches[i].value);
		}
		cwsym_status_t got = parse_copy(copy, full.len);
		/* An overrun of the last section leaves a valid prefix behind. */
		BTEST_EXPECT_EX(got == patches[i].want, "%s: status %d, want %d", patches[i].what, got, patches[i].want);
		free(copy);
	}
	cwsym_table_free(&full);
}

/* }}} */

/* Golden fixture {{{ */

static char*
read_file(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = malloc((size_t)n + 1);
	*len = fread(buf, 1, (size_t)n, f);
	buf[*len] = '\0';
	fclose(f);
	return buf;
}

static bool
write_file(const char* path, const void* data, size_t len) {
	FILE* f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	fclose(f);
	return ok;
}

/**
 * The synthetic table's bytes and dump are committed under test/fixtures
 * and shared with the Worker repository, which parses the same bytes.
 * `CW_TEST_WRITE_FIXTURES=1` rewrites them after an intended layout change.
 */
BTEST(table, golden) {
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", build_synthetic(&t, NULL, true), CWSYM_OK);

	BTEST_ASSERT(test_mkdir("work"));
	FILE* f = fopen("work/synthetic.dump", "wb");
	BTEST_ASSERT(f != NULL);
	cwsym_dump(&t, f);
	fclose(f);
	size_t dump_len;
	char* dump = read_file("work/synthetic.dump", &dump_len);
	BTEST_ASSERT(dump != NULL);

	const char* bin_path = TEST_FIXTURE_DIR "/synthetic.cwsym";
	const char* dump_path = TEST_FIXTURE_DIR "/synthetic.dump";
	if (getenv("CW_TEST_WRITE_FIXTURES") != NULL) {
		BTEST_EXPECT(write_file(bin_path, t.data, t.len));
		BTEST_EXPECT(write_file(dump_path, dump, dump_len));
		BLOG_INFO("fixtures written to %s", TEST_FIXTURE_DIR);
	} else {
		size_t want_len;
		char* want = read_file(bin_path, &want_len);
		BTEST_ASSERT_EX(want != NULL, "missing %s; run with CW_TEST_WRITE_FIXTURES=1 to create it", bin_path);
		BTEST_EXPECT_EX(want_len == t.len && memcmp(want, t.data, t.len) == 0, "%s differs from the built table", bin_path);
		free(want);
		want = read_file(dump_path, &want_len);
		BTEST_ASSERT_EX(want != NULL, "missing %s", dump_path);
		BTEST_EXPECT_EX(want_len == dump_len && memcmp(want, dump, dump_len) == 0, "%s differs:\n%s", dump_path, dump);
		free(want);
	}
	free(dump);
	cwsym_table_free(&t);
}

/* }}} */
