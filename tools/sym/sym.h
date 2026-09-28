/**
 * @file sym.h
 * Internal library behind `cwsym`: readers, normalizer, table
 * builder, parser, lookup, and symbolizer.
 *
 * Data flows one way. A reader walks one debug file and pushes raw
 * functions, line rows, and inline sites into a @ref cwsym_sink_t. A
 * @ref cwsym_table_builder_t is that sink's usual target: it normalizes
 * each function name under the current rule-set and collects rows, and
 * @ref cwsym_table_end sorts, validates, and serializes them into one
 * @ref cwsym_table_t, the immutable file image. @ref cwsym_table_parse
 * yields the same type from bytes read back, so @ref cwsym_lookup,
 * @ref cwsym_symbolize, @ref cwsym_dump, and @ref cwsym_upload never
 * know which way a table came to exist. Both lookups are the reference
 * implementations of the searches the Worker and the dashboard mirror
 * and run on the file layout.
 */
#ifndef CWSYM_H
#define CWSYM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cw.h>

#define CWSYM_VERSION 1 /**< File layout version written into the header. */
#define CWSYM_RULES   1 /**< Normalization rule-set version. Bump per DESIGN.md §6.4. */

#define CWSYM_BUILD_ID_CAP 20
#define CWSYM_FLAG_HAS_INLINES (1u << 0) /**< Reserved; never set by this version. */
#define CWSYM_FLAG_HAS_LINES   (1u << 1) /**< The line and inline-site sections are present. */

#define CWSYM_NO_PARENT UINT32_MAX /**< cwsym_table_t::site_parents value of a site inlined directly into its function. */

/**
 * Outcome of a library call.
 *
 * A message with the detail goes to the caller's @ref cwsym_log_t before
 * the call returns; the status is what tests and exit codes read.
 */
typedef enum {
	CWSYM_OK = 0,
	CWSYM_ERR_IO,          /**< The input cannot be opened or read, or a request failed. */
	CWSYM_ERR_FORMAT,      /**< Not PE, ELF, Wasm, or `cwsym`, or the file is malformed. */
	CWSYM_ERR_UNSUPPORTED, /**< The format is recognized but this host has no reader for it, such as PE on Linux. */
	CWSYM_ERR_NO_DEBUG,    /**< Debug information is missing or unusable: no PDB, GUID/age mismatch, publics-only PDB, ELF with neither DWARF nor `.symtab`. */
	CWSYM_ERR_NO_BUILD_ID, /**< The input carries no build id. */
	CWSYM_ERR_INVALID,     /**< A table invariant is broken: empty, overlapping ranges, bad nesting, wrong `rules`, string out of bounds, or a builder used out of order. */
	CWSYM_ERR_NOMEM,
} cwsym_status_t;

typedef enum {
	CWSYM_ARCH_X86_64  = 1,
	CWSYM_ARCH_AARCH64 = 2,
	CWSYM_ARCH_X86     = 3,
	CWSYM_ARCH_WASM32  = 4,
} cwsym_arch_t;

/**
 * Identity of the module a table describes.
 *
 * `build_id` holds the bytes whose lowercase hex is the id string the
 * client sends and the R2 key: ELF note bytes as they are; PDB GUID in
 * its printed field order followed by the age as four big-endian bytes;
 * the content hash for Wasm.
 */
typedef struct {
	cwsym_arch_t arch;
	uint8_t build_id[CWSYM_BUILD_ID_CAP];
	uint8_t build_id_len;
} cwsym_module_t;

/**
 * Diagnostic sink. Optional everywhere; `NULL` discards messages.
 */
typedef struct {
	/**
	 * Receive one message without a trailing newline. Warnings and the
	 * detail behind a failing status both arrive here.
	 */
	void (*log)(void* user, const char* msg);
	void* user;
} cwsym_log_t;

/**
 * One function range as a reader found it, before any naming rule.
 *
 * Every pointer is valid only for the duration of the sink call. A
 * function the compiler split into several ranges arrives once per
 * range with the same scope. A name the reader cannot place in a scope
 * arrives as a single component.
 */
typedef struct {
	uint32_t start;      /**< Module-relative start in the client's offset convention: RVA on PE, address minus the first `PT_LOAD` address on ELF, code-section byte offset on Wasm. */
	uint32_t size;       /**< Length in bytes; 0 when the format does not record one. */
	const char** scope;  /**< Scope components, outermost first; the last is the function. An unnamed namespace is `""`. A component may itself be qualified, as when the debug info stores names flat; the normalizer splits it. Template arguments and MSVC type keywords may be present; the normalizer removes them. */
	int scope_len;       /**< Number of components, at least 1. */
	const char* display; /**< Human-readable name with parameter types, or `NULL` to display the normalized name. Never hashed. */
	const char* unit;    /**< Compilation unit or compiland as the debug info names it, such as `../src/render.c` or `render.obj`, or `NULL` when unknown. */
	bool is_static;      /**< Internal linkage. Such a function gets the unit stem as a prefix. */
} cwsym_symbol_t;

/**
 * One line-table row: the bytes in `[start, start + len)` came from
 * `file:line`, after inlining. Pointers are valid only for the sink call.
 */
typedef struct {
	uint32_t start;
	uint32_t len;
	const char* file; /**< Source path as the compiler recorded it. */
	uint32_t line;    /**< 1-based. */
} cwsym_line_t;

/**
 * One range of an inlined call. A callee inlined into several
 * discontiguous ranges arrives once per range with the same call site.
 * Pointers are valid only for the sink call.
 */
typedef struct {
	uint32_t start;
	uint32_t len;
	int depth;             /**< 0 when inlined directly into the function the table names; each nesting level adds one. */
	const char* callee;    /**< Qualified name of the inlined function, display spelling. */
	const char* call_file; /**< Source path of the call site, or `NULL` when unknown. */
	uint32_t call_line;    /**< 1-based line of the call site, or 0 when unknown. */
} cwsym_site_t;

/**
 * Receiver of a reader's output.
 *
 * `begin` and `end` bracket one read; every other call happens between
 * them. Every member except `symbol` may be `NULL`. A `NULL` `line` or
 * `site` makes the reader skip the corresponding work, so a caller that
 * only wants names pays only for names.
 */
typedef struct {
	/** Called once, before anything else, with the module identity. Not called when the file is rejected before its identity is known. */
	void (*begin)(void* user, const cwsym_module_t* mod);
	/** Called once per function range. Return `false` to stop the reader, which then returns ::CWSYM_OK. */
	bool (*symbol)(void* user, const cwsym_symbol_t* sym);
	/** Called once per line-table row. Same return contract as `symbol`. */
	bool (*line)(void* user, const cwsym_line_t* line);
	/** Called once per inline-site range. Same return contract as `symbol`. */
	bool (*site)(void* user, const cwsym_site_t* site);
	/**
	 * Called once after the last item whenever `begin` was called, whether
	 * the reader ran to the end, was stopped, or failed.
	 *
	 * @param status  What the reader is about to return.
	 */
	void (*end)(void* user, cwsym_status_t status);
	void* user;
} cwsym_sink_t;

/**
 * Reader inputs that cannot be found from the file itself.
 */
typedef struct {
	const char* dia;        /**< Path of `msdia140.dll`, or `NULL` to locate it through `VSINSTALLDIR` and vswhere. PE only. */
	const char* symbol_map; /**< Emscripten `--emit-symbol-map` output for a Wasm module with a stripped name section, or `NULL`. */
} cwsym_read_options_t;

/**
 * Read one debug file and push its contents into `sink`.
 *
 * The format is detected by magic. `path` names the executable: on PE the
 * PDB is located beside it through its CodeView record and a GUID/age
 * mismatch is ::CWSYM_ERR_NO_DEBUG; on ELF it may also be the `.debug`
 * file from `objcopy --only-keep-debug`. Functions without DWARF entries
 * fall back to `.symtab`. Names are copied verbatim from the debug info's
 * scope tree; nothing is demangled. Wasm yields no lines or sites.
 *
 * @param opts  Extra inputs, or `NULL` for the defaults.
 */
cwsym_status_t
cwsym_read(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
);

/**
 * Apply the naming rules of ::CWSYM_RULES to one symbol.
 *
 * Pure: splits every component on `::` outside template arguments and
 * parentheses, keeping `operator` names whole; strips template argument
 * lists; maps anonymous namespaces to `$anon` and lambdas to `$lambda`;
 * drops `class`/`struct`/`enum` and `[abi:...]` tags; prefixes a static
 * function with the stem of its unit and `:`; joins with `::`. A flat
 * `ns::Class::method` and the components `ns`, `Class`, `method` yield
 * identical bytes, as do a GCC and an MSVC reading of one function.
 *
 * @param buf  Receives the NUL-terminated normalized name.
 * @param cap  Capacity of `buf`. A name that does not fit is truncated and the call returns `false`.
 * @return `true` when `buf` holds the complete name.
 */
bool
cwsym_normalize(const cwsym_symbol_t* sym, char* buf, size_t cap);

/**
 * Table under construction. Opaque; obtained from @ref cwsym_table_begin
 * and consumed by @ref cwsym_table_end.
 *
 * The first failure of any call on a builder is remembered and reported
 * by @ref cwsym_table_end, so a caller may add without checking and
 * check once.
 */
typedef struct cwsym_table_builder_s cwsym_table_builder_t;

/**
 * Start a table.
 *
 * @return A builder, or `NULL` when out of memory.
 */
cwsym_table_builder_t*
cwsym_table_begin(void);

/**
 * Set the module identity. Required before @ref cwsym_table_end.
 */
void
cwsym_table_set_module(cwsym_table_builder_t* builder, const cwsym_module_t* mod);

/**
 * Normalize `sym` and add one function row.
 *
 * @return ::CWSYM_OK, ::CWSYM_ERR_NOMEM, or ::CWSYM_ERR_INVALID when the name does not fit the normalizer's limit.
 */
cwsym_status_t
cwsym_table_add(cwsym_table_builder_t* builder, const cwsym_symbol_t* sym);

/** Add one line row. Same return contract as @ref cwsym_table_add. */
cwsym_status_t
cwsym_table_add_line(cwsym_table_builder_t* builder, const cwsym_line_t* line);

/** Add one inline-site row. Same return contract as @ref cwsym_table_add. */
cwsym_status_t
cwsym_table_add_site(cwsym_table_builder_t* builder, const cwsym_site_t* site);

/**
 * Sink that forwards to the builder: `begin` sets the module, the item
 * callbacks add rows and stop the reader on the first failure, and a
 * failing status handed to `end` is remembered for @ref cwsym_table_end.
 *
 * @param lines  `false` leaves the `line` and `site` callbacks `NULL`, so the reader skips them.
 */
cwsym_sink_t
cwsym_table_sink(cwsym_table_builder_t* builder, bool lines);

/**
 * Immutable table: one serialized `cwsym` file image plus typed views
 * into it. Produced by @ref cwsym_table_end or @ref cwsym_table_parse;
 * released by @ref cwsym_table_free.
 *
 * Every pointer points into `data`. On a table parsed from a prefix that
 * ends after `strings`, `disp`, `dstrings`, and the line arrays are
 * `NULL`; the line and site arrays are also `NULL` and their counts 0
 * unless ::CWSYM_FLAG_HAS_LINES is set.
 */
typedef struct {
	const void* data;             /**< The file image, little-endian, as written to disk or uploaded. */
	size_t len;
	bool owned;                   /**< `data` was allocated by @ref cwsym_table_end; @ref cwsym_table_free releases it. A parsed table borrows the caller's buffer. */
	uint16_t version;
	uint16_t rules;
	cwsym_arch_t arch;
	uint8_t build_id[CWSYM_BUILD_ID_CAP];
	uint8_t build_id_len;
	uint32_t flags;
	uint32_t count;
	const uint32_t* starts;
	const uint32_t* sizes;
	const uint32_t* names;        /**< Offsets into `strings`. */
	const char* strings;          /**< Normalized names; the Worker's hash input and the end of its prefix. */
	uint32_t strings_len;
	const uint32_t* disp;         /**< Offsets into `dstrings`; 0 means the normalized name. */
	uint32_t line_count;
	const uint32_t* line_starts;
	const uint32_t* line_lens;
	const uint32_t* line_files;   /**< Offsets into `dstrings`. */
	const uint32_t* line_lines;
	uint32_t site_count;
	const uint32_t* site_starts;
	const uint32_t* site_lens;
	const uint32_t* site_callees; /**< Offsets into `dstrings`. */
	const uint32_t* site_files;   /**< Offsets into `dstrings`; 0 when unknown. */
	const uint32_t* site_lines;   /**< 0 when unknown. */
	const uint32_t* site_parents; /**< Row index of the enclosing site, or ::CWSYM_NO_PARENT. */
	const char* dstrings;         /**< Display-only strings: display names, source paths, inlined callee names. Never part of the Worker's prefix. */
	uint32_t dstrings_len;
} cwsym_table_t;

/**
 * Finish a table: sort every section, check the invariants of a `cwsym`
 * file, serialize, and release the builder.
 *
 * Function rows are ordered by `(start, external first, name)`. Rows with
 * the same start are ICF folds; only the first survives. Line rows are
 * ordered by start and must not overlap. Site rows are ordered by
 * `(start, depth)`; each must lie inside a function row and inside its
 * parent, which this call resolves from `depth`. Strings are
 * deduplicated. The hashed sections come first and end with `strings`;
 * the display column, the line sections when any, and `dstrings` follow,
 * so the Worker can read the header and a single prefix. The file is
 * written with `rules` = ::CWSYM_RULES.
 *
 * The builder is freed whatever the outcome. On failure `table` is zeroed.
 *
 * @return ::CWSYM_OK; the first failure remembered by the builder; ::CWSYM_ERR_INVALID for a missing module, an empty function section, any other overlap, or bad nesting, with the offending names logged; ::CWSYM_ERR_NOMEM.
 */
cwsym_status_t
cwsym_table_end(cwsym_table_builder_t* builder, cwsym_table_t* table, const cwsym_log_t* log);

/**
 * Validate a `cwsym` buffer and wrap it without copying.
 *
 * Checks the magic, version, section bounds and alignment, ascending
 * non-overlapping function and line starts, site nesting, and that every
 * string offset lands on a NUL-terminated string inside its section. A
 * buffer that ends after `strings` is accepted as a prefix. Does not
 * check `rules`; the caller compares cwsym_table_t::rules with what it
 * expects. `buf` must outlive the table and be 4-byte aligned, as a
 * `malloc` or `mmap` result is; the sections are read in place, so the
 * host must be little-endian like every target.
 *
 * @return ::CWSYM_OK, ::CWSYM_ERR_FORMAT for a foreign or truncated buffer, ::CWSYM_ERR_INVALID for a broken invariant.
 */
cwsym_status_t
cwsym_table_parse(const void* buf, size_t len, cwsym_table_t* table, const cwsym_log_t* log);

/**
 * Release a table. Frees `data` when the table owns it; a no-op for a
 * parsed table and for a zeroed one.
 */
void
cwsym_table_free(cwsym_table_t* table);

/**
 * Function covering one offset.
 */
typedef struct {
	uint32_t start;
	uint32_t size;
	const char* name; /**< Normalized name, the `grp` input. */
} cwsym_hit_t;

/**
 * Resolve one module-relative offset: the row with the greatest start
 * not above `offset` whose range covers it.
 *
 * This is the search the Worker mirrors, so its behaviour on the edges
 * (`size` 0, offset past the last row) is a contract. Needs only the
 * prefix of the file and reports only what the Worker hashes with; the
 * display name comes from @ref cwsym_symbolize.
 *
 * @return `true` on a hit; `false` when no row covers `offset`.
 */
bool
cwsym_lookup(const cwsym_table_t* table, uint32_t offset, cwsym_hit_t* hit);

/**
 * One source location for a queried offset.
 *
 * An offset inside inlined code yields one call per level, innermost
 * first; the last call names the function @ref cwsym_lookup returns for
 * that offset. Pointers point into the table's `data`.
 */
typedef struct {
	uint32_t offset;      /**< The queried offset. */
	int depth;            /**< 0 for the innermost location, increasing outwards. */
	const char* function; /**< Display name, or `NULL` when no function covers `offset`. */
	const char* file;     /**< Source path, or `NULL` when the table has no line for it. */
	uint32_t line;        /**< 1-based line, or 0 when unknown. */
} cwsym_location_t;

/**
 * Receiver of @ref cwsym_symbolize output.
 */
typedef struct {
	void (*location)(void* user, const cwsym_location_t* loc);
	void* user;
} cwsym_location_sink_t;

/**
 * Resolve one offset to its source locations, innermost first.
 *
 * Pure and platform-independent: the line row covering `offset` gives
 * the innermost file and line, the innermost site covering it gives that
 * location's function, and each parent site contributes its call site
 * as the next level, ending at the function row. This is the search the
 * dashboard mirrors. A table without lines produces one location with
 * the function and no file.
 *
 * @return Number of locations delivered; 0 when no function covers `offset`, after one call with a `NULL` function.
 */
int
cwsym_symbolize(const cwsym_table_t* table, uint32_t offset, const cwsym_location_sink_t* sink);

/**
 * Lowercase hex of the build id, the R2 key and the string the client
 * sends.
 *
 * @param out  Receives at most 2 * ::CWSYM_BUILD_ID_CAP + 1 bytes.
 */
void
cwsym_build_id_hex(const uint8_t* build_id, uint8_t len, char* out);

/**
 * Print the header and every row of every section in a fixed text form
 * for inspection and for tests. The first line is the key the table
 * uploads under.
 */
void
cwsym_dump(const cwsym_table_t* table, FILE* out);

/**
 * What the upload step needs. Every string is required.
 */
typedef struct {
	const char* endpoint; /**< Base URL without a trailing slash. */
	const char* app;      /**< Application slug, as cw_config_t::app. */
	const char* token;    /**< CI bearer token. */
	const char* version;  /**< Release version, as cw_config_t::version. */
	const char* channel;  /**< Release channel, as cw_config_t::channel. */
} cwsym_upload_t;

/**
 * Register the release and upload its table in one request.
 *
 * `PUT`s the table's `data` to `<endpoint>/v1/<app>/releases/<version>
 * ?channel=<channel>` with `Authorization: Bearer <token>`; the server
 * reads the build id from the table header. Idempotent: repeating it for
 * the same version and build is a no-op. The tool passes
 * @ref cw_transport_http; a test may pass any transport. There is no
 * retry: a ::CW_RETRY from the transport or a reply outside 2xx is a
 * failure, with the reply text logged.
 *
 * @return ::CWSYM_OK, ::CWSYM_ERR_INVALID when a required string is
 *         missing or the table is a prefix, ::CWSYM_ERR_IO when the
 *         request failed or was refused.
 */
cwsym_status_t
cwsym_upload(
	const cwsym_upload_t* up, const cwsym_table_t* table,
	const cw_transport_t* transport, const cwsym_log_t* log
);

#endif /* CWSYM_H */
