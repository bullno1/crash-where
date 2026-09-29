/**
 * @file web/platform.h
 * What the C side and the shim share on the web.
 *
 * The game and the watcher are two instances of the same binary, each
 * with a memory of its own. Every function declared under "shim" runs in
 * the instance that calls it.
 */
#ifndef CW_WEB_PLATFORM_H
#define CW_WEB_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"

#define CW_WEB_BUILD_ID_CAP 20

/**
 * Watcher: one trap of the game.
 *
 * @param message  Message of the exception, as the engine worded it.
 * @param stack    Stack text of the exception.
 * @param region   Copy of the game's region, owned by the caller.
 */
typedef void (*cw_web_report_fn_t)(const char* message, const char* stack, cw_shared_t* region);

/** Watcher: the player decided; `choice` is a cw_consent_t. */
typedef void (*cw_web_consent_fn_t)(int choice);

/** Watcher: the game is exiting on purpose. */
typedef void (*cw_web_shutdown_fn_t)(int result);

/* Shim, game side. */

/**
 * Hand the region to the shim, which copies it out when the game traps.
 *
 * @return `false` when no watcher answered before `main` was released.
 */
bool
cw_web_game_start(const cw_shared_t* region, size_t len);

/** The decision the watcher found in its store, as a cw_consent_t. */
int
cw_web_game_consent(void);

/** Number of reports the watcher found waiting. */
int
cw_web_game_pending_count(void);

/**
 * File name of a waiting report.
 *
 * @param approved  Receives whether an approval released it.
 * @return `false` when `i` is out of range or the name does not fit.
 */
bool
cw_web_game_pending(int i, char* name, size_t cap, bool* approved);

void
cw_web_game_notify_consent(int choice);

void
cw_web_game_notify_shutdown(int result);

/* Shim, watcher side. */

/** Announce one waiting report; call before cw_web_watcher_ready(). */
void
cw_web_watcher_pending(const char* name, bool approved);

/**
 * The store is loaded: release the game and take its messages from now on.
 *
 * @param consent  The stored decision, as a cw_consent_t.
 */
void
cw_web_watcher_ready(
	int consent,
	cw_web_report_fn_t on_report, cw_web_consent_fn_t on_consent,
	cw_web_shutdown_fn_t on_shutdown
);

/**
 * Build id of this binary.
 *
 * @return Bytes written to `out`, 0 when the binary carries none.
 */
size_t
cw_web_build_id(uint8_t* out, size_t cap);

/** File name of this binary, empty when it does not fit. */
void
cw_web_module_name(char* out, size_t cap);

/* Stack text. */

/**
 * Frames of a stack text, innermost first.
 *
 * Takes every `wasm-function[N]:0x...` token, which V8 and SpiderMonkey
 * both print, and ignores the rest of each line. Every frame belongs to
 * module 0 and its offset counts from the start of the file.
 *
 * @return The number of frames written to `frames`, at most `cap`.
 */
int
cw_web_parse_stack(const char* stack, cw_frame_t* frames, int cap);

/**
 * Exception type for a trap message, the same for every engine.
 *
 * A message that matches nothing known becomes `TRAP`.
 */
void
cw_web_trap_type(const char* message, char* out, size_t cap);

#endif /* CW_WEB_PLATFORM_H */
