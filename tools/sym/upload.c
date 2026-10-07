/**
 * @file upload.c
 * The release request: one `PUT` of the table through a `cw_transport_t`.
 */
#include <stdio.h>
#include <string.h>

#include "reader.h"

#define REPLY_CAP 4096

/**
 * Percent-encode `s` for a path segment or query value: unreserved
 * characters pass, everything else becomes `%XX`.
 *
 * @return `false` when `out` is too small.
 */
static bool
encode(const char* s, char* out, size_t cap) {
	static const char hex[] = "0123456789ABCDEF";
	size_t at = 0;
	for (; *s != '\0'; ++s) {
		unsigned char c = (unsigned char)*s;
		bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '.' || c == '_' || c == '~';
		size_t need = plain ? 1 : 3;
		if (at + need >= cap) {
			return false;
		}
		if (plain) {
			out[at++] = (char)c;
		} else {
			out[at++] = '%';
			out[at++] = hex[c >> 4];
			out[at++] = hex[c & 0xf];
		}
	}
	out[at] = '\0';
	return true;
}

cwsym_status_t
cwsym_upload(
	const cwsym_upload_t* up, const cwsym_table_t* table,
	const cw_transport_t* transport, const cwsym_log_t* log
) {
	static const struct {
		const char* name;
		size_t offset;
	} required[] = {
		{ "endpoint", offsetof(cwsym_upload_t, endpoint) },
		{ "app", offsetof(cwsym_upload_t, app) },
		{ "token", offsetof(cwsym_upload_t, token) },
		{ "version", offsetof(cwsym_upload_t, version) },
		{ "channel", offsetof(cwsym_upload_t, channel) },
	};
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
		const char* value = *(const char* const*)((const char*)up + required[i].offset);
		if (value == NULL || value[0] == '\0') {
			cwsym_logf(log, "upload: %s is required", required[i].name);
			return CWSYM_ERR_INVALID;
		}
	}
	if (table->data == NULL || table->dstrings == NULL) {
		cwsym_logf(log, "upload: not a complete table");
		return CWSYM_ERR_INVALID;
	}
	if (transport == NULL || transport->send == NULL) {
		cwsym_logf(log, "upload: no transport");
		return CWSYM_ERR_INVALID;
	}

	char app[256];
	char version[256];
	char channel[256];
	char commit[512];
	char source_root[2048];
	char url[4096];
	if (!encode(up->app, app, sizeof(app)) || !encode(up->version, version, sizeof(version))
		|| !encode(up->channel, channel, sizeof(channel))
		|| !encode(up->commit != NULL ? up->commit : "", commit, sizeof(commit))
		|| !encode(up->source_root != NULL ? up->source_root : "", source_root, sizeof(source_root))) {
		cwsym_logf(log, "upload: app, version, channel, commit, or source root is too long");
		return CWSYM_ERR_INVALID;
	}
	int n = snprintf(
		url, sizeof(url), "%s/v1/%s/releases/%s?channel=%s%s%s%s%s", up->endpoint, app, version, channel,
		up->commit != NULL ? "&commit=" : "", commit,
		up->source_root != NULL ? "&source_root=" : "", source_root
	);
	if (n < 0 || (size_t)n >= sizeof(url)) {
		cwsym_logf(log, "upload: the URL is too long");
		return CWSYM_ERR_INVALID;
	}

	char reply[REPLY_CAP];
	cw_request_t req = {
		.method = "PUT",
		.url = url,
		.content_type = "application/octet-stream",
		.token = up->token,
		.body = table->data,
		.body_len = table->len,
		.reply = reply,
		.reply_cap = sizeof(reply) - 1,
	};
	cw_response_t resp = { 0 };
	cw_status_t status = transport->send(transport->user, &req, &resp);
	if (status == CW_RETRY) {
		cwsym_logf(log, "PUT %s: no reply; the network, name resolution, or TLS failed", url);
		return CWSYM_ERR_IO;
	}
	if (status != CW_OK) {
		cwsym_logf(log, "PUT %s: the request cannot be sent", url);
		return CWSYM_ERR_IO;
	}
	reply[resp.reply_len] = '\0';
	for (size_t i = resp.reply_len; i > 0 && (reply[i - 1] == '\n' || reply[i - 1] == '\r'); --i) {
		reply[i - 1] = '\0';
	}
	if (resp.status < 200 || resp.status >= 300) {
		cwsym_logf(log, "PUT %s: HTTP %d%s%s", url, resp.status, reply[0] != '\0' ? ": " : "", reply);
		return CWSYM_ERR_IO;
	}
	char id[2 * CWSYM_BUILD_ID_CAP + 1];
	cwsym_build_id_hex(table->build_id, table->build_id_len, id);
	cwsym_logf(log, "uploaded %s as %s (%s): HTTP %d", id, up->version, up->channel, resp.status);
	return CWSYM_OK;
}
