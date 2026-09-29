/**
 * @file elf.c
 * ELF reader over libdw and libelf, both loaded at run time.
 *
 * DWARF gives the functions with their ranges and scopes, the line
 * table, and the inline sites. `.symtab` fills in code that has no DWARF
 * entry and never displaces a DWARF range. Every entry point of the two
 * libraries is looked up by name and stored with the type of its
 * declaration, so no signature is retyped here and only the headers are
 * a build dependency.
 */
#include "reader.h"

/*
 * The reader needs libdw's header, which the Steam Runtime SDK does not
 * ship; without it this file is the stub at the end. `__has_include` is
 * C23 and an extension in every compiler this project builds with.
 */
#if defined(__linux__) && defined(__has_include)
#if __has_include(<elfutils/libdw.h>)
#define CWSYM_HAVE_LIBDW 1
#endif
#endif

#ifdef CWSYM_HAVE_LIBDW

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwelf.h>
#include <gelf.h>
#include <libelf.h>

#define BARRAY_API static inline
#define BARRAY_IMPLEMENTATION
#include "vendor/barray.h"

#define MAX_SCOPE 64
#define PAGE_MASK ((uint64_t)0xfff) /**< The kernel maps the first segment down to a page boundary. */

/** The loaded libraries and their entry points; loading is attempted once. */
static struct {
	void* libelf;
	void* libdw;
	bool tried;
	__typeof__(elf_version)* elf_version;
	__typeof__(elf_begin)* elf_begin;
	__typeof__(elf_end)* elf_end;
	__typeof__(elf_kind)* elf_kind;
	__typeof__(elf_errmsg)* elf_errmsg;
	__typeof__(elf_getphdrnum)* elf_getphdrnum;
	__typeof__(elf_getshdrstrndx)* elf_getshdrstrndx;
	__typeof__(elf_nextscn)* elf_nextscn;
	__typeof__(elf_getscn)* elf_getscn;
	__typeof__(elf_getdata)* elf_getdata;
	__typeof__(elf_strptr)* elf_strptr;
	__typeof__(gelf_getehdr)* gelf_getehdr;
	__typeof__(gelf_getphdr)* gelf_getphdr;
	__typeof__(gelf_getshdr)* gelf_getshdr;
	__typeof__(gelf_getsym)* gelf_getsym;
	__typeof__(dwarf_begin_elf)* dwarf_begin_elf;
	__typeof__(dwarf_end)* dwarf_end;
	__typeof__(dwarf_errmsg)* dwarf_errmsg;
	__typeof__(dwarf_nextcu)* dwarf_nextcu;
	__typeof__(dwarf_offdie)* dwarf_offdie;
	__typeof__(dwarf_child)* dwarf_child;
	__typeof__(dwarf_siblingof)* dwarf_siblingof;
	__typeof__(dwarf_tag)* dwarf_tag;
	__typeof__(dwarf_diename)* dwarf_diename;
	__typeof__(dwarf_attr)* dwarf_attr;
	__typeof__(dwarf_attr_integrate)* dwarf_attr_integrate;
	__typeof__(dwarf_hasattr_integrate)* dwarf_hasattr_integrate;
	__typeof__(dwarf_formstring)* dwarf_formstring;
	__typeof__(dwarf_formudata)* dwarf_formudata;
	__typeof__(dwarf_formref_die)* dwarf_formref_die;
	__typeof__(dwarf_ranges)* dwarf_ranges;
	__typeof__(dwarf_getscopes_die)* dwarf_getscopes_die;
	__typeof__(dwarf_getsrclines)* dwarf_getsrclines;
	__typeof__(dwarf_onesrcline)* dwarf_onesrcline;
	__typeof__(dwarf_lineaddr)* dwarf_lineaddr;
	__typeof__(dwarf_lineno)* dwarf_lineno;
	__typeof__(dwarf_linesrc)* dwarf_linesrc;
	__typeof__(dwarf_lineendsequence)* dwarf_lineendsequence;
	__typeof__(dwarf_getsrcfiles)* dwarf_getsrcfiles;
	__typeof__(dwarf_filesrc)* dwarf_filesrc;
	__typeof__(dwelf_elf_gnu_build_id)* dwelf_elf_gnu_build_id;
	__typeof__(dwelf_elf_gnu_debuglink)* dwelf_elf_gnu_debuglink;
} dw;

/** Resolve `NAME` from `LIB` into the member of the same name; false when it is missing. */
#define DW_SYM(LIB, NAME) \
	((dw.NAME = (__typeof__(NAME)*)dlsym(dw.LIB, #NAME)) != NULL)

static bool
load(const cwsym_log_t* log) {
	if (dw.tried) {
		return dw.libdw != NULL;
	}
	dw.tried = true;

	dw.libelf = dlopen("libelf.so.1", RTLD_NOW | RTLD_LOCAL);
	dw.libdw = dw.libelf != NULL ? dlopen("libdw.so.1", RTLD_NOW | RTLD_LOCAL) : NULL;
	if (dw.libdw == NULL) {
		cwsym_logf(log, "libdw not found; install elfutils: %s", dlerror());
		goto fail;
	}

	bool ok = DW_SYM(libelf, elf_version)
		&& DW_SYM(libelf, elf_begin)
		&& DW_SYM(libelf, elf_end)
		&& DW_SYM(libelf, elf_kind)
		&& DW_SYM(libelf, elf_errmsg)
		&& DW_SYM(libelf, elf_getphdrnum)
		&& DW_SYM(libelf, elf_getshdrstrndx)
		&& DW_SYM(libelf, elf_nextscn)
		&& DW_SYM(libelf, elf_getscn)
		&& DW_SYM(libelf, elf_getdata)
		&& DW_SYM(libelf, elf_strptr)
		&& DW_SYM(libelf, gelf_getehdr)
		&& DW_SYM(libelf, gelf_getphdr)
		&& DW_SYM(libelf, gelf_getshdr)
		&& DW_SYM(libelf, gelf_getsym)
		&& DW_SYM(libdw, dwarf_begin_elf)
		&& DW_SYM(libdw, dwarf_end)
		&& DW_SYM(libdw, dwarf_errmsg)
		&& DW_SYM(libdw, dwarf_nextcu)
		&& DW_SYM(libdw, dwarf_offdie)
		&& DW_SYM(libdw, dwarf_child)
		&& DW_SYM(libdw, dwarf_siblingof)
		&& DW_SYM(libdw, dwarf_tag)
		&& DW_SYM(libdw, dwarf_diename)
		&& DW_SYM(libdw, dwarf_attr)
		&& DW_SYM(libdw, dwarf_attr_integrate)
		&& DW_SYM(libdw, dwarf_hasattr_integrate)
		&& DW_SYM(libdw, dwarf_formstring)
		&& DW_SYM(libdw, dwarf_formudata)
		&& DW_SYM(libdw, dwarf_formref_die)
		&& DW_SYM(libdw, dwarf_ranges)
		&& DW_SYM(libdw, dwarf_getscopes_die)
		&& DW_SYM(libdw, dwarf_getsrclines)
		&& DW_SYM(libdw, dwarf_onesrcline)
		&& DW_SYM(libdw, dwarf_lineaddr)
		&& DW_SYM(libdw, dwarf_lineno)
		&& DW_SYM(libdw, dwarf_linesrc)
		&& DW_SYM(libdw, dwarf_lineendsequence)
		&& DW_SYM(libdw, dwarf_getsrcfiles)
		&& DW_SYM(libdw, dwarf_filesrc)
		&& DW_SYM(libdw, dwelf_elf_gnu_build_id)
		&& DW_SYM(libdw, dwelf_elf_gnu_debuglink);
	if (!ok) {
		cwsym_logf(log, "libdw is missing a required symbol: %s", dlerror());
		goto fail;
	}
	dw.elf_version(EV_CURRENT);
	return true;

fail:
	if (dw.libdw != NULL) {
		dlclose(dw.libdw);
	}
	if (dw.libelf != NULL) {
		dlclose(dw.libelf);
	}
	dw.libdw = NULL;
	dw.libelf = NULL;
	return false;
}

/**
 * `__cxa_demangle` of the Itanium C++ ABI. It has C linkage and a
 * signature the ABI document freezes, but `<cxxabi.h>` is a C++ header,
 * so the prototype is declared here rather than taken from one.
 */
typedef char* (*cxa_demangle_fn)(const char* mangled, char* buf, size_t* len, int* status);

/** The C++ runtime's demangler; loading is attempted once. */
static struct {
	void* lib;
	bool tried;
	cxa_demangle_fn demangle;
} cxa;

/**
 * GCC and Clang both mangle per the Itanium ABI, so one demangler reads
 * either compiler's names; only where the function lives differs.
 */
static bool
load_demangler(const cwsym_log_t* log) {
	if (cxa.tried) {
		return cxa.demangle != NULL;
	}
	cxa.tried = true;
	static const char* const names[] = { "libstdc++.so.6", "libc++abi.so.1" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && cxa.lib == NULL; ++i) {
		cxa.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (cxa.lib != NULL) {
		cxa.demangle = (cxa_demangle_fn)dlsym(cxa.lib, "__cxa_demangle");
	}
	if (cxa.demangle == NULL) {
		cwsym_logf(log, "no C++ demangler found; display names will be the normalized names");
		if (cxa.lib != NULL) {
			dlclose(cxa.lib);
			cxa.lib = NULL;
		}
		return false;
	}
	return true;
}

/** A module-relative half-open range. */
typedef struct {
	uint32_t start;
	uint32_t end;
} range_t;

/** State of one read. */
typedef struct {
	const char* path;
	const cwsym_sink_t* sink;
	const cwsym_log_t* log;
	Elf* elf;                 /**< Where DWARF and `.symtab` are read from. */
	Elf* exe;                 /**< The input when `elf` is its debug file, else `NULL`. */
	int debug_fd;
	char* debug_path;
	Dwarf* dwarf;
	uint64_t base;            /**< Address the client's offsets are relative to. */
	bool stopped;             /**< A callback returned `false`. */
	bool warned_scope;        /**< The scope stack overflowed once; said so. */
	bool warned_range;        /**< A range did not fit the table; said so. */

	barray(range_t) ranges;   /**< Every DWARF function range, for the `.symtab` pass. */

	/* The compilation unit being walked. */
	const char* unit;
	Dwarf_Files* files;
	const char* scope[MAX_SCOPE];
	int scope_len;
} reader_t;

/** Module-relative range of `[start, end)`, or false when it cannot be in the table. */
static bool
to_module(reader_t* r, Dwarf_Addr start, Dwarf_Addr end, range_t* out) {
	if (end <= start || start < r->base || end - r->base > UINT32_MAX) {
		if (!r->warned_range && end > start) {
			r->warned_range = true;
			cwsym_logf(
				r->log, "%s: range [%#" PRIx64 ", %#" PRIx64 ") is outside the module; skipped",
				r->path, (uint64_t)start, (uint64_t)end
			);
		}
		return false;
	}
	*out = (range_t){ .start = (uint32_t)(start - r->base), .end = (uint32_t)(end - r->base) };
	return true;
}

/**
 * Follow `DW_AT_abstract_origin` and `DW_AT_specification` to the DIE
 * that declares `die`. Returns `die` itself when there is no chain.
 */
static Dwarf_Die*
declaration_of(Dwarf_Die* die, Dwarf_Die* mem) {
	Dwarf_Die* cur = die;
	for (int hops = 0; hops < 4; ++hops) {
		Dwarf_Attribute attr;
		Dwarf_Attribute* a = dw.dwarf_attr(cur, DW_AT_abstract_origin, &attr);
		if (a == NULL) {
			a = dw.dwarf_attr(cur, DW_AT_specification, &attr);
		}
		if (a == NULL) {
			break;
		}
		Dwarf_Die next;
		if (dw.dwarf_formref_die(a, &next) == NULL) {
			break;
		}
		*mem = next;
		cur = mem;
	}
	return cur;
}

/** Whether a DIE of this tag names a scope component. */
static bool
is_scope_tag(int tag) {
	switch (tag) {
	case DW_TAG_namespace:
	case DW_TAG_class_type:
	case DW_TAG_structure_type:
	case DW_TAG_union_type:
	case DW_TAG_subprogram:
		return true;
	default:
		return false;
	}
}

/**
 * Scope components enclosing `decl`, outermost first, from the DIE tree
 * rather than from the walker's stack, for a DIE reached through a
 * specification. Returns the component count, or 0 when unknown.
 */
static int
scope_of(reader_t* r, Dwarf_Die* decl, const char** out, int cap) {
	Dwarf_Die* scopes = NULL;
	int n = dw.dwarf_getscopes_die(decl, &scopes);
	if (n <= 0) {
		free(scopes);
		return 0;
	}
	int count = 0;
	/* scopes[0] is decl itself; the rest walk outwards. */
	for (int i = n - 1; i >= 1 && count < cap; --i) {
		if (!is_scope_tag(dw.dwarf_tag(&scopes[i]))) {
			continue;
		}
		const char* name = dw.dwarf_diename(&scopes[i]);
		out[count++] = name != NULL ? name : "";
	}
	free(scopes);
	if (n - 1 > cap && !r->warned_scope) {
		r->warned_scope = true;
		cwsym_logf(r->log, "%s: a scope deeper than %d levels was truncated", r->path, MAX_SCOPE);
	}
	return count;
}

/** `ns::Class::name` of `decl`, display spelling, into `buf`. */
static void
qualified_name(reader_t* r, Dwarf_Die* decl, char* buf, size_t cap) {
	const char* comps[MAX_SCOPE];
	int n = scope_of(r, decl, comps, MAX_SCOPE);
	size_t len = 0;
	buf[0] = '\0';
	for (int i = 0; i < n; ++i) {
		int w = snprintf(buf + len, cap - len, "%s::", comps[i]);
		if (w < 0 || (size_t)w >= cap - len) {
			return;
		}
		len += (size_t)w;
	}
	const char* name = dw.dwarf_diename(decl);
	snprintf(buf + len, cap - len, "%s", name != NULL ? name : "");
}

/**
 * The demangled linkage name of `die`, or `NULL` for a C function, a
 * name the demangler rejects, or no demangler. The caller frees it.
 */
static char*
demangled_name(Dwarf_Die* die) {
	if (cxa.demangle == NULL) {
		return NULL;
	}
	Dwarf_Attribute mem;
	Dwarf_Attribute* a = dw.dwarf_attr_integrate(die, DW_AT_linkage_name, &mem);
	if (a == NULL) {
		a = dw.dwarf_attr_integrate(die, DW_AT_MIPS_linkage_name, &mem);
	}
	const char* mangled = a != NULL ? dw.dwarf_formstring(a) : NULL;
	if (mangled == NULL) {
		return NULL;
	}
	int status = 0;
	char* out = cxa.demangle(mangled, NULL, NULL, &status);
	if (status != 0) {
		free(out);
		return NULL;
	}
	return out;
}

/** The unsigned value of `attr` on `die`, or 0 when absent. */
static uint64_t
attr_udata(Dwarf_Die* die, unsigned attr) {
	Dwarf_Attribute mem;
	Dwarf_Attribute* a = dw.dwarf_attr(die, attr, &mem);
	Dwarf_Word value = 0;
	if (a == NULL || dw.dwarf_formudata(a, &value) != 0) {
		return 0;
	}
	return value;
}

/** Deliver one inlined range per `dwarf_ranges` entry of `die`. */
static void
emit_site(reader_t* r, Dwarf_Die* die, int depth) {
	Dwarf_Die origin_mem;
	Dwarf_Die* origin = declaration_of(die, &origin_mem);
	char scoped[1024];
	char* signature = demangled_name(origin);
	if (signature == NULL) {
		qualified_name(r, origin, scoped, sizeof(scoped));
	}
	const char* callee = signature != NULL ? signature : scoped;

	const char* call_file = NULL;
	Dwarf_Attribute file_attr;
	if (r->files != NULL && dw.dwarf_attr(die, DW_AT_call_file, &file_attr) != NULL) {
		call_file = dw.dwarf_filesrc(r->files, (size_t)attr_udata(die, DW_AT_call_file), NULL, NULL);
	}
	uint64_t call_line = attr_udata(die, DW_AT_call_line);

	Dwarf_Addr base = 0;
	Dwarf_Addr start;
	Dwarf_Addr end;
	ptrdiff_t off = 0;
	while (!r->stopped && (off = dw.dwarf_ranges(die, off, &base, &start, &end)) > 0) {
		range_t range;
		if (!to_module(r, start, end, &range)) {
			continue;
		}
		cwsym_site_t site = {
			.start = range.start,
			.len = range.end - range.start,
			.depth = depth,
			.callee = callee,
			.call_file = call_file,
			.call_line = (uint32_t)call_line,
		};
		if (!r->sink->site(r->sink->user, &site)) {
			r->stopped = true;
		}
	}
	free(signature);
}

/** Inline sites below `parent`, nesting through lexical blocks. */
static void
walk_inlines(reader_t* r, Dwarf_Die* parent, int depth) {
	Dwarf_Die die;
	if (dw.dwarf_child(parent, &die) != 0) {
		return;
	}
	do {
		switch (dw.dwarf_tag(&die)) {
		case DW_TAG_inlined_subroutine:
			emit_site(r, &die, depth);
			walk_inlines(r, &die, depth + 1);
			break;
		case DW_TAG_lexical_block:
			walk_inlines(r, &die, depth);
			break;
		default:
			break;
		}
	} while (!r->stopped && dw.dwarf_siblingof(&die, &die) == 0);
}

/**
 * Deliver a function with code: one symbol per range, then its inline
 * sites when the sink wants them.
 */
static void
visit_subprogram(reader_t* r, Dwarf_Die* die) {
	const char* name = dw.dwarf_diename(die);
	if (name == NULL) {
		return;
	}

	/*
	 * A method defined outside its class sits at unit level and points
	 * at its declaration; the scope is the declaration's, not the
	 * walker's. Everything else takes the stack as it stands.
	 */
	const char* scope[MAX_SCOPE + 1];
	int scope_len;
	Dwarf_Die decl_mem;
	Dwarf_Die* decl = declaration_of(die, &decl_mem);
	if (decl != die) {
		scope_len = scope_of(r, decl, scope, MAX_SCOPE);
	} else {
		scope_len = r->scope_len;
		memcpy(scope, r->scope, (size_t)scope_len * sizeof(scope[0]));
	}
	scope[scope_len++] = name;

	bool is_static = dw.dwarf_hasattr_integrate(die, DW_AT_external) == 0;
	char* display = demangled_name(die);
	bool any = false;
	Dwarf_Addr base = 0;
	Dwarf_Addr start;
	Dwarf_Addr end;
	ptrdiff_t off = 0;
	while (!r->stopped && (off = dw.dwarf_ranges(die, off, &base, &start, &end)) > 0) {
		range_t range;
		if (!to_module(r, start, end, &range)) {
			continue;
		}
		any = true;
		barray_push(r->ranges, range, NULL);
		cwsym_symbol_t sym = {
			.start = range.start,
			.size = range.end - range.start,
			.scope = scope,
			.scope_len = scope_len,
			.display = display,
			.unit = r->unit,
			.is_static = is_static,
		};
		if (!r->sink->symbol(r->sink->user, &sym)) {
			r->stopped = true;
		}
	}
	free(display);
	if (any && !r->stopped && r->sink->site != NULL) {
		walk_inlines(r, die, 0);
	}
}

/**
 * Walk the DIE tree below `parent`, keeping the lexical scope stack.
 * Function bodies are entered too: GCC and Clang place a local class or
 * a lambda's class, and its methods, inside the enclosing function.
 */
static void
walk(reader_t* r, Dwarf_Die* parent) {
	Dwarf_Die die;
	if (dw.dwarf_child(parent, &die) != 0) {
		return;
	}
	do {
		int tag = dw.dwarf_tag(&die);
		switch (tag) {
		case DW_TAG_namespace:
		case DW_TAG_class_type:
		case DW_TAG_structure_type:
		case DW_TAG_union_type:
			if (r->scope_len < MAX_SCOPE) {
				const char* name = dw.dwarf_diename(&die);
				r->scope[r->scope_len++] = name != NULL ? name : "";
				walk(r, &die);
				--r->scope_len;
			} else if (!r->warned_scope) {
				r->warned_scope = true;
				cwsym_logf(r->log, "%s: a scope deeper than %d levels was skipped", r->path, MAX_SCOPE);
			}
			break;
		case DW_TAG_subprogram:
			visit_subprogram(r, &die);
			/* Local classes and lambdas live inside the function's DIE, their methods with them. */
			if (!r->stopped && r->scope_len < MAX_SCOPE) {
				const char* name = dw.dwarf_diename(&die);
				r->scope[r->scope_len++] = name != NULL ? name : "";
				walk(r, &die);
				--r->scope_len;
			}
			break;
		case DW_TAG_lexical_block:
			walk(r, &die);
			break;
		default:
			break;
		}
	} while (!r->stopped && dw.dwarf_siblingof(&die, &die) == 0);
}

/** A line row waiting for its end address or a mergeable successor. */
typedef struct {
	bool have;
	uint32_t start;
	uint32_t len;
	const char* file;
	uint32_t line;
} pending_line_t;

static void
flush_line(reader_t* r, pending_line_t* p) {
	if (!p->have) {
		return;
	}
	cwsym_line_t line = { .start = p->start, .len = p->len, .file = p->file, .line = p->line };
	if (!r->sink->line(r->sink->user, &line)) {
		r->stopped = true;
	}
	p->have = false;
}

/**
 * Deliver the unit's line table as rows: each entry covers up to the
 * next entry's address, a sequence end closes the row, and adjacent
 * rows of one `file:line` merge.
 */
static void
emit_lines(reader_t* r, Dwarf_Die* cu) {
	Dwarf_Lines* lines;
	size_t n;
	if (dw.dwarf_getsrclines(cu, &lines, &n) != 0) {
		return;
	}
	pending_line_t done = { 0 };  /* Complete, may still grow. */
	bool in_row = false;          /* An entry whose end is the next address. */
	Dwarf_Addr open_addr = 0;
	const char* open_file = NULL;
	int open_line = 0;
	for (size_t i = 0; i < n && !r->stopped; ++i) {
		Dwarf_Line* l = dw.dwarf_onesrcline(lines, i);
		Dwarf_Addr addr;
		bool end_seq;
		int lineno;
		if (l == NULL || dw.dwarf_lineaddr(l, &addr) != 0
			|| dw.dwarf_lineendsequence(l, &end_seq) != 0 || dw.dwarf_lineno(l, &lineno) != 0) {
			continue;
		}
		if (in_row) {
			range_t range;
			if (to_module(r, open_addr, addr, &range)) {
				if (done.have && done.line == (uint32_t)open_line && done.start + done.len == range.start
					&& strcmp(done.file, open_file) == 0) {
					done.len = range.end - range.start + done.len;
				} else {
					flush_line(r, &done);
					done = (pending_line_t){
						.have = true, .start = range.start, .len = range.end - range.start,
						.file = open_file, .line = (uint32_t)open_line,
					};
				}
			}
			in_row = false;
		}
		if (end_seq || lineno <= 0) {
			continue;
		}
		const char* file = dw.dwarf_linesrc(l, NULL, NULL);
		if (file == NULL) {
			continue;
		}
		in_row = true;
		open_addr = addr;
		open_file = file;
		open_line = lineno;
	}
	flush_line(r, &done);
}

/** Every compilation unit: functions and sites, then lines. */
static void
walk_units(reader_t* r) {
	Dwarf_Off off = 0;
	Dwarf_Off next;
	size_t header;
	while (!r->stopped && dw.dwarf_nextcu(r->dwarf, off, &next, &header, NULL, NULL, NULL) == 0) {
		Dwarf_Die cu;
		if (dw.dwarf_offdie(r->dwarf, off + header, &cu) != NULL) {
			r->unit = dw.dwarf_diename(&cu);
			r->files = NULL;
			size_t nfiles;
			if (dw.dwarf_getsrcfiles(&cu, &r->files, &nfiles) != 0) {
				r->files = NULL;
			}
			r->scope_len = 0;
			walk(r, &cu);
			if (!r->stopped && r->sink->line != NULL) {
				emit_lines(r, &cu);
			}
		}
		off = next;
	}
}

/* .symtab fallback {{{ */

typedef struct {
	uint64_t addr;
	uint64_t size;
	const char* name;
	unsigned shndx;
	bool local;
} symtab_sym_t;

/** Order: address, then global before local, then larger, then name. */
static int
compare_syms(const void* pa, const void* pb) {
	const symtab_sym_t* a = pa;
	const symtab_sym_t* b = pb;
	if (a->addr != b->addr) {
		return a->addr < b->addr ? -1 : 1;
	}
	if (a->local != b->local) {
		return a->local ? 1 : -1;
	}
	if (a->size != b->size) {
		return a->size > b->size ? -1 : 1;
	}
	return strcmp(a->name, b->name);
}

static int
compare_ranges(const void* pa, const void* pb) {
	const range_t* a = pa;
	const range_t* b = pb;
	return a->start < b->start ? -1 : a->start > b->start ? 1 : 0;
}

typedef enum {
	COVER_NONE,    /**< No DWARF range touches the symbol. */
	COVER_INSIDE,  /**< A DWARF range contains the symbol; DWARF already told us. */
	COVER_PARTIAL, /**< The two disagree; the symbol is dropped. */
} coverage_t;

/** How the sorted DWARF ranges relate to `[start, end)`. */
static coverage_t
coverage(const reader_t* r, uint32_t start, uint32_t end) {
	/* First range starting at or after `start`; the one before it may still reach in. */
	size_t count = barray_len(r->ranges);
	size_t lo = 0;
	size_t hi = count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (r->ranges[mid].start < start) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	coverage_t result = COVER_NONE;
	for (size_t i = lo > 0 ? lo - 1 : 0; i < count && r->ranges[i].start < end; ++i) {
		const range_t* g = &r->ranges[i];
		if (g->end <= start) {
			continue;
		}
		if (g->start <= start && g->end >= end) {
			return COVER_INSIDE;
		}
		result = COVER_PARTIAL;
	}
	return result;
}

/** End address of section `shndx`, or 0 when unknown. */
static uint64_t
section_end(reader_t* r, unsigned shndx) {
	Elf_Scn* scn = dw.elf_getscn(r->elf, shndx);
	GElf_Shdr shdr;
	if (scn == NULL || dw.gelf_getshdr(scn, &shdr) == NULL) {
		return 0;
	}
	return shdr.sh_addr + shdr.sh_size;
}

/**
 * Functions from `.symtab` that DWARF did not describe. Zero-size
 * symbols extend to the next symbol of their section; aliases at one
 * address keep one entry; anything a DWARF range touches is DWARF's.
 */
static void
emit_symtab(reader_t* r) {
	qsort(r->ranges, barray_len(r->ranges), sizeof(r->ranges[0]), compare_ranges);

	barray(symtab_sym_t) syms = NULL;
	for (Elf_Scn* scn = NULL; (scn = dw.elf_nextscn(r->elf, scn)) != NULL;) {
		GElf_Shdr shdr;
		if (dw.gelf_getshdr(scn, &shdr) == NULL || shdr.sh_type != SHT_SYMTAB) {
			continue;
		}
		Elf_Data* data = dw.elf_getdata(scn, NULL);
		if (data == NULL || shdr.sh_entsize == 0) {
			continue;
		}
		size_t n = shdr.sh_size / shdr.sh_entsize;
		for (size_t i = 0; i < n; ++i) {
			GElf_Sym sym;
			if (dw.gelf_getsym(data, (int)i, &sym) == NULL
				|| (GELF_ST_TYPE(sym.st_info) != STT_FUNC && GELF_ST_TYPE(sym.st_info) != STT_GNU_IFUNC)
				|| sym.st_shndx == SHN_UNDEF || sym.st_shndx >= SHN_LORESERVE || sym.st_value == 0) {
				continue;
			}
			const char* name = dw.elf_strptr(r->elf, shdr.sh_link, sym.st_name);
			if (name == NULL || name[0] == '\0') {
				continue;
			}
			symtab_sym_t entry = {
				.addr = sym.st_value,
				.size = sym.st_size,
				.name = name,
				.shndx = sym.st_shndx,
				.local = GELF_ST_BIND(sym.st_info) == STB_LOCAL,
			};
			barray_push(syms, entry, NULL);
		}
	}
	size_t count = barray_len(syms);
	qsort(syms, count, sizeof(syms[0]), compare_syms);

	for (size_t i = 0; i < count && !r->stopped; ++i) {
		const symtab_sym_t* s = &syms[i];
		if (i > 0 && syms[i - 1].addr == s->addr) {
			continue;
		}
		uint64_t size = s->size;
		if (size == 0) {
			uint64_t next = section_end(r, s->shndx);
			for (size_t j = i + 1; j < count; ++j) {
				if (syms[j].addr != s->addr) {
					next = syms[j].shndx == s->shndx ? syms[j].addr : next;
					break;
				}
			}
			size = next > s->addr ? next - s->addr : 0;
		}
		range_t range;
		if (size == 0 || !to_module(r, s->addr, s->addr + size, &range)) {
			continue;
		}
		switch (coverage(r, range.start, range.end)) {
		case COVER_INSIDE:
			continue;
		case COVER_PARTIAL:
			cwsym_logf(
				r->log, "%s: .symtab %s [%#" PRIx32 ", %#" PRIx32 ") overlaps DWARF; dropped",
				r->path, s->name, range.start, range.end
			);
			continue;
		case COVER_NONE:
			break;
		}
		const char* scope[1] = { s->name };
		cwsym_symbol_t sym = {
			.start = range.start,
			.size = range.end - range.start,
			.scope = scope,
			.scope_len = 1,
			.display = NULL,
			.unit = NULL,
			.is_static = s->local,
		};
		if (!r->sink->symbol(r->sink->user, &sym)) {
			r->stopped = true;
		}
	}
	barray_free(syms, NULL);
}

/* }}} */

static bool
arch_of(GElf_Half machine, cwsym_arch_t* out) {
	switch (machine) {
	case EM_X86_64:
		*out = CWSYM_ARCH_X86_64;
		return true;
	case EM_AARCH64:
		*out = CWSYM_ARCH_AARCH64;
		return true;
	case EM_386:
		*out = CWSYM_ARCH_X86;
		return true;
	default:
		return false;
	}
}

/** Whether the file has a `.debug_info` section and whether it has a `.symtab`. */
static void
inspect_sections(reader_t* r, bool* has_dwarf, bool* has_symtab) {
	*has_dwarf = false;
	*has_symtab = false;
	size_t strndx;
	if (dw.elf_getshdrstrndx(r->elf, &strndx) != 0) {
		return;
	}
	for (Elf_Scn* scn = NULL; (scn = dw.elf_nextscn(r->elf, scn)) != NULL;) {
		GElf_Shdr shdr;
		if (dw.gelf_getshdr(scn, &shdr) == NULL) {
			continue;
		}
		if (shdr.sh_type == SHT_SYMTAB) {
			*has_symtab = true;
		}
		const char* name = dw.elf_strptr(r->elf, strndx, shdr.sh_name);
		if (name != NULL && (strcmp(name, ".debug_info") == 0 || strcmp(name, ".zdebug_info") == 0)) {
			*has_dwarf = true;
		}
	}
}

/**
 * Switch the read to the file `link` names beside the input, where the
 * input is what its path resolves to. The file must carry the input's
 * build id.
 */
static cwsym_status_t
follow_debuglink(reader_t* r, const char* link, const cwsym_module_t* mod) {
	char* real = realpath(r->path, NULL);
	const char* exe = real != NULL ? real : r->path;
	const char* slash = strrchr(exe, '/');
	size_t dir_len = slash != NULL ? (size_t)(slash - exe) + 1 : 0;
	size_t cap = dir_len + strlen(link) + 1;
	r->debug_path = malloc(cap);
	if (r->debug_path != NULL) {
		snprintf(r->debug_path, cap, "%.*s%s", (int)dir_len, exe, link);
	}
	free(real);
	if (r->debug_path == NULL) {
		return CWSYM_ERR_NOMEM;
	}

	r->debug_fd = open(r->debug_path, O_RDONLY);
	if (r->debug_fd < 0) {
		cwsym_logf(r->log, "%s: debug file %s: %s", r->path, r->debug_path, strerror(errno));
		return CWSYM_ERR_NO_DEBUG;
	}
	Elf* debug = dw.elf_begin(r->debug_fd, ELF_C_READ, NULL);
	const void* id;
	ssize_t id_len = debug != NULL ? dw.dwelf_elf_gnu_build_id(debug, &id) : -1;
	if (id_len != (ssize_t)mod->build_id_len || memcmp(id, mod->build_id, (size_t)id_len) != 0) {
		cwsym_logf(r->log, "%s: debug file %s is of another build", r->path, r->debug_path);
		if (debug != NULL) {
			dw.elf_end(debug);
		}
		return CWSYM_ERR_NO_DEBUG;
	}
	r->exe = r->elf;
	r->elf = debug;
	r->path = r->debug_path;
	return CWSYM_OK;
}

cwsym_status_t
cwsym_read_elf(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	(void)opts;
	if (!load(log)) {
		return CWSYM_ERR_UNSUPPORTED;
	}
	load_demangler(log);

	cwsym_status_t status = CWSYM_ERR_FORMAT;
	reader_t r = { .path = path, .sink = sink, .log = log, .debug_fd = -1 };
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		cwsym_logf(log, "%s: %s", path, strerror(errno));
		return CWSYM_ERR_IO;
	}
	r.elf = dw.elf_begin(fd, ELF_C_READ, NULL);
	if (r.elf == NULL || dw.elf_kind(r.elf) != ELF_K_ELF) {
		cwsym_logf(log, "%s: not an ELF file: %s", path, dw.elf_errmsg(-1));
		goto done;
	}

	GElf_Ehdr ehdr;
	if (dw.gelf_getehdr(r.elf, &ehdr) == NULL) {
		cwsym_logf(log, "%s: %s", path, dw.elf_errmsg(-1));
		goto done;
	}
	cwsym_module_t mod = { 0 };
	if (!arch_of(ehdr.e_machine, &mod.arch)) {
		cwsym_logf(log, "%s: machine %u is not supported", path, (unsigned)ehdr.e_machine);
		status = CWSYM_ERR_UNSUPPORTED;
		goto done;
	}

	const void* id;
	ssize_t id_len = dw.dwelf_elf_gnu_build_id(r.elf, &id);
	if (id_len <= 0) {
		cwsym_logf(log, "%s: no build id; link with -Wl,--build-id=sha1", path);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}
	if (id_len > CWSYM_BUILD_ID_CAP) {
		cwsym_logf(
			log, "%s: build id is %zd bytes, the table holds at most %d; link with -Wl,--build-id=sha1",
			path, id_len, CWSYM_BUILD_ID_CAP
		);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}
	memcpy(mod.build_id, id, (size_t)id_len);
	mod.build_id_len = (uint8_t)id_len;

	size_t phnum;
	bool found_load = false;
	if (dw.elf_getphdrnum(r.elf, &phnum) == 0) {
		for (size_t i = 0; i < phnum && !found_load; ++i) {
			GElf_Phdr phdr;
			if (dw.gelf_getphdr(r.elf, (int)i, &phdr) != NULL && phdr.p_type == PT_LOAD) {
				r.base = phdr.p_vaddr & ~PAGE_MASK;
				found_load = true;
			}
		}
	}
	if (!found_load) {
		cwsym_logf(log, "%s: no PT_LOAD segment", path);
		goto done;
	}

	bool has_dwarf;
	bool has_symtab;
	inspect_sections(&r, &has_dwarf, &has_symtab);
	GElf_Word crc;
	const char* link = has_dwarf ? NULL : dw.dwelf_elf_gnu_debuglink(r.elf, &crc);
	if (link != NULL) {
		status = follow_debuglink(&r, link, &mod);
		if (status != CWSYM_OK) {
			goto done;
		}
		status = CWSYM_ERR_FORMAT;
		inspect_sections(&r, &has_dwarf, &has_symtab);
	}
	if (!has_dwarf && !has_symtab) {
		cwsym_logf(log, "%s: neither DWARF nor .symtab; build with -g or keep the .debug file", r.path);
		status = CWSYM_ERR_NO_DEBUG;
		goto done;
	}
	if (has_dwarf) {
		r.dwarf = dw.dwarf_begin_elf(r.elf, DWARF_C_READ, NULL);
		if (r.dwarf == NULL) {
			cwsym_logf(log, "%s: cannot read DWARF: %s", r.path, dw.dwarf_errmsg(-1));
			goto done;
		}
	}

	status = CWSYM_OK;
	if (sink->begin != NULL) {
		sink->begin(sink->user, &mod);
	}
	if (r.dwarf != NULL) {
		walk_units(&r);
	}
	if (!r.stopped) {
		emit_symtab(&r);
	}
	if (sink->end != NULL) {
		sink->end(sink->user, status);
	}

done:
	barray_free(r.ranges, NULL);
	if (r.dwarf != NULL) {
		dw.dwarf_end(r.dwarf);
	}
	if (r.elf != NULL) {
		dw.elf_end(r.elf);
	}
	if (r.exe != NULL) {
		dw.elf_end(r.exe);
	}
	if (r.debug_fd >= 0) {
		close(r.debug_fd);
	}
	free(r.debug_path);
	close(fd);
	return status;
}

#else /* CWSYM_HAVE_LIBDW */

#ifdef __linux__
#warning "elfutils/libdw.h not found; the ELF reader is disabled (install libdw-dev)"
#endif

cwsym_status_t
cwsym_read_elf(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	(void)opts;
	(void)sink;
	cwsym_logf(log, "%s: ELF is read through libdw, which this build of the tool does not have", path);
	return CWSYM_ERR_UNSUPPORTED;
}

#endif /* CWSYM_HAVE_LIBDW */
