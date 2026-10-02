/**
 * @file http_server.c
 * Loopback server on top of the vendored wby, polled from one thread.
 */

/* A page cannot listen: web/http_server.c implements http_server.h over the launcher. */
#ifndef __EMSCRIPTEN__

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <blog.h>
#include "http_server.h"
#include "platform.h"
#include "wby.h"

struct test_http_server_s {
	struct wby_server wby;
	void* memory;
	thrd_t thread;
	test_http_reply_t reply;
	char url[48];
	_Atomic bool stop;
	_Atomic int count;      /**< Published after the request below it is fully recorded. */
	test_http_request_t requests[TEST_HTTP_MAX_REQUESTS];
};

static void
copy_header(struct wby_con* con, const char* name, char* out, size_t cap) {
	const char* value = wby_find_header(con, name);
	snprintf(out, cap, "%s", value != NULL ? value : "");
}

static void
record(struct wby_con* con, test_http_request_t* req) {
	snprintf(req->method, sizeof(req->method), "%s", con->request.method);
	snprintf(req->uri, sizeof(req->uri), "%s", con->request.uri);
	snprintf(req->query, sizeof(req->query), "%s", con->request.query_params != NULL ? con->request.query_params : "");
	copy_header(con, "Content-Type", req->content_type, sizeof(req->content_type));
	copy_header(con, "Content-Length", req->content_length, sizeof(req->content_length));
	copy_header(con, "Authorization", req->authorization, sizeof(req->authorization));
	copy_header(con, "User-Agent", req->user_agent, sizeof(req->user_agent));
	copy_header(con, "Expect", req->expect, sizeof(req->expect));
	req->body_len = 0;
	size_t left = con->request.content_length > 0 ? (size_t)con->request.content_length : 0;
	while (left > 0 && req->body_len < TEST_HTTP_BODY_CAP) {
		size_t chunk = left < TEST_HTTP_BODY_CAP - req->body_len ? left : TEST_HTTP_BODY_CAP - req->body_len;
		if (wby_read(con, req->body + req->body_len, chunk) != 0) {
			break;
		}
		req->body_len += chunk;
		left -= chunk;
	}
}

static int
dispatch(struct wby_con* con, void* user) {
	test_http_server_t* server = user;
	int index = atomic_load_explicit(&server->count, memory_order_relaxed);
	if (index < TEST_HTTP_MAX_REQUESTS) {
		record(con, &server->requests[index]);
	}
	atomic_store_explicit(&server->count, index + 1, memory_order_release);

	const test_http_reply_t* reply = &server->reply;
	struct wby_header headers[2];
	int num_headers = 0;
	if (reply->location != NULL) {
		headers[num_headers++] = (struct wby_header){ "Location", reply->location };
	}
	if (reply->body != NULL) {
		headers[num_headers++] = (struct wby_header){ "Content-Type", "text/plain" };
	}
	int status = reply->status != 0 ? reply->status : 200;
	if (wby_response_begin(con, status, (int)reply->body_len, headers, num_headers) != 0) {
		return 0;
	}
	if (reply->body_len > 0) {
		wby_write(con, reply->body, reply->body_len);
	}
	wby_response_end(con);
	return 0;
}

static int
serve(void* arg) {
	test_http_server_t* server = arg;
	while (!atomic_load_explicit(&server->stop, memory_order_relaxed)) {
		wby_update(&server->wby, 0);
		test_sleep_ms(1);
	}
	return 0;
}

static void
log_from_wby(const char* msg) {
	BLOG_TRACE("wby: %s", msg);
}

test_http_server_t*
test_http_start(const test_http_reply_t* reply) {
	if (!test_sockets_init()) {
		BLOG_ERROR("no socket layer");
		return NULL;
	}
	test_http_server_t* server = calloc(1, sizeof(*server));
	if (server == NULL) {
		return NULL;
	}
	if (reply != NULL) {
		server->reply = *reply;
	}
	struct wby_config config = {
		.userdata = server,
		.address = "127.0.0.1",
		.port = 0,
		.connection_max = 4,
		.request_buffer_size = 8192,
		.io_buffer_size = 8192,
		.log = log_from_wby,
		.dispatch = dispatch,
	};
	wby_size needed = 0;
	wby_init(&server->wby, &config, &needed);
	server->memory = calloc(1, needed);
	if (server->memory == NULL || wby_start(&server->wby, server->memory) != 0) {
		BLOG_ERROR("cannot start the loopback server");
		free(server->memory);
		free(server);
		return NULL;
	}
	snprintf(server->url, sizeof(server->url), "http://127.0.0.1:%u", (unsigned)server->wby.config.port);
	if (thrd_create(&server->thread, serve, server) != thrd_success) {
		BLOG_ERROR("cannot start the server thread");
		wby_stop(&server->wby);
		free(server->memory);
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
	return atomic_load_explicit(&server->count, memory_order_acquire);
}

const test_http_request_t*
test_http_request(const test_http_server_t* server, int index) {
	int count = test_http_count(server);
	return index < count && index < TEST_HTTP_MAX_REQUESTS ? &server->requests[index] : NULL;
}

void
test_http_stop(test_http_server_t* server) {
	if (server == NULL) {
		return;
	}
	atomic_store_explicit(&server->stop, true, memory_order_relaxed);
	thrd_join(server->thread, NULL);
	wby_stop(&server->wby);
	free(server->memory);
	free(server);
}

/* Last: the implementation redefines snprintf on Windows. */
#if defined(_MSC_VER)
#	pragma warning(push, 0)
#endif
#define WBY_IMPLEMENTATION
#include "wby.h"
#if defined(_MSC_VER)
#	pragma warning(pop)
#endif

#endif /* __EMSCRIPTEN__ */
