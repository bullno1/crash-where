/**
 * @file normalize.c
 * The naming rules: raw names in, the `names` column out, and one
 * ELF-shaped and one PDB-shaped input per function must agree.
 */
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "sym.h"

static btest_suite_t normalize = {
	.name = "normalize",
};

#define MAX_SCOPE 6

typedef struct {
	const char* scope[MAX_SCOPE];
	const char* unit;
	bool is_static;
	const char* want;
} case_t;

static bool
run(const case_t* c, char* buf, size_t cap) {
	const char* scope[MAX_SCOPE];
	int n = 0;
	while (n < MAX_SCOPE && c->scope[n] != NULL) {
		scope[n] = c->scope[n];
		++n;
	}
	cwsym_symbol_t sym = {
		.scope = scope,
		.scope_len = n,
		.unit = c->unit,
		.is_static = c->is_static,
	};
	return cwsym_normalize(&sym, buf, cap);
}

static void
check(const case_t* c) {
	char buf[256];
	bool ok = run(c, buf, sizeof(buf));
	BTEST_EXPECT_EX(ok && strcmp(buf, c->want) == 0, "%s -> %s, want %s", c->scope[0], buf, c->want);
}

BTEST(normalize, rules) {
	static const case_t cases[] = {
		/* Plain C. */
		{ .scope = { "render_frame" }, .want = "render_frame" },
		{ .scope = { "init" }, .unit = "../src/render.c", .is_static = true, .want = "render:init" },
		{ .scope = { "init" }, .unit = "C:\\build\\render.c.obj", .is_static = true, .want = "render:init" },
		{ .scope = { "init" }, .unit = "render.obj", .is_static = true, .want = "render:init" },
		{ .scope = { "init" }, .unit = NULL, .is_static = true, .want = "init" },
		{ .scope = { "init" }, .unit = "", .is_static = true, .want = "init" },
		/* Scopes and templates. */
		{ .scope = { "ns", "Mesh", "draw" }, .want = "ns::Mesh::draw" },
		{ .scope = { "std", "vector<int, std::allocator<int> >", "push_back" }, .want = "std::vector::push_back" },
		{ .scope = { "std", "map<int, std::vector<int>>", "find" }, .want = "std::map::find" },
		{ .scope = { "ns", "twice<int>" }, .want = "ns::twice" },
		{ .scope = { "Foo", "~Foo" }, .want = "Foo::~Foo" },
		{ .scope = { "::Foo::bar" }, .want = "Foo::bar" },
		/* Anonymous namespaces. */
		{ .scope = { "ns", "", "hidden" }, .unit = "a.cpp", .is_static = true, .want = "a:ns::$anon::hidden" },
		{ .scope = { "ns", "_GLOBAL__N_1", "hidden" }, .want = "ns::$anon::hidden" },
		{ .scope = { "ns::(anonymous namespace)::hidden" }, .want = "ns::$anon::hidden" },
		/* Lambdas. */
		{ .scope = { "Game", "tick", "<lambda(int)>", "operator()" }, .want = "Game::tick::$lambda::operator()" },
		{ .scope = { "Game", "tick", "", "operator()" }, .want = "Game::tick::$lambda::operator()" },
		{ .scope = { "Game", "tick", "$_0", "operator()" }, .want = "Game::tick::$lambda::operator()" },
		{ .scope = { "Game", "tick", "", "helper" }, .want = "Game::tick::$anon::helper" },
		/* Keywords and tags. */
		{ .scope = { "class Foo::bar" }, .want = "Foo::bar" },
		{ .scope = { "Foo", "bar[abi:cxx11]" }, .want = "Foo::bar" },
		{ .scope = { "std", "__cxx11", "basic_string<char>", "size[abi:cxx11]" }, .want = "std::__cxx11::basic_string::size" },
		/* Operators. */
		{ .scope = { "Foo", "operator<<" }, .want = "Foo::operator<<" },
		{ .scope = { "Foo", "operator<" }, .want = "Foo::operator<" },
		{ .scope = { "Foo", "operator<=>" }, .want = "Foo::operator<=>" },
		{ .scope = { "Foo", "operator->" }, .want = "Foo::operator->" },
		{ .scope = { "Foo", "operator()<int>" }, .want = "Foo::operator()" },
		{ .scope = { "Foo", "operator ns::T<int>" }, .want = "Foo::operator ns::T" },
		{ .scope = { "Foo::operator ns::T<int>" }, .want = "Foo::operator ns::T" },
		{ .scope = { "Foo", "operator new[]" }, .want = "Foo::operator new[]" },
		{ .scope = { "operators", "run" }, .want = "operators::run" },
		/* A mangled fallback stays whole. */
		{ .scope = { "_ZN2ns3fooEv" }, .is_static = true, .unit = "x.c", .want = "x:_ZN2ns3fooEv" },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		check(&cases[i]);
	}
}

/** One function, as DWARF hands it and as DIA hands it. */
typedef struct {
	case_t elf;
	case_t pdb;
} pair_t;

BTEST(normalize, readers_agree) {
	static const pair_t pairs[] = {
		{
			.elf = { .scope = { "ns", "Mesh", "draw" } },
			.pdb = { .scope = { "ns::Mesh::draw" } },
		},
		{
			.elf = { .scope = { "std", "vector<int, std::allocator<int> >", "push_back" } },
			.pdb = { .scope = { "std::vector<int,class std::allocator<int> >::push_back" } },
		},
		{
			.elf = { .scope = { "ns", "", "hidden" }, .unit = "../src/a.cpp", .is_static = true },
			.pdb = { .scope = { "ns::`anonymous namespace'::hidden" }, .unit = "C:\\out\\a.cpp.obj", .is_static = true },
		},
		{
			.elf = { .scope = { "Game", "tick", "<lambda(int)>", "operator()" } },
			.pdb = { .scope = { "Game::tick::<lambda_1>::operator()" } },
		},
		{
			.elf = { .scope = { "Game", "tick", "", "operator()" } },
			.pdb = { .scope = { "Game::tick::<lambda_8f3a2b>::operator()" } },
		},
		{
			.elf = { .scope = { "Foo", "operator<<" } },
			.pdb = { .scope = { "Foo::operator<<" } },
		},
		{
			.elf = { .scope = { "Foo", "operator ns::T<int>" } },
			.pdb = { .scope = { "Foo::operator struct ns::T<int>" } },
		},
		{
			.elf = { .scope = { "init" }, .unit = "../src/render.c", .is_static = true },
			.pdb = { .scope = { "init" }, .unit = "C:\\build\\render.c.obj", .is_static = true },
		},
	};
	for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
		char a[256];
		char b[256];
		BTEST_EXPECT(run(&pairs[i].elf, a, sizeof(a)));
		BTEST_EXPECT(run(&pairs[i].pdb, b, sizeof(b)));
		BTEST_EXPECT_EX(strcmp(a, b) == 0, "ELF %s, PDB %s", a, b);
	}
}

BTEST(normalize, truncates) {
	case_t c = { .scope = { "ns", "Mesh", "draw" }, .want = "ns::Mesh::draw" };
	char buf[8];
	BTEST_EXPECT(!run(&c, buf, sizeof(buf)));
	BTEST_EXPECT_EQUAL("%zu", strlen(buf), sizeof(buf) - 1);
	BTEST_EXPECT(!run(&c, buf, 0));
	char one[1];
	BTEST_EXPECT(!run(&c, one, sizeof(one)));
	BTEST_EXPECT_EQUAL("%d", one[0], 0);
}
