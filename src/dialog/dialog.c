/**
 * @file dialog.c
 * Checks the description and maps the pressed button back to a choice;
 * the platform backend draws.
 */
#include "cw_dialog.h"
#include "backend.h"

cw_consent_t
cw_dialog_show(const cw_dialog_desc_t* desc) {
	if (desc == NULL || desc->title == NULL) {
		return CW_CONSENT_ASK;
	}

	/* Buttons without a label are left out, wherever they sit. */
	const char* labels[CW_DIALOG_BUTTONS];
	cw_consent_t choices[CW_DIALOG_BUTTONS];
	int n = 0;
	for (int i = 0; i < CW_DIALOG_BUTTONS; ++i) {
		if (desc->buttons[i].label != NULL) {
			labels[n] = desc->buttons[i].label;
			choices[n] = desc->buttons[i].choice;
			++n;
		}
	}
	if (n == 0) {
		return CW_CONSENT_ASK;
	}

	const char* message = desc->message != NULL ? desc->message : "";
	int pressed = cw_dialog_backend_show(desc->title, message, labels, n);
	return pressed >= 0 && pressed < n ? choices[pressed] : CW_CONSENT_ASK;
}
