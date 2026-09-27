/**
 * @file windows/platform.h
 * Windows part of the shared region and the Windows process state.
 */
#ifndef CW_WINDOWS_PLATFORM_H
#define CW_WINDOWS_PLATFORM_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

/** Status a fast-fail `abort` ends the process with; reported as "ABORT". */
#define CW_STATUS_ABORT         STATUS_STACK_BUFFER_OVERRUN
/** Exception code of a C++ `throw` in MSVC-compiled code. */
#define CW_STATUS_CPP_EXCEPTION 0xE06D7363u

/**
 * Crash-time record written by the exception filter in the game and
 * read by the watcher.
 *
 * `state` holds a cw_crash_state_t.
 */
typedef struct {
	_Atomic uint32_t state;
	uint32_t tid;
	uint64_t pointers;         /**< `EXCEPTION_POINTERS*` in the game, for a later minidump. */
	EXCEPTION_RECORD record;
	CONTEXT context;
} cw_crash_t;

/**
 * Full layout of the pagefile-backed section on Windows.
 */
typedef struct {
	cw_shared_t common;
	int32_t shutdown_result;   /**< Valid once the shutdown event is set. */
	cw_crash_t crash;
} cw_region_t;

/**
 * Handles shared by the game and the watcher.
 *
 * The game creates them inheritable; their values travel to the watcher
 * in CW_ENV_WATCHER and stay the same in the child.
 */
typedef struct {
	HANDLE section;
	HANDLE game;               /**< The game process. */
	HANDLE ev_crash;           /**< Game: crash record is complete. */
	HANDLE ev_done;            /**< Watcher: report written, game may die. */
	HANDLE ev_ready;           /**< Watcher: region mapped, watching. */
	HANDLE ev_shutdown;        /**< Game: exiting on purpose, result is in the region. */
} cw_handles_t;

typedef struct {
	cw_region_t* region;
	cw_handles_t h;
	HANDLE watcher;            /**< Watcher process, game side only. */
} cw_win_t;

extern cw_win_t cw_win;

/** Install the exception filter and the abort handler in the game process. */
void
cw_install_exception_handler(void);

/**
 * Build the module table and frame list for a parked game.
 *
 * Enumerates the game's modules and walks the crashing thread's stack
 * from the recorded context while the game is blocked in the crash
 * protocol.
 *
 * @return `true` when at least one frame was produced.
 */
bool
cw_unwind(HANDLE game, const cw_crash_t* crash, cw_crash_info_t* out);

#endif /* CW_WINDOWS_PLATFORM_H */
