/**
 * @file cw_http.h
 * HTTP transport for crash-where, built on the platform's HTTP stack.
 *
 * Link the `cw_http` library next to `cw` and point
 * cw_config_t::transport at @ref cw_http_transport. Nothing else is
 * needed; the transport has no state to set up or tear down.
 */
#ifndef CW_HTTP_H
#define CW_HTTP_H

#include "cw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Transport that uses the operating system's HTTP client.
 *
 * On Windows this is WinHTTP; on Linux it's the system libcurl,
 * loaded on first use in the watcher. Proxies, certificate stores, and
 * TLS are the platform's. Redirects are never followed. When the HTTP
 * stack is unavailable every request returns ::CW_RETRY, so reports
 * stay on disk until it is.
 *
 * `user` is `NULL`; the transport never reads it.
 */
extern const cw_transport_t cw_transport_http;

#ifdef __cplusplus
}
#endif

#endif /* CW_HTTP_H */
