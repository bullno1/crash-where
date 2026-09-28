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

/* The same gate as the reader's: these tests exist exactly when it was compiled in. */
#if defined(__has_include)
#if __has_include(<elfutils/libdw.h>)
#define TEST_HAVE_CWSYM_ELF 1
#endif
#endif

#ifdef TEST_HAVE_CWSYM_ELF

static btest_suite_t sym = {
	.name = "sym",
};

/* Fixtures {{{ */

/*
 * Reached through volatile pointers so the optimizer keeps them as
 * out-of-line functions with their own ranges.
 */
enum { PROBE_LINE = __LINE__ + 2 };
static __attribute__((noinline)) int
sym_probe(int x) {
	return x * 3 + 1;
}

static volatile int sym_side_effect;

/* The store keeps the inlined body as instructions of its own instead of folding into the caller. */
static inline __attribute__((always_inline)) int
sym_inlined(int x) {
	sym_side_effect = x;
	return x + 7;
}

enum { CALL_LINE = __LINE__ + 3 };
static __attribute__((noinline)) int
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

	cwsym_status_t status = cwsym_read("/proc/self/exe", NULL, &sink, &log);
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
	BTEST_EXPECT_EX(ends_with(c.external.unit, "linux.c"), "unit is %s", c.external.unit);
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

	BTEST_EXPECT_EQUAL("%d", cwsym_read("/proc/self/exe", NULL, &sink, &log), CWSYM_OK);
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
	uint32_t want;                        /**< Module offset to match, or 0 to match by scope. */
	const char* want_scope[MAX_COMPONENTS]; /**< Scope to match when `want` is 0. */
	int want_scope_len;
	bool found;
	char scope[MAX_COMPONENTS][64];
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
	cxx_probe_t method;      /**< `ns::Foo::bar`, matched by scope. */
	cxx_probe_t lambda;      /**< `lambda_host::<unnamed>::operator()`, matched by scope. */
} cxx_capture_t;

static bool
scope_matches(const cxx_probe_t* p, const cwsym_symbol_t* s) {
	if (s->scope_len != p->want_scope_len) {
		return false;
	}
	for (int i = 0; i < s->scope_len; ++i) {
		if (strcmp(s->scope[i], p->want_scope[i]) != 0) {
			return false;
		}
	}
	return true;
}

static void
match_cxx(cxx_probe_t* p, const cwsym_symbol_t* s) {
	bool hit = p->want != 0 ? s->start == p->want : scope_matches(p, s);
	if (p->found || !hit || s->scope_len > MAX_COMPONENTS) {
		return;
	}
	p->found = true;
	p->scope_len = s->scope_len;
	for (int i = 0; i < s->scope_len; ++i) {
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
	match_cxx(&c->static_method, s);
	match_cxx(&c->anon_fn, s);
	match_cxx(&c->template_fn, s);
	match_cxx(&c->file_static, s);
	match_cxx(&c->method, s);
	match_cxx(&c->lambda, s);
	return true;
}

static bool
starts_with(const char* s, const char* prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

/** The scope as `a::b::c`, an empty component shown as `<>`, for messages. */
static const char*
scope_text(const cxx_probe_t* p, char* buf, size_t cap) {
	size_t len = 0;
	buf[0] = '\0';
	for (int i = 0; i < p->scope_len && len < cap; ++i) {
		len += (size_t)snprintf(buf + len, cap - len, "%s%s", i > 0 ? "::" : "", p->scope[i][0] ? p->scope[i] : "<>");
	}
	return buf;
}

/**
 * The scope must match exactly and the unit must be the fixture file.
 * Display names differ between compilers and GCC omits them for
 * internal linkage, so they are checked only by prefix and only when
 * present.
 */
static void
expect_cxx(const cxx_probe_t* p, const char* const* scope, int scope_len, bool is_static, const char* display_prefix) {
	char text[512];
	BTEST_ASSERT_EX(p->found, "no symbol for %s", scope[scope_len - 1]);
	BTEST_EXPECT_EX(p->scope_len == scope_len, "%s has %d components", scope_text(p, text, sizeof(text)), p->scope_len);
	for (int i = 0; i < scope_len && i < p->scope_len; ++i) {
		BTEST_EXPECT_EX(strcmp(p->scope[i], scope[i]) == 0, "%s: component %d", scope_text(p, text, sizeof(text)), i);
	}
	BTEST_EXPECT_EX(p->is_static == is_static, "%s: is_static is %d", scope_text(p, text, sizeof(text)), p->is_static);
	BTEST_EXPECT_EX(ends_with(p->unit, "cxx_fixtures.cpp"), "%s: unit is %s", scope_text(p, text, sizeof(text)), p->unit);
	if (p->has_display) {
		BTEST_EXPECT_EX(starts_with(p->display, display_prefix), "%s: display is %s", scope_text(p, text, sizeof(text)), p->display);
	} else {
		BTEST_EXPECT_EX(is_static, "%s: an external function has no display name", scope_text(p, text, sizeof(text)));
	}
}

BTEST(sym, reads_cxx_scopes) {
	test_cxx_fixtures_t fx;
	test_cxx_fixtures(&fx);
	uintptr_t base = test_image_base();
	cxx_capture_t c = {
		.static_method = { .want = (uint32_t)(fx.static_method - base) },
		.anon_fn = { .want = (uint32_t)(fx.anon_fn - base) },
		.template_fn = { .want = (uint32_t)(fx.template_fn - base) },
		.file_static = { .want = (uint32_t)(fx.file_static - base) },
		.method = { .want_scope = { "ns", "Foo", "bar" }, .want_scope_len = 3 },
		.lambda = { .want_scope = { "lambda_host", "", "operator()" }, .want_scope_len = 3 },
	};
	cwsym_sink_t sink = { .symbol = on_cxx_symbol, .user = &c };
	cwsym_log_t log = { .log = on_log };
	BTEST_ASSERT_EQUAL("%d", cwsym_read("/proc/self/exe", NULL, &sink, &log), CWSYM_OK);

	expect_cxx(&c.static_method, (const char* const[]){ "ns", "Foo", "baz" }, 3, false, "ns::Foo::baz(");
	expect_cxx(&c.anon_fn, (const char* const[]){ "ns", "", "hidden" }, 3, true, "ns::(anonymous namespace)::hidden(");
	expect_cxx(&c.template_fn, (const char* const[]){ "ns", "twice<int>" }, 2, false, "int ns::twice<int>(");
	expect_cxx(&c.file_static, (const char* const[]){ "file_static" }, 1, true, "file_static(");
	expect_cxx(&c.method, (const char* const[]){ "ns", "Foo", "bar" }, 3, false, "ns::Foo::bar(");
	expect_cxx(&c.lambda, (const char* const[]){ "lambda_host", "", "operator()" }, 3, true, "lambda_host");
}

/* }}} */

#endif /* TEST_HAVE_CWSYM_ELF */
