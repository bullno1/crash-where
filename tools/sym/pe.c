/**
 * @file pe.c
 * PE reader: the PDB beside the executable, through DIA.
 *
 * `msdia140.dll` is loaded by path and instantiated through its
 * `DllGetClassObject`, so no COM registration or initialization is
 * needed and the copy that matches the linker is the one used. The
 * path comes from `--dia`, else `VSINSTALLDIR`, else the Visual Studio
 * Installer's `vswhere.exe`. Names, ranges, linkage and units come from
 * the compiland and function symbols; lines from each function's line
 * table overlaid with the inlinee lines of its inline sites, deepest
 * first, so a row means the same as one read from DWARF.
 */
#include "reader.h"

#if defined(_WIN32) && defined(__has_include)
#if __has_include(<dia2.h>)
#define CWSYM_HAVE_DIA 1
#endif
#endif

#ifdef CWSYM_HAVE_DIA

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
/*
 * dia2.h's C binding of IDiaStackWalkHelper3 redeclares a method of its
 * base with the same name, which C++ overloads and C rejects. Skipping
 * that interface's definition costs nothing: the reader never walks stacks.
 */
#define __IDiaStackWalkHelper3_INTERFACE_DEFINED__
/* INITGUID defines the system GUIDs used below, IID_IClassFactory, in this file. */
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <dia2.h>
#include <dbghelp.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * The two DIA GUIDs the reader uses, as dia2.h spells them. Defined here
 * so the SDK's GUID library is not a build dependency; only the header
 * is. They are part of the DIA ABI, as fixed as DllGetClassObject.
 */
DEFINE_GUID(CLSID_DiaSource, 0xe6756135, 0x1e65, 0x4d17, 0x85, 0x76, 0x61, 0x07, 0x61, 0x39, 0x8c, 0x3c);
DEFINE_GUID(IID_IDiaDataSource, 0x79f1bb5f, 0xb66e, 0x48e5, 0xb6, 0xa9, 0x15, 0x45, 0xc3, 0x23, 0xca, 0x3d);

#define BARRAY_API static inline
#define BARRAY_IMPLEMENTATION
#include "vendor/barray.h"

#if defined(_M_AMD64)
#define DIA_BIN "amd64\\"
#elif defined(_M_ARM64)
#define DIA_BIN "arm64\\"
#else
#define DIA_BIN ""
#endif

/** UTF-8 copy of a wide string; the caller frees it. */
static char*
to_utf8(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 0) {
		return NULL;
	}
	char* s = malloc((size_t)n);
	if (s != NULL) {
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	}
	return s;
}

/** UTF-8 copy of a BSTR, which is freed; `NULL` for a `NULL` BSTR. */
static char*
take_bstr(BSTR b) {
	if (b == NULL) {
		return NULL;
	}
	char* s = to_utf8(b);
	SysFreeString(b);
	return s;
}

/* Locating msdia140.dll {{{ */

/**
 * Ask the Visual Studio Installer's `vswhere.exe`, at its documented
 * location, for the newest installation path. Spawned with a pipe, no
 * shell.
 */
static bool
run_vswhere(char* out, size_t cap, const cwsym_log_t* log) {
	char pf[MAX_PATH];
	if (GetEnvironmentVariableA("ProgramFiles(x86)", pf, sizeof(pf)) == 0
		&& GetEnvironmentVariableA("ProgramFiles", pf, sizeof(pf)) == 0) {
		return false;
	}
	char cmd[MAX_PATH + 128];
	snprintf(
		cmd, sizeof(cmd),
		"\"%s\\Microsoft Visual Studio\\Installer\\vswhere.exe\" -latest -products * -property installationPath", pf
	);

	SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa), .bInheritHandle = TRUE };
	HANDLE rd;
	HANDLE wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOA si = {
		.cb = sizeof(si),
		.dwFlags = STARTF_USESTDHANDLES,
		.hStdOutput = wr,
		.hStdError = GetStdHandle(STD_ERROR_HANDLE),
		.hStdInput = GetStdHandle(STD_INPUT_HANDLE),
	};
	PROCESS_INFORMATION pi = { 0 };
	BOOL started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	CloseHandle(wr);
	if (!started) {
		cwsym_logf(log, "vswhere.exe not found under %s; set VSINSTALLDIR or pass --dia", pf);
		CloseHandle(rd);
		return false;
	}
	size_t len = 0;
	DWORD got;
	while (len + 1 < cap && ReadFile(rd, out + len, (DWORD)(cap - 1 - len), &got, NULL) && got > 0) {
		len += got;
	}
	out[len] = '\0';
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
		out[--len] = '\0';
	}
	if (len == 0) {
		cwsym_logf(log, "vswhere.exe found no Visual Studio; set VSINSTALLDIR or pass --dia");
		return false;
	}
	return true;
}

/** Path of `msdia140.dll` into `out`, from the option, the environment, or vswhere. */
static bool
locate_dia(const cwsym_read_options_t* opts, char* out, size_t cap, const cwsym_log_t* log) {
	if (opts->dia != NULL) {
		snprintf(out, cap, "%s", opts->dia);
		return true;
	}
	char root[MAX_PATH];
	if (GetEnvironmentVariableA("VSINSTALLDIR", root, sizeof(root)) == 0 && !run_vswhere(root, sizeof(root), log)) {
		return false;
	}
	size_t n = strlen(root);
	while (n > 0 && (root[n - 1] == '\\' || root[n - 1] == '/')) {
		root[--n] = '\0';
	}
	snprintf(out, cap, "%s\\DIA SDK\\bin\\" DIA_BIN "msdia140.dll", root);
	return true;
}

typedef HRESULT (WINAPI *get_class_object_fn)(REFCLSID clsid, REFIID iid, void** out);

/** Instantiate the data source from the DLL at `path`, without COM registration. */
static IDiaDataSource*
open_dia(const char* path, const cwsym_log_t* log) {
	HMODULE dll = LoadLibraryA(path);
	if (dll == NULL) {
		cwsym_logf(log, "%s: cannot load (error %lu); it needs the VC runtime beside it or installed", path, GetLastError());
		return NULL;
	}
	get_class_object_fn get_class_object = (get_class_object_fn)(void*)GetProcAddress(dll, "DllGetClassObject");
	if (get_class_object == NULL) {
		cwsym_logf(log, "%s: no DllGetClassObject; not msdia140.dll", path);
		return NULL;
	}
	IClassFactory* factory = NULL;
	IDiaDataSource* source = NULL;
	HRESULT hr = get_class_object(&CLSID_DiaSource, &IID_IClassFactory, (void**)&factory);
	if (SUCCEEDED(hr)) {
		hr = IClassFactory_CreateInstance(factory, NULL, &IID_IDiaDataSource, (void**)&source);
		IClassFactory_Release(factory);
	}
	if (FAILED(hr)) {
		cwsym_logf(log, "%s: cannot create the DIA data source (HRESULT %#lx)", path, (unsigned long)hr);
		return NULL;
	}
	return source;
}

/* }}} */

/* Lines and inline sites {{{ */

/** One line record from DIA, tagged with how deeply inlined it is. */
typedef struct {
	uint32_t rva;
	uint32_t len;
	int depth;          /**< 0 for the function's own table, one more per inline level. */
	const char* file;   /**< Points into the file cache. */
	uint32_t line;
} cand_t;

/** A source file seen while reading one function. */
typedef struct {
	DWORD id;
	char* path;
} file_t;

typedef struct {
	uint32_t start;
	uint32_t end;
} range_t;

typedef struct {
	const char* path;
	const cwsym_sink_t* sink;
	const cwsym_log_t* log;
	IDiaSession* session;
	bool stopped;
	size_t functions;
	/* State of the function being read. */
	barray(cand_t) cands;
	barray(file_t) files;
	barray(range_t) ranges;
	barray(range_t) fragments; /**< Separated code of the current function, merged. */
} reader_t;

static void
clear_files(reader_t* r) {
	for (size_t i = 0; i < barray_len(r->files); ++i) {
		free(r->files[i].path);
	}
	barray_clear(r->files);
}

/** The UTF-8 path of `file`, cached per function by DIA's unique id. */
static const char*
file_path(reader_t* r, IDiaSourceFile* file) {
	DWORD id = 0;
	IDiaSourceFile_get_uniqueId(file, &id);
	for (size_t i = 0; i < barray_len(r->files); ++i) {
		if (r->files[i].id == id) {
			return r->files[i].path;
		}
	}
	BSTR name = NULL;
	IDiaSourceFile_get_fileName(file, &name);
	char* path = take_bstr(name);
	if (path == NULL) {
		return NULL;
	}
	file_t entry = { .id = id, .path = path };
	barray_push(r->files, entry, NULL);
	return path;
}

/** Append every record of `lines` as a candidate at `depth`; returns their ranges in `r->ranges`. */
static void
collect_lines(reader_t* r, IDiaEnumLineNumbers* lines, int depth) {
	IDiaLineNumber* line;
	ULONG fetched;
	while (IDiaEnumLineNumbers_Next(lines, 1, &line, &fetched) == S_OK && fetched == 1) {
		DWORD rva = 0;
		DWORD len = 0;
		DWORD number = 0;
		IDiaSourceFile* file = NULL;
		IDiaLineNumber_get_relativeVirtualAddress(line, &rva);
		IDiaLineNumber_get_length(line, &len);
		IDiaLineNumber_get_lineNumber(line, &number);
		IDiaLineNumber_get_sourceFile(line, &file);
		const char* path = file != NULL ? file_path(r, file) : NULL;
		if (file != NULL) {
			IDiaSourceFile_Release(file);
		}
		IDiaLineNumber_Release(line);
		if (len == 0) {
			continue;
		}
		range_t range = { .start = rva, .end = rva + len };
		barray_push(r->ranges, range, NULL);
		if (path == NULL || number == 0) {
			continue;
		}
		cand_t c = { .rva = rva, .len = len, .depth = depth, .file = path, .line = number };
		barray_push(r->cands, c, NULL);
	}
}

static int
compare_ranges(const void* pa, const void* pb) {
	const range_t* a = pa;
	const range_t* b = pb;
	return a->start < b->start ? -1 : a->start > b->start ? 1 : 0;
}

/** Sort `r->ranges` and merge touching or overlapping ones in place; returns the count. */
static size_t
coalesce_ranges(reader_t* r) {
	qsort(r->ranges, barray_len(r->ranges), sizeof(range_t), compare_ranges);
	size_t n = 0;
	for (size_t i = 0; i < barray_len(r->ranges); ++i) {
		if (n > 0 && r->ranges[i].start <= r->ranges[n - 1].end) {
			if (r->ranges[i].end > r->ranges[n - 1].end) {
				r->ranges[n - 1].end = r->ranges[i].end;
			}
		} else {
			r->ranges[n++] = r->ranges[i];
		}
	}
	return n;
}

/** The candidate at `depth` covering `rva`, or `NULL`: the enclosing level's line there. */
static const cand_t*
line_at(const reader_t* r, uint32_t rva, int depth) {
	for (size_t i = 0; i < barray_len(r->cands); ++i) {
		const cand_t* c = &r->cands[i];
		if (c->depth == depth && c->rva <= rva && rva - c->rva < c->len) {
			return c;
		}
	}
	return NULL;
}

/**
 * Inline sites below `parent`. Each site's inlinee lines give both its
 * code ranges and, at `depth + 1`, the rows that override the caller's;
 * its call site is the enclosing level's line at its first byte.
 */
static void
walk_sites(reader_t* r, IDiaSymbol* parent, int depth) {
	IDiaEnumSymbols* sites = NULL;
	if (FAILED(IDiaSymbol_findChildren(parent, SymTagInlineSite, NULL, nsNone, &sites)) || sites == NULL) {
		return;
	}
	IDiaSymbol* site;
	ULONG fetched;
	while (!r->stopped && IDiaEnumSymbols_Next(sites, 1, &site, &fetched) == S_OK && fetched == 1) {
		BSTR name = NULL;
		IDiaSymbol_get_name(site, &name);
		char* callee = take_bstr(name);
		barray_clear(r->ranges);
		IDiaEnumLineNumbers* lines = NULL;
		if (SUCCEEDED(IDiaSymbol_findInlineeLines(site, &lines)) && lines != NULL) {
			collect_lines(r, lines, depth + 1);
			IDiaEnumLineNumbers_Release(lines);
		}
		size_t n = coalesce_ranges(r);
		if (callee != NULL && n > 0) {
			const cand_t* call = line_at(r, r->ranges[0].start, depth);
			for (size_t i = 0; i < n && !r->stopped; ++i) {
				cwsym_site_t s = {
					.start = r->ranges[i].start,
					.len = r->ranges[i].end - r->ranges[i].start,
					.depth = depth,
					.callee = callee,
					.call_file = call != NULL ? call->file : NULL,
					.call_line = call != NULL ? call->line : 0,
				};
				if (!r->sink->site(r->sink->user, &s)) {
					r->stopped = true;
				}
			}
			walk_sites(r, site, depth + 1);
		}
		free(callee);
		IDiaSymbol_Release(site);
	}
	IDiaEnumSymbols_Release(sites);
}

static int
compare_u32(const void* pa, const void* pb) {
	uint32_t a = *(const uint32_t*)pa;
	uint32_t b = *(const uint32_t*)pb;
	return a < b ? -1 : a > b ? 1 : 0;
}

/**
 * Deliver the function's line rows: between every pair of candidate
 * boundaries the deepest candidate wins, and adjacent rows of one
 * `file:line` merge.
 */
static void
emit_lines(reader_t* r) {
	size_t n = barray_len(r->cands);
	if (n == 0) {
		return;
	}
	barray(uint32_t) points = NULL;
	for (size_t i = 0; i < n; ++i) {
		barray_push(points, r->cands[i].rva, NULL);
		barray_push(points, r->cands[i].rva + r->cands[i].len, NULL);
	}
	qsort(points, barray_len(points), sizeof(uint32_t), compare_u32);

	cwsym_line_t pending = { 0 };
	bool have = false;
	for (size_t i = 0; i + 1 < barray_len(points) && !r->stopped; ++i) {
		uint32_t a = points[i];
		uint32_t b = points[i + 1];
		if (a == b) {
			continue;
		}
		const cand_t* best = NULL;
		for (size_t k = 0; k < n; ++k) {
			const cand_t* c = &r->cands[k];
			if (c->rva <= a && b <= c->rva + c->len && (best == NULL || c->depth > best->depth)) {
				best = c;
			}
		}
		if (best != NULL && have && pending.line == best->line && pending.start + pending.len == a
			&& strcmp(pending.file, best->file) == 0) {
			pending.len += b - a;
			continue;
		}
		if (have && !r->sink->line(r->sink->user, &pending)) {
			r->stopped = true;
		}
		have = best != NULL;
		if (have) {
			pending = (cwsym_line_t){ .start = a, .len = b - a, .file = best->file, .line = best->line };
		}
	}
	if (have && !r->stopped && !r->sink->line(r->sink->user, &pending)) {
		r->stopped = true;
	}
	barray_free(points, NULL);
}

/* }}} */

/* Functions {{{ */

/**
 * Separated code: blocks of the function placed outside its own range,
 * which CodeView records as `S_SEPCODE` and DIA presents as
 * `SymTagBlock` children with their own address. Nested blocks of a
 * fragment lie inside it, so the ranges are merged afterwards.
 */
static void
collect_fragments(reader_t* r, IDiaSymbol* parent, uint32_t fn_start, uint32_t fn_end) {
	IDiaEnumSymbols* blocks = NULL;
	if (FAILED(IDiaSymbol_findChildren(parent, SymTagBlock, NULL, nsNone, &blocks)) || blocks == NULL) {
		return;
	}
	IDiaSymbol* block;
	ULONG fetched;
	while (IDiaEnumSymbols_Next(blocks, 1, &block, &fetched) == S_OK && fetched == 1) {
		DWORD rva = 0;
		ULONGLONG len = 0;
		if (SUCCEEDED(IDiaSymbol_get_relativeVirtualAddress(block, &rva)) && SUCCEEDED(IDiaSymbol_get_length(block, &len))
			&& len > 0 && len <= UINT32_MAX && (rva >= fn_end || rva + len <= fn_start)) {
			range_t range = { .start = rva, .end = rva + (uint32_t)len };
			barray_push(r->ranges, range, NULL);
		}
		collect_fragments(r, block, fn_start, fn_end);
		IDiaSymbol_Release(block);
	}
	IDiaEnumSymbols_Release(blocks);
}

/** Display flags: the signature without calling convention, return type, or access. */
#define DISPLAY_FLAGS \
	(UNDNAME_NO_MS_KEYWORDS | UNDNAME_NO_FUNCTION_RETURNS | UNDNAME_NO_ALLOCATION_MODEL \
	| UNDNAME_NO_ALLOCATION_LANGUAGE | UNDNAME_NO_ACCESS_SPECIFIERS | UNDNAME_NO_THISTYPE \
	| UNDNAME_NO_MEMBER_TYPE)

static void
visit_function(reader_t* r, IDiaSymbol* fn, const char* unit) {
	DWORD rva = 0;
	ULONGLONG len = 0;
	if (FAILED(IDiaSymbol_get_relativeVirtualAddress(fn, &rva)) || FAILED(IDiaSymbol_get_length(fn, &len))
		|| len == 0 || len > UINT32_MAX) {
		return;
	}
	BSTR wname = NULL;
	IDiaSymbol_get_name(fn, &wname);
	char* name = take_bstr(wname);
	if (name == NULL || name[0] == '\0') {
		free(name);
		return;
	}
	BOOL is_static = FALSE;
	IDiaSymbol_get_isStatic(fn, &is_static);
	BSTR wdisplay = NULL;
	IDiaSymbol_get_undecoratedNameEx(fn, DISPLAY_FLAGS, &wdisplay);
	char* display = take_bstr(wdisplay);
	if (display != NULL && strcmp(display, name) == 0) {
		free(display);
		display = NULL;
	}

	++r->functions;
	const char* scope[] = { name };
	cwsym_symbol_t sym = {
		.start = rva,
		.size = (uint32_t)len,
		.scope = scope,
		.scope_len = 1,
		.display = display,
		.unit = unit,
		.is_static = is_static != FALSE,
	};
	if (!r->sink->symbol(r->sink->user, &sym)) {
		r->stopped = true;
	}

	/* One more row per separated fragment, sharing everything but the range. */
	barray_clear(r->ranges);
	collect_fragments(r, fn, rva, rva + (uint32_t)len);
	size_t nfrag = coalesce_ranges(r);
	barray_clear(r->fragments);
	for (size_t i = 0; i < nfrag && !r->stopped; ++i) {
		barray_push(r->fragments, r->ranges[i], NULL);
		sym.start = r->ranges[i].start;
		sym.size = r->ranges[i].end - r->ranges[i].start;
		if (!r->sink->symbol(r->sink->user, &sym)) {
			r->stopped = true;
		}
	}

	if (!r->stopped && (r->sink->line != NULL || r->sink->site != NULL)) {
		barray_clear(r->cands);
		clear_files(r);
		IDiaEnumLineNumbers* lines = NULL;
		if (SUCCEEDED(IDiaSession_findLinesByRVA(r->session, rva, (DWORD)len, &lines)) && lines != NULL) {
			barray_clear(r->ranges);
			collect_lines(r, lines, 0);
			IDiaEnumLineNumbers_Release(lines);
		}
		for (size_t i = 0; i < barray_len(r->fragments); ++i) {
			const range_t* f = &r->fragments[i];
			if (SUCCEEDED(IDiaSession_findLinesByRVA(r->session, f->start, f->end - f->start, &lines)) && lines != NULL) {
				barray_clear(r->ranges);
				collect_lines(r, lines, 0);
				IDiaEnumLineNumbers_Release(lines);
			}
		}
		if (r->sink->site != NULL) {
			walk_sites(r, fn, 0);
		}
		if (r->sink->line != NULL && !r->stopped) {
			emit_lines(r);
		}
	}
	free(display);
	free(name);
}

/**
 * The compiland's source file, so the unit stem agrees with the DWARF
 * readers. A compiland's name is its object file, and its source file
 * property is empty in PDBs the MSVC linker writes; the path cl.exe
 * received is in the `src` entry of the compiland's environment. The
 * object file name stands in for a compiland without one, such as
 * import thunks and linker-made code.
 */
static char*
compiland_unit(IDiaSymbol* compiland) {
	char* unit = NULL;
	IDiaEnumSymbols* envs = NULL;
	if (SUCCEEDED(IDiaSymbol_findChildren(compiland, SymTagCompilandEnv, NULL, nsNone, &envs)) && envs != NULL) {
		IDiaSymbol* env;
		ULONG fetched;
		while (unit == NULL && IDiaEnumSymbols_Next(envs, 1, &env, &fetched) == S_OK && fetched == 1) {
			BSTR name = NULL;
			IDiaSymbol_get_name(env, &name);
			if (name != NULL && wcscmp(name, L"src") == 0) {
				VARIANT value;
				VariantInit(&value);
				if (IDiaSymbol_get_value(env, &value) == S_OK && V_VT(&value) == VT_BSTR && V_BSTR(&value) != NULL && V_BSTR(&value)[0] != L'\0') {
					unit = to_utf8(V_BSTR(&value));
				}
				VariantClear(&value);
			}
			SysFreeString(name);
			IDiaSymbol_Release(env);
		}
		IDiaEnumSymbols_Release(envs);
	}
	if (unit == NULL) {
		BSTR name = NULL;
		IDiaSymbol_get_name(compiland, &name);
		unit = take_bstr(name);
	}
	return unit;
}

/** Every function of every compiland. */
static void
walk_compilands(reader_t* r, IDiaSymbol* global) {
	IDiaEnumSymbols* compilands = NULL;
	if (FAILED(IDiaSymbol_findChildren(global, SymTagCompiland, NULL, nsNone, &compilands)) || compilands == NULL) {
		return;
	}
	IDiaSymbol* compiland;
	ULONG fetched;
	while (!r->stopped && IDiaEnumSymbols_Next(compilands, 1, &compiland, &fetched) == S_OK && fetched == 1) {
		char* unit = compiland_unit(compiland);
		IDiaEnumSymbols* functions = NULL;
		if (SUCCEEDED(IDiaSymbol_findChildren(compiland, SymTagFunction, NULL, nsNone, &functions)) && functions != NULL) {
			IDiaSymbol* fn;
			while (!r->stopped && IDiaEnumSymbols_Next(functions, 1, &fn, &fetched) == S_OK && fetched == 1) {
				visit_function(r, fn, unit);
				IDiaSymbol_Release(fn);
			}
			IDiaEnumSymbols_Release(functions);
		}
		free(unit);
		IDiaSymbol_Release(compiland);
	}
	IDiaEnumSymbols_Release(compilands);
}

/* }}} */

static bool
arch_of(DWORD machine, cwsym_arch_t* out) {
	switch (machine) {
	case IMAGE_FILE_MACHINE_AMD64:
		*out = CWSYM_ARCH_X86_64;
		return true;
	case IMAGE_FILE_MACHINE_ARM64:
		*out = CWSYM_ARCH_AARCH64;
		return true;
	case IMAGE_FILE_MACHINE_I386:
		*out = CWSYM_ARCH_X86;
		return true;
	default:
		return false;
	}
}

/** The PDB's GUID in printed field order, then the age big-endian: the client's id bytes. */
static void
build_id_of(const GUID* g, DWORD age, cwsym_module_t* mod) {
	uint8_t* b = mod->build_id;
	b[0] = (uint8_t)(g->Data1 >> 24);
	b[1] = (uint8_t)(g->Data1 >> 16);
	b[2] = (uint8_t)(g->Data1 >> 8);
	b[3] = (uint8_t)g->Data1;
	b[4] = (uint8_t)(g->Data2 >> 8);
	b[5] = (uint8_t)g->Data2;
	b[6] = (uint8_t)(g->Data3 >> 8);
	b[7] = (uint8_t)g->Data3;
	memcpy(b + 8, g->Data4, 8);
	b[16] = (uint8_t)(age >> 24);
	b[17] = (uint8_t)(age >> 16);
	b[18] = (uint8_t)(age >> 8);
	b[19] = (uint8_t)age;
	mod->build_id_len = 20;
}

cwsym_status_t
cwsym_read_pe(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	char dia[MAX_PATH + 64];
	if (!locate_dia(opts, dia, sizeof(dia), log)) {
		return CWSYM_ERR_UNSUPPORTED;
	}
	IDiaDataSource* source = open_dia(dia, log);
	if (source == NULL) {
		return CWSYM_ERR_UNSUPPORTED;
	}

	cwsym_status_t status = CWSYM_ERR_NO_DEBUG;
	reader_t r = { .path = path, .sink = sink, .log = log };
	IDiaSymbol* global = NULL;
	wchar_t* wpath = NULL;
	int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	wpath = wlen > 0 ? malloc((size_t)wlen * sizeof(wchar_t)) : NULL;
	if (wpath == NULL) {
		status = CWSYM_ERR_NOMEM;
		goto done;
	}
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

	HRESULT hr = IDiaDataSource_loadDataForExe(source, wpath, NULL, NULL);
	if (FAILED(hr)) {
		cwsym_logf(log, "%s: no matching PDB beside it (HRESULT %#lx); link with /DEBUG:FULL and keep the PDB", path, (unsigned long)hr);
		goto done;
	}
	if (FAILED(IDiaDataSource_openSession(source, &r.session)) || FAILED(IDiaSession_get_globalScope(r.session, &global))) {
		cwsym_logf(log, "%s: cannot open a DIA session on its PDB", path);
		status = CWSYM_ERR_FORMAT;
		goto done;
	}

	cwsym_module_t mod = { 0 };
	DWORD machine = 0;
	IDiaSymbol_get_machineType(global, &machine);
	if (!arch_of(machine, &mod.arch)) {
		cwsym_logf(log, "%s: machine %#lx is not supported", path, (unsigned long)machine);
		status = CWSYM_ERR_UNSUPPORTED;
		goto done;
	}
	GUID guid;
	DWORD age = 0;
	if (FAILED(IDiaSymbol_get_guid(global, &guid))) {
		cwsym_logf(log, "%s: the PDB has no GUID", path);
		status = CWSYM_ERR_NO_BUILD_ID;
		goto done;
	}
	IDiaSymbol_get_age(global, &age);
	build_id_of(&guid, age, &mod);

	status = CWSYM_OK;
	if (sink->begin != NULL) {
		sink->begin(sink->user, &mod);
	}
	walk_compilands(&r, global);
	if (r.functions == 0 && !r.stopped) {
		cwsym_logf(log, "%s: the PDB has no functions; it holds public symbols only, compile with /Zi or /Z7", path);
		status = CWSYM_ERR_NO_DEBUG;
	}
	if (sink->end != NULL) {
		sink->end(sink->user, status);
	}

done:
	clear_files(&r);
	barray_free(r.files, NULL);
	barray_free(r.cands, NULL);
	barray_free(r.ranges, NULL);
	barray_free(r.fragments, NULL);
	free(wpath);
	if (global != NULL) {
		IDiaSymbol_Release(global);
	}
	if (r.session != NULL) {
		IDiaSession_Release(r.session);
	}
	IDiaDataSource_Release(source);
	return status;
}

#else /* CWSYM_HAVE_DIA */

#if defined(_WIN32) && defined(_MSC_VER)
#pragma message("dia2.h not found; the PE reader is disabled (set VSINSTALLDIR)")
#endif

cwsym_status_t
cwsym_read_pe(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	(void)opts;
	(void)sink;
	cwsym_logf(log, "%s: PE is read through DIA, which this build of the tool does not have", path);
	return CWSYM_ERR_UNSUPPORTED;
}

#endif /* CWSYM_HAVE_DIA */
