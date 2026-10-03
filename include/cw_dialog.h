/**
 * @file cw_dialog.h
 * Native dialog for the consent prompt of crash-where.
 */
#ifndef CW_DIALOG_H
#define CW_DIALOG_H

#include "cw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One button of a native dialog.
 */
typedef struct {
	const char* label;   /**< UTF-8 button text, or `NULL` to leave the button out. */
	cw_consent_t choice; /**< Returned when the player presses it. */
} cw_dialog_button_t;

/**
 * A native dialog with up to three buttons.
 *
 * Buttons appear in array order and the first one is activated by
 * Enter. Every string is UTF-8 and need only outlive the call.
 */
typedef struct {
	const char* title;   /**< Window title. `NULL` shows nothing. */
	const char* message; /**< Body text; newlines break lines. `NULL` for none. */
	cw_dialog_button_t buttons[3];
} cw_dialog_desc_t;

/**
 * Show a native dialog and wait for the user's answer.
 *
 * Needs nothing initialized and may be called from any process,
 * including the watcher from cw_consent_dialog_t::show.
 *
 * On Windows this is a task dialog, which needs Common Controls
 * version 6 in the executable's manifest. A build with MSVC gets the
 * dependency from the library.
 *
 * On Linux it is the message box of the SDL library found on the system,
 * loaded on first use.
 *
 * On the web it is a `<dialog>` element on the page, unstyled and marked
 * with the class `cw-dialog`. A page may define `Module.cwShowDialog(title,
 * message, labels)` returning a promise of the pressed button's index
 * to override the dialog.
 * A browser that cannot suspend the watcher shows nothing.
 *
 * @return The choice of the pressed button. ::CW_CONSENT_ASK when the
 *         dialog was closed or could not be shown.
 */
cw_consent_t
cw_dialog_show(const cw_dialog_desc_t* desc);

#ifdef __cplusplus
}
#endif

#endif /* CW_DIALOG_H */
