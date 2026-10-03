/**
 * @file dialog/web.c
 * Web backend: a dialog on the page, in an instance that can wait for it.
 */
#include "backend.h"
#include "internal.h"

/** A cw_dialog_web_show() that could not show anything. */
#define CW_DIALOG_WEB_UNSUPPORTED (-2)

/**
 * Shim: show the dialog on the page and wait for the player.
 *
 * Suspends the caller where the instance was set up for it. Elsewhere
 * nothing can wait, so nothing is shown.
 *
 * @return The index of the pressed button, -1 when closed without one,
 *         or @ref CW_DIALOG_WEB_UNSUPPORTED.
 */
int
cw_dialog_web_show(
	const char* title, const char* message,
	const char* const* labels, int n
);

int
cw_dialog_backend_show(
	const char* title, const char* message,
	const char* const* labels, int n
) {
	int pressed = cw_dialog_web_show(title, message, labels, n);
	if (pressed == CW_DIALOG_WEB_UNSUPPORTED) {
		cw_log(CW_LOG_WARN, "this instance cannot wait for a dialog, none shown");
		return -1;
	}
	return pressed;
}
