/**
 * @file backend.h
 * What every cw_http backend implements and what they share.
 */
#ifndef CW_HTTP_BACKEND_H
#define CW_HTTP_BACKEND_H

#include "cw.h"

#define CW_HTTP_USER_AGENT      "crash-where/0.1"
#define CW_HTTP_CONNECT_TIMEOUT_MS 10000L
#define CW_HTTP_TIMEOUT_MS         60000L

/**
 * Perform one request on the platform's HTTP stack.
 *
 * Same contract as cw_transport_t::send: send the body with its content
 * type and length, the bearer token when given, never follow redirects,
 * copy at most `reply_cap` bytes of the reply and drain the rest.
 *
 * @return ::CW_OK when a reply arrived, ::CW_RETRY when the stack is
 *         unavailable or the network failed, ::CW_DROP when the request
 *         can never succeed.
 */
cw_status_t
cw_http_backend_send(const cw_request_t* req, cw_response_t* resp);

#endif /* CW_HTTP_BACKEND_H */
