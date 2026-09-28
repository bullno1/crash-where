/**
 * @file upload.c
 * Sends one written report through the configured transport.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/** Largest reply body the protocol ever needs to read. */
#define CW_REPLY_CAP 4096

/**
 * Read a whole file into a NUL-terminated heap buffer.
 *
 * @return The buffer, or `NULL` when the file cannot be read.
 */
static char*
read_file(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	char* buf = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long size = ftell(f);
		if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
			buf = malloc((size_t)size + 1);
			if (buf != NULL) {
				*len = fread(buf, 1, (size_t)size, f);
				buf[*len] = '\0';
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

/**
 * Map an HTTP status onto what happens to the report.
 *
 * Redirects count as rejections: they are never followed, and one means
 * the compiled-in endpoint is wrong, which retrying cannot fix.
 */
static cw_status_t
map_status(int status) {
	if (status >= 200 && status < 300) {
		return CW_OK;
	}
	if (status == 0 || status == 429 || status >= 500) {
		return CW_RETRY;
	}
	return CW_DROP;
}

/**
 * Whether a JSON reply sets `key` to `true`.
 *
 * The reply is ours and tiny, so a scan for the quoted key and the
 * literal after its colon stands in for a parser.
 */
static bool
reply_flag(const char* reply, const char* key) {
	char quoted[64];
	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	const char* p = strstr(reply, quoted);
	if (p == NULL) {
		return false;
	}
	p += strlen(quoted);
	p += strspn(p, " \t\r\n");
	if (*p != ':') {
		return false;
	}
	++p;
	p += strspn(p, " \t\r\n");
	return strncmp(p, "true", 4) == 0;
}

void
cw_upload_report(const char* path) {
	const cw_transport_t* tr = cw_ctx.cfg.transport;
	if (tr == NULL) {
		cw_log(CW_LOG_WARN, "no transport configured, report kept at %s", path);
		return;
	}

	char id[37];
	if (!path_id(path, id)) {
		cw_log(CW_LOG_ERROR, "unexpected report name %s", path);
		return;
	}
	size_t len = 0;
	char* json = read_file(path, &len);
	if (json == NULL) {
		cw_log(CW_LOG_ERROR, "cannot read %s back for upload", path);
		return;
	}

	char url[CW_STR_CAP + 128];
	snprintf(url, sizeof(url), "%s/v1/%s/report", cw_ctx.cfg.endpoint, cw_ctx.cfg.app);
	char reply[CW_REPLY_CAP];
	cw_request_t req = {
		.method = "POST",
		.url = url,
		.content_type = "application/json",
		.token = NULL,
		.body = json,
		.body_len = len,
		.reply = reply,
		.reply_cap = sizeof(reply) - 1,
	};
	cw_response_t resp = { 0 };
	cw_status_t status = tr->send(tr->user, &req, &resp);
	free(json);

	bool want_attachments = false;
	if (status == CW_OK) {
		if (resp.reply_len > req.reply_cap) {
			resp.reply_len = req.reply_cap;
		}
		reply[resp.reply_len] = '\0';
		status = map_status(resp.status);
		if (status != CW_OK) {
			cw_log(CW_LOG_DEBUG, "server answered %d for report %s", resp.status, id);
		}
		want_attachments = status == CW_OK && reply_flag(reply, "want_attachments");
	}
	/* The prototype writes no attachments yet. */
	if (want_attachments) {
		cw_log(CW_LOG_DEBUG, "server wants attachments for report %s, none written", id);
	}

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
