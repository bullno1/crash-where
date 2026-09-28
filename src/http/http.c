/**
 * @file http.c
 * The transport object; the platform backend does the work.
 */
#include "cw_http.h"
#include "backend.h"

static cw_status_t
http_send(void* user, const cw_request_t* req, cw_response_t* resp) {
	(void)user;
	*resp = (cw_response_t){ 0 };
	return cw_http_backend_send(req, resp);
}

const cw_transport_t cw_transport_http = {
	.send = http_send,
	.user = NULL,
};
