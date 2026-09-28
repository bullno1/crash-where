/**
 * @file read.c
 * Format detection by magic, then dispatch to the matching reader.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "reader.h"

void
cwsym_logf(const cwsym_log_t* log, const char* fmt, ...) {
	if (log == NULL || log->log == NULL) {
		return;
	}
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log->log(log->user, buf);
}

cwsym_status_t
cwsym_read(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	static const cwsym_read_options_t defaults = { 0 };
	if (opts == NULL) {
		opts = &defaults;
	}

	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		cwsym_logf(log, "%s: %s", path, strerror(errno));
		return CWSYM_ERR_IO;
	}
	unsigned char magic[8] = { 0 };
	size_t n = fread(magic, 1, sizeof(magic), f);
	fclose(f);

	if (n >= 4 && memcmp(magic, "\x7f" "ELF", 4) == 0) {
		return cwsym_read_elf(path, opts, sink, log);
	} else if (n >= 2 && memcmp(magic, "MZ", 2) == 0) {
		return cwsym_read_pe(path, opts, sink, log);
	} else if (n >= 4 && memcmp(magic, "\0asm", 4) == 0) {
		return cwsym_read_wasm(path, opts, sink, log);
	} else if (n >= 6 && memcmp(magic, "CWSYM\0", 6) == 0) {
		cwsym_logf(log, "%s: is a cwsym table, not a debug file", path);
		return CWSYM_ERR_FORMAT;
	} else {
		cwsym_logf(log, "%s: not a PE, ELF, or Wasm file", path);
		return CWSYM_ERR_FORMAT;
	}
}
