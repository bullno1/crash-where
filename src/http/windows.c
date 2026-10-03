/**
 * @file windows.c
 * WinHTTP backend.
 *
 * winhttp.dll is delay-loaded, so the game never maps it; the first call
 * in the watcher does.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "internal.h"

/** UTF-16 copy of a UTF-8 string, on the heap; `NULL` when out of memory. */
static wchar_t*
widen(const char* s) {
	int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (len <= 0) {
		return NULL;
	}
	wchar_t* out = malloc((size_t)len * sizeof(wchar_t));
	if (out != NULL) {
		MultiByteToWideChar(CP_UTF8, 0, s, -1, out, len);
	}
	return out;
}

/** Log the last WinHTTP error for `req` at `level`. */
static void
log_error(cw_log_level_t level, const cw_request_t* req, const char* what) {
	DWORD err = GetLastError();
	wchar_t msg[256] = L"";
	FormatMessageW(
		FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		GetModuleHandleW(L"winhttp.dll"), err, 0, msg, sizeof(msg) / sizeof(msg[0]), NULL
	);
	size_t len = wcslen(msg);
	while (len > 0 && (msg[len - 1] == L'\r' || msg[len - 1] == L'\n' || msg[len - 1] == L'.')) {
		msg[--len] = L'\0';
	}
	if (len > 0) {
		cw_log(level, "%s %s: %s failed: %ls (%lu)", req->method, req->url, what, msg, (unsigned long)err);
	} else {
		cw_log(level, "%s %s: %s failed with error %lu", req->method, req->url, what, (unsigned long)err);
	}
}

/** A URL WinHTTP cannot parse or a scheme it does not speak will never work. */
static cw_status_t
map_error(DWORD err) {
	switch (err) {
	case ERROR_WINHTTP_INVALID_URL:
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
	case ERROR_INSUFFICIENT_BUFFER:
		return CW_DROP;
	default:
		return CW_RETRY;
	}
}

/**
 * Read the body: the first `cap` bytes into `reply`, the rest into a
 * scratch buffer so the connection completes cleanly.
 */
static void
read_reply(HINTERNET request, const cw_request_t* req, cw_response_t* resp) {
	char drain[4096];
	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(request, &avail)) {
			log_error(CW_LOG_WARN, req, "WinHttpQueryDataAvailable");
			return;
		}
		if (avail == 0) {
			return;
		}
		char* dst = drain;
		DWORD want = avail < sizeof(drain) ? avail : sizeof(drain);
		size_t room = req->reply_cap - resp->reply_len;
		if (room > 0) {
			dst = req->reply + resp->reply_len;
			want = avail < room ? avail : (DWORD)room;
		}
		DWORD got = 0;
		if (!WinHttpReadData(request, dst, want, &got)) {
			log_error(CW_LOG_WARN, req, "WinHttpReadData");
			return;
		}
		if (got == 0) {
			return;
		}
		if (dst != drain) {
			resp->reply_len += got;
		}
	}
}

/** Build the `Name: value\r\n` block WinHTTP takes as extra headers. */
static wchar_t*
build_headers(const cw_request_t* req) {
	size_t len = 1;
	if (req->content_type != NULL) {
		len += sizeof("Content-Type: \r\n") + strlen(req->content_type);
	}
	if (req->content_encoding != NULL) {
		len += sizeof("Content-Encoding: \r\n") + strlen(req->content_encoding);
	}
	if (req->token != NULL) {
		len += sizeof("Authorization: Bearer \r\n") + strlen(req->token);
	}
	char* utf8 = malloc(len);
	if (utf8 == NULL) {
		return NULL;
	}
	utf8[0] = '\0';
	if (req->content_type != NULL) {
		snprintf(utf8 + strlen(utf8), len - strlen(utf8), "Content-Type: %s\r\n", req->content_type);
	}
	if (req->content_encoding != NULL) {
		snprintf(utf8 + strlen(utf8), len - strlen(utf8), "Content-Encoding: %s\r\n", req->content_encoding);
	}
	if (req->token != NULL) {
		snprintf(utf8 + strlen(utf8), len - strlen(utf8), "Authorization: Bearer %s\r\n", req->token);
	}
	wchar_t* wide = widen(utf8);
	free(utf8);
	return wide;
}

cw_status_t
cw_http_backend_send(const cw_request_t* req, cw_response_t* resp) {
	if (req->body_len > MAXDWORD) {
		cw_log(CW_LOG_ERROR, "%s %s: body of %zu bytes is too large", req->method, req->url, req->body_len);
		return CW_DROP;
	}

	cw_status_t status = CW_RETRY;
	HINTERNET session = NULL;
	HINTERNET connection = NULL;
	HINTERNET request = NULL;
	wchar_t* method = NULL;
	wchar_t* headers = NULL;
	wchar_t* url = widen(req->url);
	if (url == NULL) {
		cw_log(CW_LOG_ERROR, "out of memory building request");
		goto end;
	}

	wchar_t host[256];
	wchar_t path[2048];
	wchar_t extra[2048];
	URL_COMPONENTS parts = {
		.dwStructSize = sizeof(parts),
		.lpszHostName = host, .dwHostNameLength = sizeof(host) / sizeof(host[0]),
		.lpszUrlPath = path, .dwUrlPathLength = sizeof(path) / sizeof(path[0]),
		.lpszExtraInfo = extra, .dwExtraInfoLength = sizeof(extra) / sizeof(extra[0]),
	};
	if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
		status = map_error(GetLastError());
		log_error(CW_LOG_WARN, req, "WinHttpCrackUrl");
		goto end;
	}
	if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
		cw_log(CW_LOG_WARN, "%s %s: not an http(s) URL", req->method, req->url);
		status = CW_DROP;
		goto end;
	}
	if (wcslen(path) + wcslen(extra) >= sizeof(path) / sizeof(path[0])) {
		cw_log(CW_LOG_WARN, "%s %s: URL too long", req->method, req->url);
		status = CW_DROP;
		goto end;
	}
	wcscat(path, extra);

	session = WinHttpOpen(
		L"" CW_HTTP_USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0
	);
	if (session == NULL) {
		/* Automatic proxy discovery needs Windows 8.1; older systems take the default proxy. */
		session = WinHttpOpen(
			L"" CW_HTTP_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0
		);
	}
	if (session == NULL) {
		log_error(CW_LOG_ERROR, req, "WinHttpOpen");
		goto end;
	}
	WinHttpSetTimeouts(
		session,
		CW_HTTP_CONNECT_TIMEOUT_MS, CW_HTTP_CONNECT_TIMEOUT_MS,
		CW_HTTP_TIMEOUT_MS, CW_HTTP_TIMEOUT_MS
	);

	connection = WinHttpConnect(session, host, parts.nPort, 0);
	if (connection == NULL) {
		log_error(CW_LOG_WARN, req, "WinHttpConnect");
		goto end;
	}

	method = widen(req->method);
	headers = build_headers(req);
	if (method == NULL || headers == NULL) {
		cw_log(CW_LOG_ERROR, "out of memory building request");
		goto end;
	}
	DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
	request = WinHttpOpenRequest(
		connection, method, path, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
	);
	if (request == NULL) {
		log_error(CW_LOG_WARN, req, "WinHttpOpenRequest");
		goto end;
	}
	DWORD disable = WINHTTP_DISABLE_REDIRECTS;
	if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) {
		log_error(CW_LOG_ERROR, req, "WinHttpSetOption");
		goto end;
	}

	DWORD body_len = (DWORD)req->body_len;
	/* The body pointer is only read during this call; WinHTTP takes it non-const. */
	LPVOID body = req->body != NULL ? (LPVOID)req->body : WINHTTP_NO_REQUEST_DATA;
	if (!WinHttpSendRequest(request, headers, (DWORD)-1L, body, body_len, body_len, 0)) {
		status = map_error(GetLastError());
		log_error(CW_LOG_WARN, req, "WinHttpSendRequest");
		goto end;
	}
	if (!WinHttpReceiveResponse(request, NULL)) {
		log_error(CW_LOG_WARN, req, "WinHttpReceiveResponse");
		goto end;
	}
	DWORD code = 0;
	DWORD size = sizeof(code);
	if (!WinHttpQueryHeaders(
		request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX
	)) {
		log_error(CW_LOG_WARN, req, "WinHttpQueryHeaders");
		goto end;
	}
	resp->status = (int)code;
	read_reply(request, req, resp);
	status = CW_OK;

end:
	if (request != NULL) {
		WinHttpCloseHandle(request);
	}
	if (connection != NULL) {
		WinHttpCloseHandle(connection);
	}
	if (session != NULL) {
		WinHttpCloseHandle(session);
	}
	free(headers);
	free(method);
	free(url);
	return status;
}
