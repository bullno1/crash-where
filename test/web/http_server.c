/**
 * @file web/http_server.c
 * The loopback server of http_server.h, for the web.
 *
 * A page cannot listen, so the server is one the launcher starts on a
 * port of its own, and the calls below are its remote control. The
 * imports are implemented by the test library.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <blog.h>
#include "http_server.h"

/**
 * Start a server.
 *
 * @param body      Reply body, or `NULL` for none.
 * @param location  `Location` header, or `NULL` for none.
 * @param url       Receives the base URL.
 * @return The server's id, or -1 when the launcher did not answer.
 */
int
test_web_http_start(
	int status, const char* body, size_t body_len,
	const char* location, char* url, size_t url_cap
);

int
test_web_http_count(int id);

/** Read one recorded request back; `false` when it has not arrived. */
bool
test_web_http_load(int id, int index);

/** One text field of a loaded request, by its name in test_http_request_t. */
void
test_web_http_field(int id, int index, const char* name, char* out, size_t cap);

/** The body of a loaded request; returns the bytes written. */
size_t
test_web_http_body(int id, int index, char* out, size_t cap);

void
test_web_http_stop(int id);

struct test_http_server_s {
	int id;
	char url[48];
	bool loaded[TEST_HTTP_MAX_REQUESTS];
	test_http_request_t requests[TEST_HTTP_MAX_REQUESTS];
};

test_http_server_t*
test_http_start(const test_http_reply_t* reply) {
	static const test_http_reply_t empty = { 0 };
	if (reply == NULL) {
		reply = &empty;
	}
	test_http_server_t* server = calloc(1, sizeof(*server));
	if (server == NULL) {
		return NULL;
	}
	server->id = test_web_http_start(
		reply->status, reply->body, reply->body_len,
		reply->location, server->url, sizeof(server->url)
	);
	if (server->id < 0) {
		BLOG_ERROR("the launcher started no server");
		free(server);
		return NULL;
	}
	return server;
}

const char*
test_http_url(const test_http_server_t* server) {
	return server->url;
}

int
test_http_count(const test_http_server_t* server) {
	return test_web_http_count(server->id);
}

const test_http_request_t*
test_http_request(const test_http_server_t* server, int index) {
	if (index < 0 || index >= TEST_HTTP_MAX_REQUESTS) {
		return NULL;
	}
	/* Read back on first use; the copy is this object's to fill. */
	test_http_server_t* cache = (test_http_server_t*)server;
	test_http_request_t* req = &cache->requests[index];
	if (!cache->loaded[index]) {
		if (!test_web_http_load(server->id, index)) {
			return NULL;
		}
#define FIELD(NAME) test_web_http_field(server->id, index, #NAME, req->NAME, sizeof(req->NAME))
		FIELD(method);
		FIELD(uri);
		FIELD(query);
		FIELD(content_type);
		FIELD(content_length);
		FIELD(authorization);
		FIELD(user_agent);
		FIELD(expect);
#undef FIELD
		req->body_len = test_web_http_body(server->id, index, req->body, sizeof(req->body));
		cache->loaded[index] = true;
	}
	return req;
}

void
test_http_stop(test_http_server_t* server) {
	if (server == NULL) {
		return;
	}
	test_web_http_stop(server->id);
	free(server);
}
