/**
 * @file wasm.c
 * Wasm reader: function bodies from the code section, names from the
 * name section or the Emscripten symbol map.
 */
#include "reader.h"

cwsym_status_t
cwsym_read_wasm(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	(void)opts;
	(void)sink;
	cwsym_logf(log, "%s: the Wasm reader is not implemented", path);
	return CWSYM_ERR_UNSUPPORTED;
}
