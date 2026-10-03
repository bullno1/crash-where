/**
 * @file sdl.c
 * SDL backend for Linux: the message box of the SDL on the system.
 *
 * SDL is loaded on first use and never linked, so no SDL headers are
 * needed to build. Its message box works before the library is
 * initialized, tries every display driver it has, and draws by itself
 * on X11, so it needs no helper program the Steam Runtime lacks.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>

#include "backend.h"
#include "internal.h"

/* The message box ABI, the same in SDL2 and SDL3. */

#define SDL_BUTTON_RETURNKEY_DEFAULT 1

typedef struct {
	uint32_t flags;
	int id;
	const char* text;
} sdl_button_t;

typedef struct {
	uint32_t flags;
	void* window;
	const char* title;
	const char* message;
	int num_buttons;
	const sdl_button_t* buttons;
	const void* color_scheme;
} sdl_data_t;

/* The two differ in how success is reported: SDL3 returns true, SDL2 returns 0. */
typedef bool (*sdl3_show_t)(const sdl_data_t* data, int* pressed);
typedef int (*sdl2_show_t)(const sdl_data_t* data, int* pressed);
typedef const char* (*sdl_get_error_t)(void);

/** The loaded library and its entry points; loading is attempted once. */
static struct {
	void* lib;
	bool tried;
	sdl3_show_t show3;
	sdl2_show_t show2;
	sdl_get_error_t get_error;
} sdl;

static bool
load(void) {
	if (sdl.tried) {
		return sdl.lib != NULL;
	}
	sdl.tried = true;

	/* The library the game links, if any, is already loaded and found first. */
	static const struct {
		const char* name;
		bool v3;
	} names[] = {
		{ "libSDL3.so.0", true },
		{ "libSDL2-2.0.so.0", false },
	};
	bool v3 = false;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && sdl.lib == NULL; ++i) {
		sdl.lib = dlopen(names[i].name, RTLD_NOW | RTLD_LOCAL);
		v3 = names[i].v3;
	}
	if (sdl.lib == NULL) {
		cw_log(CW_LOG_WARN, "no SDL found, dialog not shown: %s", dlerror());
		return false;
	}

	void* show = dlsym(sdl.lib, "SDL_ShowMessageBox");
	sdl.get_error = (sdl_get_error_t)dlsym(sdl.lib, "SDL_GetError");
	if (show == NULL || sdl.get_error == NULL) {
		cw_log(CW_LOG_ERROR, "SDL is missing a required symbol: %s", dlerror());
		dlclose(sdl.lib);
		sdl.lib = NULL;
		return false;
	}
	if (v3) {
		sdl.show3 = (sdl3_show_t)show;
	} else {
		sdl.show2 = (sdl2_show_t)show;
	}
	return true;
}

int
cw_dialog_backend_show(
	const char* title, const char* message,
	const char* const* labels, int n
) {
	if (!load()) {
		return -1;
	}

	sdl_button_t buttons[CW_DIALOG_BUTTONS];
	for (int i = 0; i < n; ++i) {
		buttons[i] = (sdl_button_t){
			.flags = i == 0 ? SDL_BUTTON_RETURNKEY_DEFAULT : 0,
			.id = i,
			.text = labels[i],
		};
	}
	sdl_data_t data = {
		.title = title,
		.message = message,
		.num_buttons = n,
		.buttons = buttons,
	};

	/* Left alone by a dialog closed without a button. */
	int pressed = -1;
	bool ok = sdl.show3 != NULL ? sdl.show3(&data, &pressed) : sdl.show2(&data, &pressed) == 0;
	if (!ok) {
		cw_log(CW_LOG_WARN, "SDL could not show the dialog: %s", sdl.get_error());
		return -1;
	}
	return pressed;
}
