/**
 * @file upload.c
 * Hands one written report to the configured uploader.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/**
 * Read a whole file into a NUL-terminated heap buffer.
 *
 * @return The buffer, or `NULL` when the file cannot be read.
 */
static char*
read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char* buf = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long len = ftell(f);
		if (len >= 0 && fseek(f, 0, SEEK_SET) == 0) {
			buf = malloc((size_t)len + 1);
			if (buf != NULL) {
				size_t n = fread(buf, 1, (size_t)len, f);
				buf[n] = '\0';
			}
		}
	}
	fclose(f);
	return buf;
}

/**
 * Take the report id from an envelope path, `<ts>_<fp>_<id>.json`.
 */
static bool
path_id(const char* path, char out[37]) {
	static const char ext[] = ".json";
	const char* end = strrchr(path, '.');
	if (end == NULL || strcmp(end, ext) != 0 || end - path < 37 || end[-37] != '_') {
		return false;
	}
	memcpy(out, end - 36, 36);
	out[36] = '\0';
	return true;
}

void
cw_upload_report(const char* path) {
	const cw_uploader_t* up = cw_ctx.cfg.uploader;
	if (up == NULL) {
		cw_log(CW_LOG_WARN, "no uploader configured, report kept at %s", path);
		return;
	}

	char id[37];
	if (!path_id(path, id)) {
		cw_log(CW_LOG_ERROR, "unexpected report name %s", path);
		return;
	}
	char* json = read_file(path);
	if (json == NULL) {
		cw_log(CW_LOG_ERROR, "cannot read %s back for upload", path);
		return;
	}

	/* The prototype writes no attachments yet. */
	const char* const attachments[] = { NULL };
	cw_report_t report = {
		.id = id,
		.envelope_json = json,
		.token = NULL,
		.attachments = attachments,
		.attempts = 0,
	};
	bool want_attachments = false;
	cw_status_t status = up->send_envelope(up->user, &report, &want_attachments);
	for (size_t i = 0; status == CW_OK && want_attachments && attachments[i] != NULL; ++i) {
		status = up->send_attachment(up->user, &report, attachments[i]);
	}
	free(json);

	switch (status) {
	case CW_OK:
		cw_log(CW_LOG_INFO, "report %s uploaded", id);
		remove(path);
		break;
	case CW_RETRY:
		cw_log(CW_LOG_WARN, "upload of report %s failed, kept at %s", id, path);
		break;
	case CW_DROP:
		cw_log(CW_LOG_WARN, "report %s rejected, deleted", id);
		remove(path);
		break;
	}
}
