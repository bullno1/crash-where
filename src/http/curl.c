/**
 * @file curl.c
 * libcurl backend for Linux and macOS.
 *
 * The system libcurl is loaded on first use and never linked. The header
 * supplies the types and option numbers; every entry point is looked up
 * by name and stored with the type of its declaration, so a signature is
 * never retyped here.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "backend.h"
#include "internal.h"

/** The loaded library and its entry points; loading is attempted once. */
static struct {
	void* lib;
	bool tried;
	__typeof__(curl_easy_init)* easy_init;
	__typeof__(curl_easy_setopt)* easy_setopt;
	__typeof__(curl_easy_perform)* easy_perform;
	__typeof__(curl_easy_getinfo)* easy_getinfo;
	__typeof__(curl_easy_cleanup)* easy_cleanup;
	__typeof__(curl_easy_strerror)* easy_strerror;
	__typeof__(curl_slist_append)* slist_append;
	__typeof__(curl_slist_free_all)* slist_free_all;
} curl;

/** Resolve `curl_<NAME>` into the member `NAME`; false when it is missing. */
#define CURL_SYM(NAME) \
	((curl.NAME = (__typeof__(curl_##NAME)*)dlsym(curl.lib, "curl_" #NAME)) != NULL)

static bool
load(void) {
	if (curl.tried) {
		return curl.lib != NULL;
	}
	curl.tried = true;

	static const char* const names[] = {
		"libcurl.so.4",
		"libcurl-gnutls.so.4",
		"libcurl.4.dylib",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && curl.lib == NULL; ++i) {
		curl.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (curl.lib == NULL) {
		cw_log(CW_LOG_WARN, "libcurl not found, uploads deferred: %s", dlerror());
		return false;
	}

	bool ok = CURL_SYM(easy_init)
		&& CURL_SYM(easy_setopt)
		&& CURL_SYM(easy_perform)
		&& CURL_SYM(easy_getinfo)
		&& CURL_SYM(easy_cleanup)
		&& CURL_SYM(easy_strerror)
		&& CURL_SYM(slist_append)
		&& CURL_SYM(slist_free_all);
	if (!ok) {
		cw_log(CW_LOG_ERROR, "libcurl is missing a required symbol: %s", dlerror());
		dlclose(curl.lib);
		curl.lib = NULL;
		return false;
	}
	return true;
}

/** Destination of the reply body. */
typedef struct {
	char* buf;
	size_t cap;
	size_t len;
} reply_t;

/**
 * Keep the first `cap` bytes; anything after that is consumed and
 * dropped so the transfer still completes and the status is known.
 */
static size_t
on_reply(char* data, size_t size, size_t nmemb, void* user) {
	reply_t* reply = user;
	size_t n = size * nmemb;
	size_t room = reply->cap - reply->len;
	size_t take = n < room ? n : room;
	memcpy(reply->buf + reply->len, data, take);
	reply->len += take;
	return n;
}

/**
 * Append `<name>: <value>` to `list`.
 *
 * @return `false` when out of memory; `list` is unchanged.
 */
static bool
add_header(struct curl_slist** list, const char* name, const char* value) {
	size_t len = strlen(name) + 2 + strlen(value) + 1;
	char* line = malloc(len);
	if (line == NULL) {
		return false;
	}
	snprintf(line, len, "%s: %s", name, value);
	struct curl_slist* next = curl.slist_append(*list, line);
	free(line);
	if (next == NULL) {
		return false;
	}
	*list = next;
	return true;
}

/** A URL that libcurl cannot even parse will never succeed. */
static cw_status_t
map_error(CURLcode rc) {
	switch (rc) {
	case CURLE_URL_MALFORMAT:
	case CURLE_UNSUPPORTED_PROTOCOL:
		return CW_DROP;
	default:
		return CW_RETRY;
	}
}

cw_status_t
cw_http_backend_send(const cw_request_t* req, cw_response_t* resp) {
	if (!load()) {
		return CW_RETRY;
	}
	CURL* h = curl.easy_init();
	if (h == NULL) {
		cw_log(CW_LOG_ERROR, "curl_easy_init failed");
		return CW_RETRY;
	}

	cw_status_t status = CW_RETRY;
	struct curl_slist* headers = NULL;
	/* Suppress `Expect: 100-continue`, which curl adds to larger bodies. */
	bool ok = add_header(&headers, "Expect", "");
	if (req->content_type != NULL) {
		ok = ok && add_header(&headers, "Content-Type", req->content_type);
	}
	if (req->token != NULL) {
		size_t len = strlen(req->token) + sizeof("Bearer ");
		char* bearer = malloc(len);
		ok = ok && bearer != NULL;
		if (ok) {
			snprintf(bearer, len, "Bearer %s", req->token);
			ok = add_header(&headers, "Authorization", bearer);
		}
		free(bearer);
	}
	if (!ok) {
		cw_log(CW_LOG_ERROR, "out of memory building request headers");
		goto end;
	}

	reply_t reply = { .buf = req->reply, .cap = req->reply_cap };
	CURLcode rc = CURLE_OK;
#define SETOPT(OPT, VAL) do { if (rc == CURLE_OK) { rc = curl.easy_setopt(h, OPT, VAL); } } while (0)
	SETOPT(CURLOPT_URL, req->url);
	SETOPT(CURLOPT_NOSIGNAL, 1L);
	SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
	SETOPT(CURLOPT_USERAGENT, CW_HTTP_USER_AGENT);
	SETOPT(CURLOPT_CONNECTTIMEOUT_MS, CW_HTTP_CONNECT_TIMEOUT_MS);
	SETOPT(CURLOPT_TIMEOUT_MS, CW_HTTP_TIMEOUT_MS);
	SETOPT(CURLOPT_HTTPHEADER, headers);
	SETOPT(CURLOPT_WRITEFUNCTION, on_reply);
	SETOPT(CURLOPT_WRITEDATA, &reply);
	if (req->body != NULL) {
		SETOPT(CURLOPT_POSTFIELDS, req->body);
		SETOPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
	} else {
		SETOPT(CURLOPT_HTTPGET, 1L);
	}
	SETOPT(CURLOPT_CUSTOMREQUEST, req->method);
#undef SETOPT
	if (rc != CURLE_OK) {
		cw_log(CW_LOG_ERROR, "curl_easy_setopt: %s", curl.easy_strerror(rc));
		goto end;
	}

	rc = curl.easy_perform(h);
	if (rc != CURLE_OK) {
		cw_log(CW_LOG_WARN, "%s %s: %s", req->method, req->url, curl.easy_strerror(rc));
		status = map_error(rc);
		goto end;
	}
	long code = 0;
	curl.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
	resp->status = (int)code;
	resp->reply_len = reply.len;
	status = CW_OK;

end:
	curl.slist_free_all(headers);
	curl.easy_cleanup(h);
	return status;
}
