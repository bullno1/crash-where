/**
 * @file host/web.c
 * Web backend of the host collector. Records nothing: the facts belong
 * to the page and are not collected.
 */
#include "backend.h"

void
cw_host_backend_init(void) {
}

void
cw_host_backend_report(uint32_t pid) {
	(void)pid;
}
