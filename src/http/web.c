/**
 * @file http/web.c
 * Web backend: sends nothing, so every report stays in the store.
 */
#include "backend.h"

cw_status_t
cw_http_backend_send(const cw_request_t* req, cw_response_t* resp) {
	(void)req;
	(void)resp;
	return CW_RETRY;
}
