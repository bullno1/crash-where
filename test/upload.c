/**
 * @file upload.c
 * The release request against the loopback server: what is sent, and
 * how each way the server or the transport can refuse is reported.
 */
#include <stdio.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "cw_http.h"
#include "http_server.h"
#include "sym.h"

static test_http_server_t* server;
static char log_text[4096];

static void
stop_server(void) {
	if (server != NULL) {
		test_http_stop(server);
		server = NULL;
	}
}

static btest_suite_t upload = {
	.name = "upload",
	.cleanup_per_test = stop_server,
};

static void
on_log(void* user, const char* msg) {
	(void)user;
	BLOG_INFO("cwsym: %s", msg);
	size_t len = strlen(log_text);
	snprintf(log_text + len, sizeof(log_text) - len, "%s\n", msg);
}

static const cwsym_log_t logger = { .log = on_log };

/** A one-function table with a display name, so it is complete. */
static void
build_table(cwsym_table_t* t) {
	cwsym_table_builder_t* b = cwsym_table_begin();
	cwsym_table_set_module(b, &(cwsym_module_t){
		.arch = CWSYM_ARCH_X86_64, .build_id = { 0xab, 0xcd }, .build_id_len = 20,
	});
	const char* scope[] = { "main" };
	cwsym_table_add(b, &(cwsym_symbol_t){ .start = 0x1000, .size = 0x40, .scope = scope, .scope_len = 1, .display = "main()" });
	BTEST_ASSERT_EQUAL("%d", cwsym_table_end(b, t, &logger), CWSYM_OK);
}

static cwsym_upload_t
request(const char* version, const char* channel) {
	return (cwsym_upload_t){
		.endpoint = test_http_url(server),
		.app = "forest-quest",
		.token = "ci-token",
		.version = version,
		.channel = channel,
	};
}

BTEST(upload, puts_table_to_release_route) {
	static const char reply[] = "ok\n";
	server = test_http_start(&(test_http_reply_t){ .status = 200, .body = reply, .body_len = sizeof(reply) - 1 });
	BTEST_ASSERT(server != NULL);
	cwsym_table_t t;
	build_table(&t);
	log_text[0] = '\0';

	cwsym_upload_t up = request("1.4.2", "stable");
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &cw_transport_http, &logger), CWSYM_OK);
	BTEST_ASSERT_EQUAL("%d", test_http_count(server), 1);
	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_EXPECT(strcmp(req->method, "PUT") == 0);
	BTEST_EXPECT_EX(strcmp(req->uri, "/v1/forest-quest/releases/1.4.2") == 0, "uri is %s", req->uri);
	BTEST_EXPECT_EX(strcmp(req->query, "channel=stable") == 0, "query is %s", req->query);
	BTEST_EXPECT(strcmp(req->content_type, "application/octet-stream") == 0);
	BTEST_EXPECT(strcmp(req->authorization, "Bearer ci-token") == 0);
	char len[24];
	snprintf(len, sizeof(len), "%zu", t.len);
	BTEST_EXPECT(strcmp(req->content_length, len) == 0);
	BTEST_EXPECT_EQUAL("%zu", req->body_len, t.len);
	BTEST_EXPECT(memcmp(req->body, t.data, t.len) == 0);
	BTEST_EXPECT_EX(strstr(log_text, "abcd") != NULL && strstr(log_text, "200") != NULL, "log: %s", log_text);
	cwsym_table_free(&t);
}

BTEST(upload, encodes_version_and_channel) {
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	cwsym_table_t t;
	build_table(&t);
	cwsym_upload_t up = request("1.4.2 rc/1", "beta+nightly");
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &cw_transport_http, &logger), CWSYM_OK);
	BTEST_ASSERT_EQUAL("%d", test_http_count(server), 1);
	const test_http_request_t* req = test_http_request(server, 0);
	/* The server decodes the path but hands the query over as sent. */
	BTEST_EXPECT_EX(strcmp(req->uri, "/v1/forest-quest/releases/1.4.2 rc/1") == 0, "uri is %s", req->uri);
	BTEST_EXPECT_EX(strcmp(req->query, "channel=beta%2Bnightly") == 0, "query is %s", req->query);
	cwsym_table_free(&t);
}

BTEST(upload, reports_server_refusal) {
	static const char reply[] = "conflict build_id\n";
	server = test_http_start(&(test_http_reply_t){ .status = 409, .body = reply, .body_len = sizeof(reply) - 1 });
	BTEST_ASSERT(server != NULL);
	cwsym_table_t t;
	build_table(&t);
	log_text[0] = '\0';
	cwsym_upload_t up = request("1.4.2", "stable");
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &cw_transport_http, &logger), CWSYM_ERR_IO);
	BTEST_EXPECT_EX(strstr(log_text, "409") != NULL && strstr(log_text, "conflict build_id") != NULL, "log: %s", log_text);
	cwsym_table_free(&t);
}

BTEST(upload, refuses_incomplete_input) {
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	cwsym_table_t t;
	build_table(&t);

	cwsym_upload_t up = request("1.4.2", "stable");
	up.token = NULL;
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &cw_transport_http, &logger), CWSYM_ERR_INVALID);
	up = request("1.4.2", "");
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &cw_transport_http, &logger), CWSYM_ERR_INVALID);

	/* The Worker's prefix is not a table to upload. */
	cwsym_table_t prefix;
	size_t prefix_len = (size_t)((const char*)t.strings - (const char*)t.data) + t.strings_len;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_parse(t.data, prefix_len, &prefix, &logger), CWSYM_OK);
	up = request("1.4.2", "stable");
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &prefix, &cw_transport_http, &logger), CWSYM_ERR_INVALID);
	cwsym_table_free(&prefix);

	BTEST_EXPECT_EQUAL("%d", test_http_count(server), 0);
	cwsym_table_free(&t);
}

/* Fake transports {{{ */

static cw_status_t
send_retry(void* user, const cw_request_t* req, cw_response_t* resp) {
	(void)user;
	(void)req;
	(void)resp;
	return CW_RETRY;
}

static cw_status_t
send_created(void* user, const cw_request_t* req, cw_response_t* resp) {
	(void)user;
	(void)req;
	*resp = (cw_response_t){ .status = 201, .reply_len = 0 };
	return CW_OK;
}

BTEST(upload, maps_transport_outcomes) {
	cwsym_table_t t;
	build_table(&t);
	cwsym_upload_t up = {
		.endpoint = "https://crash.example", .app = "forest-quest", .token = "t", .version = "1", .channel = "stable",
	};
	log_text[0] = '\0';
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &(cw_transport_t){ .send = send_retry }, &logger), CWSYM_ERR_IO);
	BTEST_EXPECT_EX(strstr(log_text, "https://crash.example/v1/forest-quest/releases/1?channel=stable") != NULL, "log: %s", log_text);
	BTEST_EXPECT_EQUAL("%d", cwsym_upload(&up, &t, &(cw_transport_t){ .send = send_created }, &logger), CWSYM_OK);
	cwsym_table_free(&t);
}

/* }}} */
