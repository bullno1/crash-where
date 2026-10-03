/**
 * @file backend.h
 * What every cw_dialog backend implements.
 */
#ifndef CW_DIALOG_BACKEND_H
#define CW_DIALOG_BACKEND_H

/** Buttons a description may hold. */
#define CW_DIALOG_BUTTONS 3

/**
 * Show a dialog on the platform's UI and wait for the player.
 *
 * @param title    Window title; never `NULL`.
 * @param message  Body text; never `NULL`, possibly empty.
 * @param labels   `n` button labels in display order, all non-`NULL`.
 * @return The index of the pressed button, or -1 when the dialog was
 *         closed without one or could not be shown.
 */
int
cw_dialog_backend_show(
	const char* title, const char* message,
	const char* const* labels, int n
);

#endif /* CW_DIALOG_BACKEND_H */
