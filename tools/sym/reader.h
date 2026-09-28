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

#endif /* CWSYM_READER_H */
