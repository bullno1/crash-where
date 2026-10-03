/**
 * @file dialog.c
 * cw_dialog_show: an empty description draws nothing, and a place with
 * nothing to draw on answers "ask".
 */
#include <stdlib.h>

#include "btest.h"
#include "cw_dialog.h"
#include "internal.h"
#include "scenario.h"

static void
dialog_init(void) {
	cw_ctx.cfg.log = test_cw_log;
}

static void
dialog_cleanup(void) {
	cw_ctx.cfg.log = NULL;
}

static btest_suite_t dialog = {
	.name = "dialog",
	.init_per_test = dialog_init,
	.cleanup_per_test = dialog_cleanup,
};

static const cw_dialog_desc_t three_buttons = {
	.title = "cw_test",
	.message = "Not meant to be seen.",
	.buttons = {
		{ "Send", CW_CONSENT_ONCE },
		{ "Always send", CW_CONSENT_ALWAYS },
		{ "Don't send", CW_CONSENT_NEVER },
	},
};

BTEST(dialog, null_description_asks) {
	BTEST_EXPECT_EQUAL("%d", cw_dialog_show(NULL), CW_CONSENT_ASK);
}

BTEST(dialog, no_title_asks) {
	cw_dialog_desc_t desc = three_buttons;
	desc.title = NULL;
	BTEST_EXPECT_EQUAL("%d", cw_dialog_show(&desc), CW_CONSENT_ASK);
}

BTEST(dialog, no_buttons_asks) {
	cw_dialog_desc_t desc = { .title = "cw_test", .message = "Not meant to be seen." };
	BTEST_EXPECT_EQUAL("%d", cw_dialog_show(&desc), CW_CONSENT_ASK);
}

/*
 * A Windows desktop would show the dialog here and wait for a click;
 * a page that is not the watcher cannot wait for one and shows nothing.
 */
#if !defined(_WIN32)
BTEST(dialog, nothing_to_draw_on_asks) {
#if defined(__linux__)
	/*
	 * SDL's windowless driver has no message box. Pinned by both names
	 * so that SDL2 and SDL3 obey; an unset display is not enough, since
	 * the Wayland driver finds the default socket by itself.
	 */
	setenv("SDL_VIDEO_DRIVER", "dummy", 1);
	setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
	BTEST_EXPECT_EQUAL("%d", cw_dialog_show(&three_buttons), CW_CONSENT_ASK);
}
#endif
