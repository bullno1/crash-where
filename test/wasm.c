/**
 * @file wasm.c
 * The symbol tool's Wasm reader against a module assembled here, whose
 * every body offset and name is known: the name section, the symbol
 * map, the build id, each rejection, and the pipeline through lookup.
 * Runs on every platform, the web included, since it reads no
 * executable.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "platform.h"
#include "sym.h"

static btest_suite_t wasm = {
	.name = "wasm",
};

/* Assembler {{{ */

/** A growing byte buffer. */
typedef struct {
	uint8_t* data;
	size_t len;
	size_t cap;
} buf_t;

static void
put(buf_t* b, const void* p, size_t n) {
	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2;
		b->data = realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static void
put_u8(buf_t* b, uint8_t v) {
	put(b, &v, 1);
}

static void
put_leb(buf_t* b, uint64_t v) {
	do {
		uint8_t byte = v & 0x7f;
		v >>= 7;
		put_u8(b, byte | (v != 0 ? 0x80 : 0));
	} while (v != 0);
}

/** A length-prefixed byte vector. */
static void
put_vector(buf_t* b, const void* p, size_t n) {
	put_leb(b, n);
	put(b, p, n);
}

static void
put_name(buf_t* b, const char* s) {
	put_vector(b, s, strlen(s));
}

/** Append a section and return the file offset of its payload. */
static size_t
put_section(buf_t* module, uint8_t id, const buf_t* payload) {
	put_u8(module, id);
	put_leb(module, payload->len);
	size_t at = module->len;
	put(module, payload->data, payload->len);
	return at;
}

#define BODY_COUNT 3
#define IMPORTED_FUNCTIONS 1

/** What to assemble and what came out of it. */
typedef struct {
	bool names;                   /**< Write a name section. */
	size_t build_id_len;          /**< 0 leaves the section out. */
	uint32_t starts[BODY_COUNT];  /**< File offset of each body. */
	uint32_t sizes[BODY_COUNT];
} module_t;

static const uint8_t build_id[21] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
	0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
};

/** Names in the function index space; the import comes first and the last body stays unnamed. */
static const char* const function_names[IMPORTED_FUNCTIONS + BODY_COUNT] = {
	"imp", "main", "ns::Foo::bar(int volatile*)", NULL,
};

/**
 * A module with one function import among imports of every other kind,
 * three bodies of different shapes, and a body long enough for a
 * two-byte size prefix.
 */
static void
assemble(buf_t* out, module_t* m) {
	put(out, "\0asm\x01\0\0\0", 8);

	buf_t types = { 0 };
	put_leb(&types, 1);
	put(&types, "\x60\x00\x00", 3);
	put_section(out, 1, &types);
	free(types.data);

	buf_t imports = { 0 };
	put_leb(&imports, 4);
	put_name(&imports, "env");
	put_name(&imports, "imp");
	put_u8(&imports, 0);
	put_leb(&imports, 0);
	put_name(&imports, "env");
	put_name(&imports, "memory");
	put_u8(&imports, 2);
	put(&imports, "\x01\x01\x02", 3);
	put_name(&imports, "env");
	put_name(&imports, "table");
	put_u8(&imports, 1);
	put(&imports, "\x70\x00\x00", 3);
	put_name(&imports, "env");
	put_name(&imports, "global");
	put_u8(&imports, 3);
	put(&imports, "\x7f\x00", 2);
	put_section(out, 2, &imports);
	free(imports.data);

	buf_t functions = { 0 };
	put_leb(&functions, BODY_COUNT);
	for (int i = 0; i < BODY_COUNT; ++i) {
		put_leb(&functions, 0);
	}
	put_section(out, 3, &functions);
	free(functions.data);

	buf_t code = { 0 };
	size_t rel[BODY_COUNT];
	put_leb(&code, BODY_COUNT);
	/* No locals, end. */
	static const uint8_t body_a[] = { 0x00, 0x0b };
	/* Two i32 locals, i32.const 0, drop, end. */
	static const uint8_t body_b[] = { 0x01, 0x02, 0x7f, 0x41, 0x00, 0x1a, 0x0b };
	/* No locals, 130 nops, end. */
	uint8_t body_c[132];
	memset(body_c, 0x01, sizeof(body_c));
	body_c[0] = 0x00;
	body_c[sizeof(body_c) - 1] = 0x0b;
	const uint8_t* bodies[BODY_COUNT] = { body_a, body_b, body_c };
	size_t sizes[BODY_COUNT] = { sizeof(body_a), sizeof(body_b), sizeof(body_c) };
	for (int i = 0; i < BODY_COUNT; ++i) {
		put_leb(&code, sizes[i]);
		rel[i] = code.len;
		put(&code, bodies[i], sizes[i]);
	}
	size_t payload = put_section(out, 10, &code);
	for (int i = 0; i < BODY_COUNT; ++i) {
		m->starts[i] = (uint32_t)(payload + rel[i]);
		m->sizes[i] = (uint32_t)sizes[i];
	}
	free(code.data);

	if (m->names) {
		buf_t names = { 0 };
		put_name(&names, "name");
		/* The module name subsection, skipped. */
		put_u8(&names, 0);
		put_leb(&names, 1 + strlen("m"));
		put_name(&names, "m");
		buf_t funcs = { 0 };
		put_leb(&funcs, 3);
		for (uint32_t i = 0; i < IMPORTED_FUNCTIONS + BODY_COUNT; ++i) {
			if (function_names[i] != NULL) {
				put_leb(&funcs, i);
				put_name(&funcs, function_names[i]);
			}
		}
		put_u8(&names, 1);
		put_vector(&names, funcs.data, funcs.len);
		free(funcs.data);
		/* An empty locals subsection, skipped. */
		put(&names, "\x02\x01\x00", 3);
		put_section(out, 0, &names);
		free(names.data);
	}

	if (m->build_id_len > 0) {
		buf_t id = { 0 };
		put_name(&id, "build_id");
		put_vector(&id, build_id, m->build_id_len);
		put_section(out, 0, &id);
		free(id.data);
	}
}

static bool
write_file(const char* path, const void* data, size_t len) {
	FILE* f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

/** Assemble `m` into `path`. */
static bool
write_module(const char* path, module_t* m, size_t truncate_to) {
	buf_t out = { 0 };
	assemble(&out, m);
	bool ok = test_mkdir("work") && test_mkdir("work/wasm")
		&& write_file(path, out.data, truncate_to != 0 && truncate_to < out.len ? truncate_to : out.len);
	free(out.data);
	return ok;
}

/* }}} */

/* Sink {{{ */

typedef struct {
	uint32_t start;
	uint32_t size;
	char name[64];
	char display[64];
	int scope_len;
	bool is_static;
	bool has_unit;
} seen_t;

typedef struct {
	int begins;
	int ends;
	cwsym_status_t end_status;
	cwsym_module_t mod;
	int count;
	seen_t seen[BODY_COUNT + 1];
	bool refuse;  /**< Stop the reader at the first symbol. */
} capture_t;

static void
on_begin(void* user, const cwsym_module_t* mod) {
	capture_t* c = user;
	++c->begins;
	c->mod = *mod;
}

static bool
on_symbol(void* user, const cwsym_symbol_t* s) {
	capture_t* c = user;
	if (c->count < BODY_COUNT + 1) {
		seen_t* seen = &c->seen[c->count];
		*seen = (seen_t){
			.start = s->start,
			.size = s->size,
			.scope_len = s->scope_len,
			.is_static = s->is_static,
			.has_unit = s->unit != NULL,
		};
		snprintf(seen->name, sizeof(seen->name), "%s", s->scope[s->scope_len - 1]);
		snprintf(seen->display, sizeof(seen->display), "%s", s->display != NULL ? s->display : "");
	}
	++c->count;
	return !c->refuse;
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

static const cwsym_log_t logger = { .log = on_log };

static cwsym_sink_t
sink_of(capture_t* c) {
	return (cwsym_sink_t){ .begin = on_begin, .symbol = on_symbol, .end = on_end, .user = c };
}

/** Every body as the assembler placed it, named as the name source says. */
static void
check_bodies(const capture_t* c, const module_t* m) {
	BTEST_ASSERT_EQUAL("%d", c->count, BODY_COUNT);
	for (int i = 0; i < BODY_COUNT; ++i) {
		const seen_t* s = &c->seen[i];
		BTEST_EXPECT_EQUAL("%u", s->start, m->starts[i]);
		BTEST_EXPECT_EQUAL("%u", s->size, m->sizes[i]);
		BTEST_EXPECT_EQUAL("%d", s->scope_len, 1);
		BTEST_EXPECT(!s->is_static);
		BTEST_EXPECT(!s->has_unit);
	}
	BTEST_EXPECT_EX(strcmp(c->seen[0].name, "main") == 0, "name is %s", c->seen[0].name);
	BTEST_EXPECT_EX(c->seen[0].display[0] == '\0', "display is %s", c->seen[0].display);
	BTEST_EXPECT_EX(strcmp(c->seen[1].name, "ns::Foo::bar(int volatile*)") == 0, "name is %s", c->seen[1].name);
	BTEST_EXPECT_EX(strcmp(c->seen[1].display, c->seen[1].name) == 0, "display is %s", c->seen[1].display);
	BTEST_EXPECT_EX(strcmp(c->seen[2].name, "wasm-function[3]") == 0, "name is %s", c->seen[2].name);
	BTEST_EXPECT_EX(c->seen[2].display[0] == '\0', "display is %s", c->seen[2].display);
}

/* }}} */

BTEST(wasm, reads_name_section) {
	module_t m = { .names = true, .build_id_len = 20 };
	BTEST_ASSERT(write_module("work/wasm/named.wasm", &m, 0));
	/* The body after a two-byte size prefix sits past the 128-byte mark of the section. */
	BTEST_EXPECT_RELATION("%u", m.sizes[2], >=, 128u);

	capture_t c = { 0 };
	cwsym_sink_t sink = sink_of(&c);
	BTEST_ASSERT_EQUAL("%d", cwsym_read("work/wasm/named.wasm", NULL, &sink, &logger), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", c.begins, 1);
	BTEST_EXPECT_EQUAL("%d", c.ends, 1);
	BTEST_EXPECT_EQUAL("%d", c.end_status, CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", c.mod.arch, CWSYM_ARCH_WASM32);
	BTEST_EXPECT_EQUAL("%u", c.mod.build_id_len, 20u);
	BTEST_EXPECT(memcmp(c.mod.build_id, build_id, 20) == 0);
	check_bodies(&c, &m);
}

BTEST(wasm, reads_symbol_map) {
	module_t m = { .names = false, .build_id_len = 20 };
	BTEST_ASSERT(write_module("work/wasm/stripped.wasm", &m, 0));

	capture_t c = { 0 };
	cwsym_sink_t sink = sink_of(&c);
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/stripped.wasm", NULL, &sink, &logger), CWSYM_ERR_NO_DEBUG);
	BTEST_EXPECT_EQUAL("%d", c.begins, 0);

	cwsym_read_options_t opts = { .symbol_map = "work/wasm/missing.symbols" };
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/stripped.wasm", &opts, &sink, &logger), CWSYM_ERR_IO);
	BTEST_EXPECT_EQUAL("%d", c.begins, 0);

	/* As Emscripten writes it, with a blank line and a CRLF for good measure. */
	static const char map[] = "0:imp\n1:main\r\n\n2:ns::Foo::bar(int volatile*)\nnot a mapping\n9:beyond\n";
	BTEST_ASSERT(write_file("work/wasm/stripped.symbols", map, sizeof(map) - 1));
	opts.symbol_map = "work/wasm/stripped.symbols";
	BTEST_ASSERT_EQUAL("%d", cwsym_read("work/wasm/stripped.wasm", &opts, &sink, &logger), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", c.begins, 1);
	BTEST_EXPECT_EQUAL("%d", c.ends, 1);
	check_bodies(&c, &m);
}

BTEST(wasm, rejects_bad_modules) {
	capture_t c = { 0 };
	cwsym_sink_t sink = sink_of(&c);

	module_t m = { .names = true, .build_id_len = 0 };
	BTEST_ASSERT(write_module("work/wasm/no-id.wasm", &m, 0));
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/no-id.wasm", NULL, &sink, &logger), CWSYM_ERR_NO_BUILD_ID);

	m = (module_t){ .names = true, .build_id_len = 21 };
	BTEST_ASSERT(write_module("work/wasm/long-id.wasm", &m, 0));
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/long-id.wasm", NULL, &sink, &logger), CWSYM_ERR_NO_BUILD_ID);

	/* Cut inside the last body. */
	m = (module_t){ .names = true, .build_id_len = 20 };
	BTEST_ASSERT(write_module("work/wasm/named.wasm", &m, 0));
	BTEST_ASSERT(write_module("work/wasm/short.wasm", &m, m.starts[2] + 10));
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/short.wasm", NULL, &sink, &logger), CWSYM_ERR_FORMAT);

	BTEST_ASSERT(write_file("work/wasm/v2.wasm", "\0asm\x02\0\0\0", 8));
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/v2.wasm", NULL, &sink, &logger), CWSYM_ERR_FORMAT);

	BTEST_EXPECT_EQUAL("%d", c.begins, 0);
	BTEST_EXPECT_EQUAL("%d", c.ends, 0);
}

BTEST(wasm, stops_when_asked) {
	module_t m = { .names = true, .build_id_len = 20 };
	BTEST_ASSERT(write_module("work/wasm/named.wasm", &m, 0));

	capture_t c = { .refuse = true };
	cwsym_sink_t sink = sink_of(&c);
	BTEST_EXPECT_EQUAL("%d", cwsym_read("work/wasm/named.wasm", NULL, &sink, &logger), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%d", c.count, 1);
	BTEST_EXPECT_EQUAL("%d", c.ends, 1);
	BTEST_EXPECT_EQUAL("%d", c.end_status, CWSYM_OK);
}

typedef struct {
	int count;
	char function[64];
	bool has_file;
} first_location_t;

static void
on_first_location(void* user, const cwsym_location_t* loc) {
	first_location_t* f = user;
	if (f->count++ == 0) {
		snprintf(f->function, sizeof(f->function), "%s", loc->function != NULL ? loc->function : "");
		f->has_file = loc->file != NULL;
	}
}

/** The whole pipeline: read, build, look up, symbolize. */
BTEST(wasm, builds_table) {
	module_t m = { .names = true, .build_id_len = 20 };
	BTEST_ASSERT(write_module("work/wasm/named.wasm", &m, 0));

	cwsym_table_builder_t* b = cwsym_table_begin();
	BTEST_ASSERT(b != NULL);
	cwsym_sink_t sink = cwsym_table_sink(b, true);
	BTEST_ASSERT_EQUAL("%d", cwsym_read("work/wasm/named.wasm", NULL, &sink, &logger), CWSYM_OK);
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_end(b, &t, &logger), CWSYM_OK);
	BTEST_EXPECT_EQUAL("%u", t.count, (unsigned)BODY_COUNT);
	BTEST_EXPECT_EQUAL("%u", t.line_count, 0u);
	BTEST_EXPECT_EQUAL("%d", t.arch, CWSYM_ARCH_WASM32);

	char id[41];
	cwsym_build_id_hex(t.build_id, t.build_id_len, id);
	BTEST_EXPECT_EX(strcmp(id, "101112131415161718191a1b1c1d1e1f20212223") == 0, "id is %s", id);

	/* An offset inside a body, as an engine prints one, names its function and no other. */
	cwsym_hit_t hit;
	BTEST_ASSERT(cwsym_lookup(&t, m.starts[1] + 3, &hit));
	BTEST_EXPECT_EX(strcmp(hit.name, "ns::Foo::bar") == 0, "name is %s", hit.name);
	BTEST_EXPECT_EQUAL("%u", hit.start, m.starts[1]);
	BTEST_EXPECT_EQUAL("%u", hit.size, m.sizes[1]);
	BTEST_ASSERT(cwsym_lookup(&t, m.starts[2] + 100, &hit));
	BTEST_EXPECT_EX(strcmp(hit.name, "wasm-function[3]") == 0, "name is %s", hit.name);
	/* The size prefix before the first body belongs to no function. */
	BTEST_EXPECT(!cwsym_lookup(&t, m.starts[0] - 1, &hit));
	BTEST_EXPECT(!cwsym_lookup(&t, m.starts[2] + m.sizes[2], &hit));

	first_location_t first = { 0 };
	cwsym_location_sink_t locations = { .location = on_first_location, .user = &first };
	BTEST_EXPECT_EQUAL("%d", cwsym_symbolize(&t, m.starts[1] + 3, &locations), 1);
	BTEST_EXPECT_EX(strcmp(first.function, "ns::Foo::bar(int volatile*)") == 0, "function is %s", first.function);
	BTEST_EXPECT(!first.has_file);
	cwsym_table_free(&t);
}
