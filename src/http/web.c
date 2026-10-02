/**
 * @file http/web.c
 * Web backend: `fetch` in an instance that can wait for it, a
 * synchronous request in one that cannot.
 */
#include "backend.h"
#include "internal.h"

/**
 * How cw_http_web_fetch() ended.
 */
typedef enum {
	CW_HTTP_WEB_RESPONDED   = 0, /**< A reply arrived; `status` and `reply_len` are set. */
	CW_HTTP_WEB_FAILED      = 1, /**< The network failed, or the browser refused the reply. */
	CW_HTTP_WEB_INVALID_URL = 2, /**< The URL can never be fetched. */
	CW_HTTP_WEB_UNSUPPORTED = 3, /**< No way to send a request here; nothing was sent. */
} cw_http_web_result_t;

/**
 * Shim: perform one request and wait for the reply.
 *
 * Suspends the caller where the instance was set up for it. Elsewhere
 * it blocks on a synchronous request, which follows redirects and
 * reports one only afterwards.
 *
 * @param content_type  `NULL` for none.
 * @param token         `NULL` for none.
 * @return A cw_http_web_result_t.
 */
int
cw_http_web_fetch(
	const char* method, const char* url,
	const char* content_type, const char* token,
	const void* body, size_t body_len,
	char* reply, size_t reply_cap,
	long timeout_ms, int* status, size_t* reply_len
);

cw_status_t
cw_http_backend_send(const cw_request_t* req, cw_response_t* resp) {
	int status = 0;
	size_t reply_len = 0;
	int result = cw_http_web_fetch(
		req->method, req->url,
		req->content_type, req->token,
		req->body, req->body_len,
		req->reply, req->reply_cap,
		CW_HTTP_TIMEOUT_MS, &status, &reply_len
	);
	switch (result) {
	case CW_HTTP_WEB_RESPONDED:
		*resp = (cw_response_t){ .status = status, .reply_len = reply_len };
		return CW_OK;
	case CW_HTTP_WEB_INVALID_URL:
		return CW_DROP;
	case CW_HTTP_WEB_UNSUPPORTED:
		cw_log(CW_LOG_WARN, "no way to send a request here, nothing sent");
		return CW_RETRY;
	default:
		return CW_RETRY;
	}
}
