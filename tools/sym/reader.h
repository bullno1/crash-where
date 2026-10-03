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

#endif /* CWSYM_READER_H */
