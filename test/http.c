/**
 * @file http.c
 * The cw_http transport against the loopback server.
 */
#include <stdio.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "cw_http.h"
#include "http/backend.h"
#include "http_server.h"
#include "internal.h"
#include "scenario.h"

static test_http_server_t* server;
static char reply_buf[16384];

static void
http_init(void) {
	cw_ctx.cfg.log = test_cw_log;
}

static void
http_cleanup(void) {
	test_http_stop(server);
	server = NULL;
	cw_ctx.cfg.log = NULL;
}

static btest_suite_t http = {
	.name = "http",
	.init_per_test = http_init,
	.cleanup_per_test = http_cleanup,
};

/**
 * Send `req` to `url`, receiving into `reply_buf`, which is NUL-terminated afterwards.
 *
 * A zero `reply_cap` means the whole buffer.
 */
static cw_status_t
send_to(const char* url, cw_request_t req, cw_response_t* resp) {
	req.url = url;
	req.reply = reply_buf;
	if (req.reply_cap == 0) {
		req.reply_cap = sizeof(reply_buf) - 1;
	}
	*resp = (cw_response_t){ 0 };
	cw_status_t status = cw_transport_http.send(cw_transport_http.user, &req, resp);
	reply_buf[resp->reply_len] = '\0';
	return status;
}

/** Send `req` to `path` on the running server. */
static cw_status_t
send_path(const char* path, cw_request_t req, cw_response_t* resp) {
	char url[128];
	snprintf(url, sizeof(url), "%s%s", test_http_url(server), path);
	return send_to(url, req, resp);
}

BTEST(http, post_carries_body_and_headers) {
	static const char reply[] = "want_attachments 1\n";
	server = test_http_start(&(test_http_reply_t){
		.status = 200, .body = reply, .body_len = sizeof(reply) - 1,
	});
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/report", (cw_request_t){
		.method = "POST",
		.content_type = "application/json",
		.content_encoding = "gzip",
		.token = "tok-123",
		.body = "{\"a\":1}",
		.body_len = 7,
	}, &resp);
	BTEST_ASSERT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 200);
	BTEST_EXPECT_EQUAL("%zu", resp.reply_len, sizeof(reply) - 1);
	BTEST_EXPECT(strcmp(reply_buf, reply) == 0);

	BTEST_ASSERT_EQUAL("%d", test_http_count(server), 1);
	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_EXPECT(strcmp(req->method, "POST") == 0);
	BTEST_EXPECT(strcmp(req->uri, "/v1/game/report") == 0);
	BTEST_EXPECT(strcmp(req->content_type, "application/json") == 0);
	BTEST_EXPECT(strcmp(req->content_encoding, "gzip") == 0);
	BTEST_EXPECT(strcmp(req->content_length, "7") == 0);
	BTEST_EXPECT(strcmp(req->authorization, "Bearer tok-123") == 0);
	if (TEST_HTTP_OWN_USER_AGENT) {
		BTEST_EXPECT(strcmp(req->user_agent, CW_HTTP_USER_AGENT) == 0);
	}
	BTEST_EXPECT(strcmp(req->expect, "") == 0);
	BTEST_EXPECT_EQUAL("%zu", req->body_len, 7);
	BTEST_EXPECT(memcmp(req->body, "{\"a\":1}", 7) == 0);
}

BTEST(http, large_body_arrives_intact) {
	static char body[40000];
	for (size_t i = 0; i < sizeof(body); ++i) {
		body[i] = (char)('a' + i % 26);
	}
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/attach", (cw_request_t){
		.method = "POST",
		.content_type = "application/octet-stream",
		.body = body,
		.body_len = sizeof(body),
	}, &resp);
	BTEST_ASSERT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 200);
	BTEST_EXPECT_EQUAL("%zu", resp.reply_len, 0);

	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_ASSERT(req != NULL);
	BTEST_EXPECT(strcmp(req->content_length, "40000") == 0);
	BTEST_EXPECT(strcmp(req->content_encoding, "") == 0);
	BTEST_EXPECT(strcmp(req->expect, "") == 0);
	BTEST_ASSERT_EQUAL("%zu", req->body_len, sizeof(body));
	BTEST_EXPECT(memcmp(req->body, body, sizeof(body)) == 0);
}

BTEST(http, get_sends_no_body) {
	server = test_http_start(&(test_http_reply_t){ .status = 204 });
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/ping", (cw_request_t){ .method = "GET" }, &resp);
	BTEST_ASSERT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 204);
	BTEST_EXPECT_EQUAL("%zu", resp.reply_len, 0);

	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_ASSERT(req != NULL);
	BTEST_EXPECT(strcmp(req->method, "GET") == 0);
	BTEST_EXPECT(strcmp(req->uri, "/v1/game/ping") == 0);
	BTEST_EXPECT(strcmp(req->content_type, "") == 0);
	BTEST_EXPECT(strcmp(req->content_length, "") == 0);
	BTEST_EXPECT(strcmp(req->authorization, "") == 0);
	BTEST_EXPECT_EQUAL("%zu", req->body_len, 0);
}

BTEST(http, error_status_is_passed_through) {
	static const char reply[] = "retry later\n";
	server = test_http_start(&(test_http_reply_t){
		.status = 503, .body = reply, .body_len = sizeof(reply) - 1,
	});
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/report", (cw_request_t){
		.method = "POST", .content_type = "application/json", .body = "{}", .body_len = 2,
	}, &resp);
	BTEST_EXPECT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 503);
	BTEST_EXPECT(strcmp(reply_buf, reply) == 0);
}

BTEST(http, long_reply_is_truncated_to_cap) {
	static char reply[10000];
	for (size_t i = 0; i < sizeof(reply); ++i) {
		reply[i] = (char)('A' + i % 26);
	}
	server = test_http_start(&(test_http_reply_t){ .body = reply, .body_len = sizeof(reply) });
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/report", (cw_request_t){
		.method = "GET", .reply_cap = 100,
	}, &resp);
	BTEST_ASSERT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 200);
	BTEST_ASSERT_EQUAL("%zu", resp.reply_len, 100);
	BTEST_EXPECT(memcmp(reply_buf, reply, 100) == 0);
	BTEST_EXPECT_EQUAL("%d", test_http_count(server), 1);
}

BTEST(http, redirect_is_not_followed) {
	server = test_http_start(&(test_http_reply_t){
		.status = 302, .location = "http://127.0.0.1:9/elsewhere",
	});
	BTEST_ASSERT(server != NULL);

	cw_response_t resp;
	cw_status_t status = send_path("/v1/game/report", (cw_request_t){
		.method = "POST", .content_type = "application/json", .body = "{}", .body_len = 2,
	}, &resp);
	BTEST_EXPECT_EQUAL("%d", status, CW_OK);
	BTEST_EXPECT_EQUAL("%d", resp.status, 302);
	BTEST_EXPECT_EQUAL("%d", test_http_count(server), 1);
}

BTEST(http, refused_connection_is_retry) {
	/* The port was ours a moment ago, so nothing else is listening on it. */
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	char url[128];
	snprintf(url, sizeof(url), "%s/v1/game/report", test_http_url(server));
	test_http_stop(server);
	server = NULL;

	cw_response_t resp;
	cw_status_t status = send_to(url, (cw_request_t){
		.method = "POST", .content_type = "application/json", .body = "{}", .body_len = 2,
	}, &resp);
	BTEST_EXPECT_EQUAL("%d", status, CW_RETRY);
}

BTEST(http, unusable_url_is_drop) {
	cw_response_t resp;
	cw_request_t req = { .method = "POST", .content_type = "application/json", .body = "{}", .body_len = 2 };
	BTEST_EXPECT_EQUAL("%d", send_to("", req, &resp), CW_DROP);
	BTEST_EXPECT_EQUAL("%d", send_to("nope://127.0.0.1/", req, &resp), CW_DROP);
}
