/**
 * @file sym.c
 * The symbol tool's ELF reader against this executable: functions whose
 * addresses the test knows, with their scope, unit, lines, and an
 * inlined call.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "cxx_fixtures.h"
#include "platform.h"
#include "sym.h"

/* The same gates as the readers': these tests exist exactly when one was compiled in. */
#if defined(__has_include)
#if defined(_WIN32)
#if __has_include(<dia2.h>)
#define TEST_HAVE_CWSYM_READER 1
#endif
#elif __has_include(<elfutils/libdw.h>)
#define TEST_HAVE_CWSYM_READER 1
#endif
#endif

#ifdef TEST_HAVE_CWSYM_READER

#if defined(_MSC_VER)
#define KEEP __declspec(noinline)
#define ALWAYS_INLINE __forceinline
#define PLATFORM_UNIT "windows.c"
#else
#define KEEP __attribute__((noinline))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define PLATFORM_UNIT "linux.c"
#endif

/** This executable's path; empty when it cannot be found, which the read then reports. */
static const char*
self_path(void) {
	static char path[1024];
	return test_self_path(path, sizeof(path)) ? path : "";
}

static btest_suite_t sym = {
	.name = "sym",
};

/* Fixtures {{{ */

/*
 * Reached through volatile pointers so the optimizer keeps them as
 * out-of-line functions with their own ranges.
 */
enum { PROBE_LINE = __LINE__ + 2 };
static KEEP int
sym_probe(int x) {
	return x * 3 + 1;
}

static volatile int sym_side_effect;

/* The store keeps the inlined body as instructions of its own instead of folding into the caller. */
static ALWAYS_INLINE int
sym_inlined(int x) {
	sym_side_effect = x;
	return x + 7;
}

enum { CALL_LINE = __LINE__ + 3 };
static KEEP int
sym_inline_caller(int x) {
	return sym_inlined(x) * 2;
}

static int (*volatile keep_probe)(int) = sym_probe;
static int (*volatile keep_caller)(int) = sym_inline_caller;

/* }}} */

/** What the sink saw about one function of interest. */
typedef struct {
	uint32_t want;            /**< Module offset of the function's first byte. */
	bool found;
	char name[128];
	char unit[256];
	uint32_t size;
	int scope_len;
	bool is_static;
} probe_t;

typedef struct {
	int begins;
	int ends;
	cwsym_status_t end_status;
	cwsym_module_t mod;
	size_t symbols;
	size_t lines;
	size_t sites;
	probe_t probe;            /**< sym_probe */
	probe_t caller;           /**< sym_inline_caller */
	probe_t external;         /**< test_image_base, in linux.c */
	bool line_found;          /**< A line row covers sym_probe's first byte. */
	char line_file[256];
	uint32_t line_no;
	bool site_found;          /**< A depth-0 site inside sym_inline_caller. */
	char site_callee[128];
	char site_file[256];
	uint32_t site_line;
} capture_t;

static void
copy_str(char* dst, size_t cap, const char* src) {
	snprintf(dst, cap, "%s", src != NULL ? src : "");
}

static bool
ends_with(const char* s, const char* suffix) {
	size_t n = strlen(s);
	size_t m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void
on_begin(void* user, const cwsym_module_t* mod) {
	capture_t* c = user;
	++c->begins;
	c->mod = *mod;
}

static void
match_probe(probe_t* p, const cwsym_symbol_t* s) {
	if (p->found || s->start != p->want) {
		return;
	}
	p->found = true;
	copy_str(p->name, sizeof(p->name), s->scope[s->scope_len - 1]);
	copy_str(p->unit, sizeof(p->unit), s->unit);
	p->size = s->size;
	p->scope_len = s->scope_len;
	p->is_static = s->is_static;
}

static bool
on_symbol(void* user, const cwsym_symbol_t* s) {
	capture_t* c = user;
	++c->symbols;
	match_probe(&c->probe, s);
	match_probe(&c->caller, s);
	match_probe(&c->external, s);
	return true;
}

static bool
on_line(void* user, const cwsym_line_t* l) {
	capture_t* c = user;
	++c->lines;
	if (!c->line_found && l->start <= c->probe.want && c->probe.want < l->start + l->len) {
		c->line_found = true;
		copy_str(c->line_file, sizeof(c->line_file), l->file);
		c->line_no = l->line;
	}
	return true;
}

static bool
on_site(void* user, const cwsym_site_t* s) {
	capture_t* c = user;
	++c->sites;
	/* A function's symbol precedes its sites, so the caller's range is known here. */
	if (!c->site_found && s->depth == 0 && c->caller.found
		&& s->start >= c->caller.want && s->start < c->caller.want + c->caller.size) {
		c->site_found = true;
		copy_str(c->site_callee, sizeof(c->site_callee), s->callee);
		copy_str(c->site_file, sizeof(c->site_file), s->call_file);
		c->site_line = s->call_line;
	}
	return true;
}

static void
on_end(void* user, cwsym_status_t status) {
	capture_t* c = user;
	++c->ends;
	c->end_status = status;
}

static void
on_log(void* user, const char* msg) {
	(void)user;
	BLOG_INFO("cwsym: %s", msg);
}

static uint32_t
offset_of(void (*fn)(void)) {
	return (uint32_t)((uintptr_t)fn - test_image_base());
}

BTEST(sym, reads_this_executable) {
	capture_t c = {
		.probe = { .want = offset_of((void (*)(void))keep_probe) },
		.caller = { .want = offset_of((void (*)(void))keep_caller) },
		.external = { .want = offset_of((void (*)(void))test_image_base) },
	};
	cwsym_sink_t sink = {
		.begin = on_begin,
		.symbol = on_symbol,
		.line = on_line,
		.site = on_site,
		.end = on_end,
		.user = &c,
	};
	cwsym_log_t log = { .log = on_log };

	cwsym_status_t status = cwsym_read(self_path(), NULL, &sink, &log);
	BTEST_ASSERT_EQUAL("%d", status, CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", c.begins, 1);
	BTEST_EXPECT_EQUAL("%d", c.ends, 1);
	BTEST_EXPECT_EQUAL("%d", c.end_status, CWSYM_OK);
#if defined(__x86_64__)
	BTEST_EXPECT_EQUAL("%d", c.mod.arch, CWSYM_ARCH_X86_64);
#elif defined(__aarch64__)
	BTEST_EXPECT_EQUAL("%d", c.mod.arch, CWSYM_ARCH_AARCH64);
#endif
	BTEST_EXPECT_EQUAL("%u", c.mod.build_id_len, 20u);
	BTEST_EXPECT_RELATION("%zu", c.symbols, >, 100u);
	BTEST_EXPECT_RELATION("%zu", c.lines, >, 1000u);

	BTEST_ASSERT_EX(c.probe.found, "no function starts at %#x", c.probe.want);
	BTEST_EXPECT_EX(strcmp(c.probe.name, "sym_probe") == 0, "name is %s", c.probe.name);
	BTEST_EXPECT_EX(ends_with(c.probe.unit, "sym.c"), "unit is %s", c.probe.unit);
	BTEST_EXPECT(c.probe.is_static);
	BTEST_EXPECT_EQUAL("%d", c.probe.scope_len, 1);
	BTEST_EXPECT_RELATION("%u", c.probe.size, >, 0u);

	BTEST_ASSERT_EX(c.external.found, "no function starts at %#x", c.external.want);
	BTEST_EXPECT_EX(strcmp(c.external.name, "test_image_base") == 0, "name is %s", c.external.name);
	BTEST_EXPECT_EX(ends_with(c.external.unit, PLATFORM_UNIT), "unit is %s", c.external.unit);
	BTEST_EXPECT(!c.external.is_static);

	BTEST_EXPECT_EX(c.line_found, "no line row covers %#x", c.probe.want);
	if (c.line_found) {
		BTEST_EXPECT_EX(ends_with(c.line_file, "sym.c"), "file is %s", c.line_file);
		BTEST_EXPECT_EX(
			c.line_no >= PROBE_LINE && c.line_no <= PROBE_LINE + 3,
			"line is %u, want %d..%d", c.line_no, PROBE_LINE, PROBE_LINE + 3
		);
	}

	BTEST_ASSERT_EX(c.caller.found, "no function starts at %#x", c.caller.want);
	BTEST_EXPECT_EX(c.site_found, "no inline site inside sym_inline_caller at %#x", c.caller.want);
	if (c.site_found) {
		BTEST_EXPECT_EX(strcmp(c.site_callee, "sym_inlined") == 0, "callee is %s", c.site_callee);
		BTEST_EXPECT_EX(ends_with(c.site_file, "sym.c"), "call file is %s", c.site_file);
		BTEST_EXPECT_EQUAL("%u", c.site_line, (unsigned)CALL_LINE);
	}
}

static bool
on_symbol_refuse(void* user, const cwsym_symbol_t* s) {
	on_symbol(user, s);
	return false;
}

BTEST(sym, stops_when_asked) {
	capture_t c = { 0 };
	cwsym_sink_t sink = { .begin = on_begin, .symbol = on_symbol_refuse, .end = on_end, .user = &c };
	cwsym_log_t log = { .log = on_log };

	BTEST_EXPECT_EQUAL("%d", cwsym_read(self_path(), NULL, &sink, &log), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%zu", c.symbols, (size_t)1);
	BTEST_EXPECT_EQUAL("%d", c.begins, 1);
	BTEST_EXPECT_EQUAL("%d", c.ends, 1);
	BTEST_EXPECT_EQUAL("%d", c.end_status, CWSYM_OK);
}

BTEST(sym, rejects_foreign_files) {
	cwsym_log_t log = { .log = on_log };
	cwsym_sink_t sink = { .symbol = on_symbol, .user = &(capture_t){ 0 } };

	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/does-not-exist", NULL, &sink, &log), CWSYM_ERR_IO);

	BTEST_ASSERT(test_mkdir("work"));
	FILE* f = fopen("work/not-a-debug-file.txt", "wb");
	BTEST_ASSERT(f != NULL);
	fputs("plain text\n", f);
	fclose(f);
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/not-a-debug-file.txt", NULL, &sink, &log), CWSYM_ERR_FORMAT);
}

/* C++ names {{{ */

#define MAX_COMPONENTS 6

/** What the sink saw about one C++ fixture. */
typedef struct {
	uint32_t want;            /**< Module offset to match, or 0 to match by normalized name. */
	const char* want_name;    /**< Expected normalized name; the match key when `want` is 0. */
	bool found;
	char name[256];           /**< Normalized name. */
	char scope[MAX_COMPONENTS][64]; /**< Raw components, for the shapes the normalizer rewrites. */
	int scope_len;
	char display[256];
	bool has_display;
	char unit[256];
	bool is_static;
} cxx_probe_t;

typedef struct {
	cxx_probe_t static_method;
	cxx_probe_t anon_fn;
	cxx_probe_t template_fn;
	cxx_probe_t file_static;
	cxx_probe_t method;      /**< `ns::Foo::bar`, no address C can take. */
	cxx_probe_t lambda;      /**< The static host's lambda `operator()`, likewise. */
	cxx_probe_t lambda_ext;  /**< The external host's lambda `operator()`. */
	char lambdas[1024];      /**< Every normalized name mentioning a lambda, for the failure message. */
} cxx_capture_t;

static void
match_cxx(cxx_probe_t* p, const cwsym_symbol_t* s, const char* name) {
	bool hit = p->want != 0 ? s->start == p->want : strcmp(name, p->want_name) == 0;
	if (p->found || !hit) {
		return;
	}
	p->found = true;
	copy_str(p->name, sizeof(p->name), name);
	p->scope_len = s->scope_len < MAX_COMPONENTS ? s->scope_len : MAX_COMPONENTS;
	for (int i = 0; i < p->scope_len; ++i) {
		copy_str(p->scope[i], sizeof(p->scope[i]), s->scope[i]);
	}
	p->has_display = s->display != NULL;
	copy_str(p->display, sizeof(p->display), s->display);
	copy_str(p->unit, sizeof(p->unit), s->unit);
	p->is_static = s->is_static;
}

static bool
on_cxx_symbol(void* user, const cwsym_symbol_t* s) {
	cxx_capture_t* c = user;
	char name[512];
	cwsym_normalize(s, name, sizeof(name));
	match_cxx(&c->static_method, s, name);
	match_cxx(&c->anon_fn, s, name);
	match_cxx(&c->template_fn, s, name);
	match_cxx(&c->file_static, s, name);
	match_cxx(&c->method, s, name);
	match_cxx(&c->lambda, s, name);
	match_cxx(&c->lambda_ext, s, name);
	if (strstr(name, "lambda") != NULL) {
		size_t len = strlen(c->lambdas);
		snprintf(c->lambdas + len, sizeof(c->lambdas) - len, " [%s%s]", name, s->is_static ? " static" : "");
	}
	return true;
}

/**
 * The normalized name, linkage, and unit must match exactly. Display
 * names differ between compilers and GCC omits them for internal
 * linkage, so they are checked only for a fragment and only when present.
 */
static void
expect_cxx(const cxx_probe_t* p, bool is_static, const char* display_prefix, const char* seen) {
	BTEST_ASSERT_EX(p->found, "no symbol for %s; seen:%s", p->want_name, seen);
	BTEST_EXPECT_EX(strcmp(p->name, p->want_name) == 0, "%s: normalized as %s", p->want_name, p->name);
	BTEST_EXPECT_EX(p->is_static == is_static, "%s: is_static is %d", p->want_name, p->is_static);
	BTEST_EXPECT_EX(ends_with(p->unit, "cxx_fixtures.cpp"), "%s: unit is %s", p->want_name, p->unit);
	if (p->has_display) {
		BTEST_EXPECT_EX(strstr(p->display, display_prefix) != NULL, "%s: display is %s", p->want_name, p->display);
	} else {
		BTEST_EXPECT_EX(is_static, "%s: an external function has no display name", p->want_name);
	}
}

#if !defined(_MSC_VER)
/**
 * The raw components from DWARF, for a shape the normalizer rewrites and
 * would otherwise hide: the anonymous namespace and the lambda's class
 * must arrive as empty components. DIA hands over one flat name, so
 * there is nothing raw to check on Windows.
 */
static void
expect_components(const cxx_probe_t* p, const char* const* scope, int scope_len) {
	BTEST_EXPECT_EX(p->scope_len == scope_len, "%s: %d raw components", p->want_name, p->scope_len);
	for (int i = 0; i < scope_len && i < p->scope_len; ++i) {
		BTEST_EXPECT_EX(strcmp(p->scope[i], scope[i]) == 0, "%s: raw component %d is \"%s\"", p->want_name, i, p->scope[i]);
	}
}
#endif

BTEST(sym, reads_cxx_scopes) {
	test_cxx_fixtures_t fx;
	test_cxx_fixtures(&fx);
	uintptr_t base = test_image_base();
	cxx_capture_t c = {
		.static_method = { .want = (uint32_t)(fx.static_method - base), .want_name = "ns::Foo::baz" },
		.anon_fn = { .want = (uint32_t)(fx.anon_fn - base), .want_name = "cxx_fixtures:ns::$anon::hidden" },
		.template_fn = { .want = (uint32_t)(fx.template_fn - base), .want_name = "ns::twice" },
		.file_static = { .want = (uint32_t)(fx.file_static - base), .want_name = "cxx_fixtures:file_static" },
		.method = { .want_name = "ns::Foo::bar" },
		/*
		 * MSVC names a closure type by a hash at global scope and records
		 * no enclosing function, whatever the host's linkage, so the host
		 * is lost there; GCC and Clang nest it. The CRT's delay-load
		 * helpers show MSVC's nested spelling, which normalizes the same
		 * way as DWARF's.
		 */
#if defined(_MSC_VER)
		.lambda = { .want_name = "cxx_fixtures:$lambda::operator()" },
		.lambda_ext = { .want_name = "cxx_fixtures:$lambda::operator()" },
#else
		.lambda = { .want_name = "cxx_fixtures:lambda_host::$lambda::operator()" },
		.lambda_ext = { .want_name = "cxx_fixtures:lambda_host_ext::$lambda::operator()" },
#endif
	};
	cwsym_sink_t sink = { .symbol = on_cxx_symbol, .user = &c };
	cwsym_log_t log = { .log = on_log };
	BTEST_ASSERT_EQUAL("%d", cwsym_read(self_path(), NULL, &sink, &log), CWSYM_OK);

	expect_cxx(&c.static_method, false, "ns::Foo::baz(", c.lambdas);
	expect_cxx(&c.anon_fn, true, "hidden(", c.lambdas);
	expect_cxx(&c.template_fn, false, "ns::twice<int>(", c.lambdas);
	expect_cxx(&c.file_static, true, "file_static(", c.lambdas);
	expect_cxx(&c.method, false, "ns::Foo::bar(", c.lambdas);
	expect_cxx(&c.lambda, true, "operator()", c.lambdas);
	expect_cxx(&c.lambda_ext, true, "operator()", c.lambdas);

#if !defined(_MSC_VER)
	/* The two shapes where DWARF's empty component carries meaning. */
	expect_components(&c.anon_fn, (const char* const[]){ "ns", "", "hidden" }, 3);
	expect_components(&c.lambda, (const char* const[]){ "lambda_host", "", "operator()" }, 3);
#endif
}

/* }}} */

/* Reader into table {{{ */

typedef struct {
	int count;
	char file[256];
	uint32_t line;
	char function[256];
} first_location_t;

static void
on_first_location(void* user, const cwsym_location_t* loc) {
	first_location_t* f = user;
	if (f->count++ == 0) {
		copy_str(f->file, sizeof(f->file), loc->file);
		copy_str(f->function, sizeof(f->function), loc->function);
		f->line = loc->line;
	}
}

/** The whole pipeline on this executable: read, build, look up, symbolize. */
BTEST(sym, builds_table_from_this_executable) {
	cwsym_log_t log = { .log = on_log };
	cwsym_table_builder_t* b = cwsym_table_begin();
	BTEST_ASSERT(b != NULL);
	cwsym_sink_t sink = cwsym_table_sink(b, true);
	BTEST_ASSERT_EQUAL("%d", cwsym_read(self_path(), NULL, &sink, &log), CWSYM_OK);
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_end(b, &t, &log), CWSYM_OK);
	BTEST_EXPECT_RELATION("%u", t.count, >, 100u);
	BTEST_EXPECT_RELATION("%u", t.line_count, >, 1000u);
	BTEST_EXPECT_RELATION("%u", t.site_count, >, 0u);

	uint32_t probe = offset_of((void (*)(void))keep_probe);
	cwsym_hit_t hit;
	BTEST_ASSERT_EX(cwsym_lookup(&t, probe, &hit), "no function at %#x", probe);
	BTEST_EXPECT_EX(strcmp(hit.name, "sym:sym_probe") == 0, "name is %s", hit.name);
	BTEST_EXPECT_EQUAL("%u", hit.start, probe);

	first_location_t first = { 0 };
	cwsym_location_sink_t locations = { .location = on_first_location, .user = &first };
	BTEST_EXPECT_RELATION("%d", cwsym_symbolize(&t, probe, &locations), >=, 1);
	BTEST_EXPECT_EX(ends_with(first.file, "sym.c"), "file is %s", first.file);
	BTEST_EXPECT_EX(
		first.line >= PROBE_LINE && first.line <= PROBE_LINE + 3,
		"line is %u, want %d..%d", first.line, PROBE_LINE, PROBE_LINE + 3
	);

	test_cxx_fixtures_t fx;
	test_cxx_fixtures(&fx);
	uint32_t method = (uint32_t)(fx.static_method - test_image_base());
	BTEST_ASSERT(cwsym_lookup(&t, method, &hit));
	BTEST_EXPECT_EX(strcmp(hit.name, "ns::Foo::baz") == 0, "name is %s", hit.name);

	char id[41];
	cwsym_build_id_hex(t.build_id, t.build_id_len, id);
	BTEST_EXPECT_EQUAL("%zu", strlen(id), (size_t)40);
	cwsym_table_free(&t);
}

/* }}} */

#endif /* TEST_HAVE_CWSYM_READER */
