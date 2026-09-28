/**
 * @file normalize.c
 * The naming rules of ::CWSYM_RULES: raw scope components in, the
 * `names` column out. Pure; no allocation.
 *
 * Every component is first cut into pieces at top-level `::`, so a flat
 * `ns::Foo::bar` from a PDB and the components `ns`, `Foo`, `bar` from
 * DWARF meet here as the same pieces. Each piece is then rendered by the
 * rules below, in order, and the pieces are joined with `::`.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "sym.h"

#define MAX_PIECES 64

/** One scope piece, pointing into the caller's strings. */
typedef struct {
	const char* s;
	size_t len;
	bool unnamed;  /**< A whole component that was `""`: an unnamed namespace or class. */
} piece_t;

/** Output cursor with truncation tracking. */
typedef struct {
	char* buf;
	size_t cap;
	size_t pos;
	bool truncated;
} out_t;

static void
put(out_t* o, const char* s, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		if (o->pos + 1 < o->cap) {
			o->buf[o->pos++] = s[i];
		} else {
			o->truncated = true;
		}
	}
}

static void
put_str(out_t* o, const char* s) {
	put(o, s, strlen(s));
}

static bool
equals(piece_t p, const char* s) {
	return p.len == strlen(s) && memcmp(p.s, s, p.len) == 0;
}

static bool
starts_with(const char* s, size_t len, const char* prefix) {
	size_t n = strlen(prefix);
	return len >= n && memcmp(s, prefix, n) == 0;
}

static bool
is_ident_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/** `operator` followed by something that is not more identifier: an operator function's name. */
static bool
is_operator(const char* s, size_t len) {
	return starts_with(s, len, "operator") && (len == 8 || !is_ident_char(s[8]));
}

/**
 * Cut one component at top-level `::`. From an `operator` piece on, the
 * rest is a single piece: an operator name is always the last one and
 * a conversion operator's type may itself be qualified. A leading `::`
 * (the global namespace) yields nothing.
 */
static int
split(const char* comp, piece_t* out, int n) {
	size_t len = strlen(comp);
	if (len == 0) {
		if (n < MAX_PIECES) {
			out[n++] = (piece_t){ .s = comp, .len = 0, .unnamed = true };
		}
		return n;
	}
	size_t start = 0;
	int depth = 0;
	for (size_t i = 0; i < len;) {
		if (i == start && is_operator(comp + i, len - i)) {
			break;
		}
		char c = comp[i];
		if (c == '<' || c == '(' || c == '[') {
			++depth;
		} else if ((c == '>' || c == ')' || c == ']') && depth > 0) {
			--depth;
		} else if (depth == 0 && c == ':' && i + 1 < len && comp[i + 1] == ':') {
			if (i > start && n < MAX_PIECES) {
				out[n++] = (piece_t){ .s = comp + start, .len = i - start };
			}
			i += 2;
			start = i;
			continue;
		}
		++i;
	}
	if (len > start && n < MAX_PIECES) {
		out[n++] = (piece_t){ .s = comp + start, .len = len - start };
	}
	return n;
}

/** Every spelling of an anonymous namespace the readers or a demangler produce. */
static bool
is_anonymous(piece_t p) {
	return p.unnamed
		|| starts_with(p.s, p.len, "_GLOBAL__N_")
		|| equals(p, "`anonymous namespace'")
		|| equals(p, "(anonymous namespace)")
		|| equals(p, "{anonymous}");
}

/**
 * Every spelling of a lambda's closure type: GCC `<lambda(int)>`, MSVC
 * `<lambda_1>`, Clang `$_0`, demangler forms, and an unnamed class whose
 * only visible member is the `operator()` that follows it.
 */
static bool
is_lambda(piece_t p, bool next_is_call_operator) {
	return starts_with(p.s, p.len, "<lambda")
		|| starts_with(p.s, p.len, "$_")
		|| starts_with(p.s, p.len, "'lambda")
		|| starts_with(p.s, p.len, "{lambda")
		|| (p.unnamed && next_is_call_operator);
}

/** MSVC writes these in front of type names; GCC never does. */
static const char* const keywords[] = { "class ", "struct ", "union ", "enum " };

/** Length of the type keyword at a word start in `s`, or 0. */
static size_t
keyword_at(const char* s, size_t len, bool word_start) {
	if (!word_start) {
		return 0;
	}
	for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); ++k) {
		if (starts_with(s, len, keywords[k])) {
			return strlen(keywords[k]);
		}
	}
	return 0;
}

/**
 * Copy `s` without template argument lists, `[abi:...]` tags, and type
 * keywords. In a type, `<` always opens template arguments.
 */
static void
put_stripped(out_t* o, const char* s, size_t len) {
	int depth = 0;
	for (size_t i = 0; i < len; ++i) {
		char c = s[i];
		size_t kw = depth == 0 ? keyword_at(s + i, len - i, i == 0 || !is_ident_char(s[i - 1])) : 0;
		if (kw != 0) {
			i += kw - 1;
		} else if (c == '<') {
			++depth;
		} else if (c == '>' && depth > 0) {
			--depth;
		} else if (depth == 0 && starts_with(s + i, len - i, "[abi:")) {
			while (i < len && s[i] != ']') {
				++i;
			}
		} else if (depth == 0) {
			put(o, &c, 1);
		}
	}
}

/**
 * The symbol of an operator function, longest spelling first, so
 * `operator<<` is not read as `operator<` followed by template arguments.
 */
static const char* const operator_symbols[] = {
	"<=>", "<<=", ">>=", "->*",
	"()", "[]", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "++", "--",
	"+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", "->",
	"+", "-", "*", "/", "%", "^", "&", "|", "~", "!", "=", "<", ">", ",",
};

/** `operator` and its symbol verbatim, then the rest stripped like a type. */
static void
put_operator(out_t* o, const char* s, size_t len) {
	put(o, s, 8);
	s += 8;
	len -= 8;
	for (size_t k = 0; k < sizeof(operator_symbols) / sizeof(operator_symbols[0]); ++k) {
		const char* sym = operator_symbols[k];
		size_t n = strlen(sym);
		if (starts_with(s, len, sym)) {
			put(o, s, n);
			s += n;
			len -= n;
			break;
		}
	}
	put_stripped(o, s, len);
}

/** Render one piece by the rules, in order. */
static void
put_piece(out_t* o, piece_t p, bool next_is_call_operator) {
	if (is_lambda(p, next_is_call_operator)) {
		put_str(o, "$lambda");
		return;
	}
	if (is_anonymous(p)) {
		put_str(o, "$anon");
		return;
	}
	if (is_operator(p.s, p.len)) {
		put_operator(o, p.s, p.len);
	} else {
		put_stripped(o, p.s, p.len);
	}
}

/**
 * The unit stem: the last path component cut at its first dot, so
 * `../src/render.c` and `C:\build\render.c.obj` both give `render`.
 */
static void
put_unit_stem(out_t* o, const char* unit) {
	const char* base = unit;
	for (const char* p = unit; *p != '\0'; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	size_t len = 0;
	while (base[len] != '\0' && base[len] != '.') {
		++len;
	}
	put(o, base, len);
}

bool
cwsym_normalize(const cwsym_symbol_t* sym, char* buf, size_t cap) {
	if (cap == 0) {
		return false;
	}
	out_t o = { .buf = buf, .cap = cap };

	piece_t pieces[MAX_PIECES];
	int n = 0;
	for (int i = 0; i < sym->scope_len; ++i) {
		n = split(sym->scope[i], pieces, n);
	}

	if (sym->is_static && sym->unit != NULL && sym->unit[0] != '\0') {
		size_t before = o.pos;
		put_unit_stem(&o, sym->unit);
		if (o.pos > before) {
			put(&o, ":", 1);
		}
	}
	for (int i = 0; i < n; ++i) {
		if (i > 0) {
			put(&o, "::", 2);
		}
		bool next_is_call_operator = i + 2 == n && equals(pieces[i + 1], "operator()");
		put_piece(&o, pieces[i], next_is_call_operator);
	}
	buf[o.pos] = '\0';
	return !o.truncated;
}
