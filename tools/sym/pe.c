/**
 * @file pe.c
 * PE reader: the PDB beside the executable, through DIA.
 */
#include "reader.h"

cwsym_status_t
cwsym_read_pe(
	const char* path, const cwsym_read_options_t* opts,
	const cwsym_sink_t* sink, const cwsym_log_t* log
) {
	(void)opts;
	(void)sink;
	cwsym_logf(log, "%s: the PE reader is not implemented", path);
	return CWSYM_ERR_UNSUPPORTED;
}
