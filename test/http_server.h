/**
 * @file http_server.h
 * Loopback HTTP/1.1 server that records what a transport sends.
 *
 * The server runs on its own thread, listens on an ephemeral port of
 * 127.0.0.1, and answers every request the same way. Requests are
 * recorded in arrival order and can be read once the client has seen
 * the reply.
 */
#ifndef CW_TEST_HTTP_SERVER_H
#define CW_TEST_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#define TEST_HTTP_MAX_REQUESTS 4
#define TEST_HTTP_BODY_CAP     (64 * 1024)

/**
 * One request as the server saw it. Header fields are empty when the
 * header was absent.
 */
typedef struct {
	char method[16];
	char uri[256];           /**< Path, percent-decoded by the server. */
	char query[256];         /**< Query string after `?`, as sent, or empty. */
	char content_type[64];
	char content_encoding[32];
	char content_length[24];
	char authorization[256];
	char user_agent[64];
	char expect[32];
	size_t body_len;         /**< Bytes of body received, capped at ::TEST_HTTP_BODY_CAP. */
	char body[TEST_HTTP_BODY_CAP];
} test_http_request_t;

/**
 * How the server answers.
 */
typedef struct {
	int status;              /**< HTTP status; 0 selects 200. */
	const char* body;        /**< Reply body, or `NULL` for an empty one. Must outlive the server. */
	size_t body_len;
	const char* location;    /**< `Location` header, or `NULL` for none. */
} test_http_reply_t;

typedef struct test_http_server_s test_http_server_t;

/**
 * Start a server on a new thread.
 *
 * @param reply  How to answer, or `NULL` for an empty 200.
 * @return The server, or `NULL` when it could not listen. Free with test_http_stop().
 */
test_http_server_t*
test_http_start(const test_http_reply_t* reply);

/**
 * Base URL of the server, `http://127.0.0.1:<port>`, valid until test_http_stop().
 */
const char*
test_http_url(const test_http_server_t* server);

/**
 * Number of requests served so far.
 */
int
test_http_count(const test_http_server_t* server);

/**
 * The `index`th request, or `NULL` when fewer have arrived or it is past
 * the ::TEST_HTTP_MAX_REQUESTS the server keeps.
 */
const test_http_request_t*
test_http_request(const test_http_server_t* server, int index);

/**
 * Stop the thread, close the socket, and free the server.
 */
void
test_http_stop(test_http_server_t* server);

#endif /* CW_TEST_HTTP_SERVER_H */
