/**
 * @file none.c
 * Backend for a platform without one: nothing is shown.
 */
#include "backend.h"
#include "internal.h"

int
cw_dialog_backend_show(
	const char* title, const char* message,
	const char* const* labels, int n
) {
	(void)title;
	(void)message;
	(void)labels;
	(void)n;
	cw_log(CW_LOG_WARN, "no dialog on this platform, none shown");
	return -1;
}
