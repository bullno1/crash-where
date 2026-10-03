/**
 * @file reader.h
 * Shared by the format readers, the detector that dispatches to them,
 * and the table code.
 */
#ifndef CWSYM_READER_H
#define CWSYM_READER_H

#include "sym.h"

/**
 * Format one message for `log`. Nothing happens when `log` is `NULL`.
 */
void
cwsym_logf(const cwsym_log_t* log, const char* fmt, ...);

/**
 * One reader per container format, all with the contract of
 * @ref cwsym_read. Each is called only after the detector has matched
 * the file's magic, so a reader never sees another format's file.
 */
cwsym_status_t
cwsym_read_elf(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
);

cwsym_status_t
cwsym_read_pe(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
);

cwsym_status_t
cwsym_read_wasm(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
);

/** One DWARF section lifted out of a container that is not ELF. */
typedef struct {
	const char* name; /**< As DWARF names it: `.debug_info`, `.debug_line`, and so on. */
	const void* data;
	size_t len;
} cwsym_dwarf_section_t;

/**
 * Deliver the functions, lines and inline sites of DWARF kept outside
 * an ELF file. The sections are wrapped in an ELF image for libdw, and
 * `shift` is added to every DWARF address to reach the client's offset.
 * Rows go to `sink` without `begin` or `end`; a callback returning
 * `false` stops the walk, which is still ::CWSYM_OK. `path` is only
 * named in messages.
 *
 * @return ::CWSYM_OK; ::CWSYM_ERR_UNSUPPORTED when libdw cannot be loaded; ::CWSYM_ERR_FORMAT when libdw rejects the sections; ::CWSYM_ERR_NOMEM.
 */
cwsym_status_t
cwsym_read_dwarf(
	const char* path, const cwsym_dwarf_section_t* sections, int count, int64_t shift,
	const cwsym_sink_t* sink, const cwsym_log_t* log
);

/**
 * One segment of a source map: from `offset` on, the code came from
 * `file:line`, until the next segment.
 */
typedef struct {
	uint32_t offset;  /**< File offset in the module. */
	const char* file; /**< Source path, or `NULL` for a segment that maps to nothing. */
	uint32_t line;    /**< 1-based; 0 when `file` is `NULL`. */
} cwsym_map_segment_t;

/**
 * Receiver of a source map's segments, in ascending offset.
 */
typedef struct {
	/** Return `false` to stop. Pointers are valid until `end`. */
	bool (*segment)(void* user, const cwsym_map_segment_t* seg);
	/** Called once after the last segment, also after a stop; not when the map was rejected. May be `NULL`. */
	void (*end)(void* user);
	void* user;
} cwsym_map_sink_t;

/**
 * Read the source map Emscripten writes with `-gsource-map`, whose
 * columns are file offsets of the module. A `debugId` in the map must
 * spell `build_id` in hex. A segment out of order is skipped and
 * counted in a log line.
 *
 * @return ::CWSYM_OK; ::CWSYM_ERR_IO; ::CWSYM_ERR_FORMAT for a file that is not such a map; ::CWSYM_ERR_NO_DEBUG for another build's; ::CWSYM_ERR_NOMEM.
 */
cwsym_status_t
cwsym_read_source_map(
	const char* path, const uint8_t* build_id, size_t build_id_len,
	const cwsym_map_sink_t* sink, const cwsym_log_t* log
);

#endif /* CWSYM_READER_H */
