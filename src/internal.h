/**
 * @file internal.h
 * Cross-platform internals shared by every source file.
 *
 * Nothing here names a platform type. The shared region layout, the
 * process context, and the crash description handed to the envelope
 * writer are defined here; each platform embeds cw_shared_t at the start
 * of its own region type and appends what it needs.
 */
#ifndef CW_INTERNAL_H
#define CW_INTERNAL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cw.h"

#define CW_REGION_MAGIC   0x30575243u /* "CRW0" */
#define CW_CRUMB_COUNT    128
#define CW_STATE_COUNT    32
#define CW_MAX_MODULES    64
#define CW_MAX_FRAMES     64
#define CW_STR_CAP        256
#define CW_ENV_WATCHER    "CW_WATCHER" /**< Set by the game on the watcher: `<pid>,<platform handles>`. */

/**
 * Lifecycle of the platform's crash-time record.
 */
typedef enum {
	CW_CRASH_IDLE    = 0,      /**< Nothing recorded. */
	CW_CRASH_DONE    = 1,      /**< A handler has completed the record. */
	CW_CRASH_WRITING = 2,      /**< A handler owns the record and is filling it. */
} cw_crash_state_t;

/**
 * One breadcrumb ring entry.
 *
 * `seq` is 0 while the entry is being written and the publish sequence
 * number afterwards. Readers drop entries with `seq == 0`.
 */
typedef struct {
	_Atomic uint64_t seq;
	uint64_t t_ms;
	uint32_t tid;
	char cat[8];
	char msg[48];
} cw_crumb_t;

/**
 * One key/value state slot.
 *
 * `seq` is 0 when the slot is free, otherwise the stamp of the last
 * write, which doubles as the eviction order.
 */
typedef struct {
	_Atomic uint64_t seq;
	char key[16];
	char value[40];
} cw_state_slot_t;

/**
 * Cross-platform head of the shared region.
 */
typedef struct {
	uint32_t magic;            /**< Guards against a wrong fd number in the environment. */
	_Atomic uint64_t heartbeat;
	_Atomic uint32_t heartbeat_tid; /**< Thread of the last cw_heartbeat() call. */
	_Atomic uint64_t crumb_next;
	_Atomic uint64_t state_seq;
	cw_crumb_t crumbs[CW_CRUMB_COUNT];
	cw_state_slot_t state[CW_STATE_COUNT];
} cw_shared_t;

/**
 * Process-wide library state.
 */
typedef struct {
	cw_config_t cfg;           /**< Copy of the caller's config; string members point into the buffers below. */
	char app[64];
	char version[64];
	char channel[64];
	char endpoint[CW_STR_CAP];
	char report_dir[CW_STR_CAP];
	cw_transport_t transport;  /**< Copy of the caller's transport; `cfg.transport` points here or is `NULL`. */
	cw_shared_t* shared;       /**< Mapped region, or `NULL` when inactive. */
	bool initialized;          /**< cw_init() has run, whatever the outcome. */
	bool active;               /**< Capture is armed in this process. */
} cw_ctx_t;

extern cw_ctx_t cw_ctx;

#define CW_HANG_DEFAULT_MS  10000u
#define CW_HANG_MAX_REPORTS 3

/**
 * Hang detector, driven by the watcher from heartbeat readings.
 *
 * Arms on the first change of the heartbeat, files one report per
 * stall, and re-arms once the heartbeat moves again. Holds no clock of
 * its own; every step takes the current time as an argument.
 */
typedef struct {
	uint64_t count;            /**< Heartbeat value at the last change. */
	uint64_t since_ms;         /**< When the heartbeat last changed. */
	bool armed;                /**< A heartbeat has been seen. */
	bool reported;             /**< The current stall has been reported. */
	int reports;               /**< Reports filed this session. */
} cw_hang_t;

typedef enum {
	CW_HANG_NONE,
	CW_HANG_REPORT,            /**< The heartbeat has been silent for the timeout. */
	CW_HANG_RECOVERED,         /**< The heartbeat resumed after a report. */
} cw_hang_event_t;

/**
 * Feed one heartbeat reading to the detector.
 *
 * @param count       Current heartbeat value.
 * @param now_ms      Current monotonic time.
 * @param timeout_ms  Silence that counts as a hang.
 */
cw_hang_event_t
cw_hang_step(cw_hang_t* hang, uint64_t count, uint64_t now_ms, uint64_t timeout_ms);

/**
 * Restart the silence clock without changing the armed state, for a
 * game that is stopped or under a debugger and cannot tick.
 */
void
cw_hang_reset(cw_hang_t* hang, uint64_t now_ms);

/**
 * How often the watcher samples the heartbeat for `timeout_ms`: a
 * quarter of it, between 10 ms and 1 s.
 */
int
cw_hang_poll_ms(uint64_t timeout_ms);

/**
 * One loaded module as seen at crash time.
 */
typedef struct {
	char path[CW_STR_CAP];
	char build_id[41];         /**< Hex, empty when unknown. */
	uint64_t base;
	uint64_t size;
} cw_module_t;

/**
 * One stack frame: a module index into cw_crash_info_t::modules,
 * or -1 with `offset` holding the raw address.
 */
typedef struct {
	int module;
	uint64_t offset;
} cw_frame_t;

/**
 * Platform-independent description of a crash, filled by the
 * platform and consumed by cw_write_envelope().
 */
typedef struct {
	char type[32];             /**< Exception type such as "SIGSEGV". */
	char message_raw[128];
	uint64_t fault_addr;
	uint32_t tid;
	int main_module;           /**< Index of the executable in `modules`, or -1. */
	cw_module_t modules[CW_MAX_MODULES];
	int module_count;
	cw_frame_t frames[CW_MAX_FRAMES];
	int frame_count;
} cw_crash_info_t;

/* Implemented by the platform. */

/**
 * Game side: spawn the watcher, map the region, set cw_ctx_t::shared,
 * install handlers. Returns `false` when the library must stay inactive.
 */
bool
cw_platform_run_game(void);

/**
 * Watcher side: adopt the handles named in `spec`, the value of
 * CW_ENV_WATCHER, watch the game, exit.
 */
_Noreturn void
cw_platform_run_watcher(const char* spec);

bool
cw_platform_random(void* buf, size_t len);

/** Monotonic milliseconds. Async-signal-safe. */
uint64_t
cw_platform_now_ms(void);

/** Calling thread id. Async-signal-safe. */
uint32_t
cw_platform_tid(void);

/** Tell the watcher the game is exiting on purpose. */
void
cw_platform_shutdown(int result);

/**
 * Attach the calling thread.
 */
void
cw_platform_attach_thread(void);

/**
 * Check that the crash handlers installed at init are still in place;
 * log a warning naming the module that replaced one. Called once, from
 * the first cw_heartbeat().
 */
void
cw_platform_check_handlers(void);

/* Implemented by the core. */

void
cw_log(cw_log_level_t level, const char* fmt, ...);

/**
 * Write one envelope into `<report_dir>/pending/`.
 *
 * @param out_path  Receives the final path on success.
 * @return `true` when the file was renamed into place.
 */
bool
cw_write_envelope(
	const char* report_dir, const cw_crash_info_t* info,
	const cw_shared_t* shared, char* out_path, size_t cap
);

/**
 * Send one written envelope through the transport.
 *
 * Delivered and rejected envelopes are deleted; failed ones stay in
 * `pending/`. Without a transport the file is left where it is and a
 * warning is logged.
 */
void
cw_upload_report(const char* path);

#endif /* CW_INTERNAL_H */
