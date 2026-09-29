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
#define CW_UUID_CAP       37 /**< A UUID in text with its terminator; report ids are UUIDs. */
#define CW_PENDING_CAP    64
#define CW_TOKEN_CAP      1024
#define CW_PROOF_CAP      4096
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
	char report_dir[CW_STR_CAP]; /**< Resolved report directory; empty until cw_init() accepted the config. */
	cw_transport_t transport;  /**< Copy of the caller's transport; `cfg.transport` points here or is `NULL`. */
	cw_consent_dialog_t dialog; /**< Copy of the caller's prompt; `cfg.consent_dialog` points here or is `NULL`. */
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
	cw_report_kind_t kind;     /**< Names the file. */
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

/** Tell the watcher that the token or proof file was rewritten. */
void
cw_platform_notify_auth(void);

/** Tell the watcher the player's decision. */
void
cw_platform_notify_consent(cw_consent_t choice);

/**
 * The platform's report directory for cw_ctx_t::app.
 *
 * @return `false` when the environment names no state directory.
 */
bool
cw_platform_default_report_dir(char* out, size_t cap);

/** Create a directory and every missing parent. Succeeds when it exists. */
bool
cw_platform_mkdir_p(const char* path);

/**
 * Call `fn` with the name of every entry of a directory, in no
 * particular order and without `.` and `..`.
 *
 * @return `false` when the directory cannot be read.
 */
bool
cw_platform_list_dir(const char* path, void (*fn)(void* user, const char* name), void* user);

/** Rename `from` over `to`, replacing an existing file. */
bool
cw_platform_replace(const char* from, const char* to);

/**
 * Take an exclusive lock on a file, held until the process exits.
 *
 * @return `false` when another process holds it.
 */
bool
cw_platform_lock(const char* path);

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

/* Report directory files: `consent`, `token`, `proof`, `lock`, and `pending/`. */

/** Build `<report_dir>/<name>`. */
bool
cw_store_path(char* out, size_t cap, const char* name);

/**
 * Write a file under the report directory atomically, creating the
 * directory on first use.
 */
bool
cw_store_write(const char* name, const void* data, size_t len);

/**
 * Read a whole file under the report directory into `buf`, NUL
 * terminated.
 *
 * @return `false` when the file is absent or larger than `cap - 1`.
 */
bool
cw_store_read(const char* name, void* buf, size_t cap, size_t* len);

void
cw_store_remove(const char* name);

/** The persisted decision, ::CW_CONSENT_ASK when there is none. */
cw_consent_t
cw_consent_load(void);

/** Persist ::CW_CONSENT_ALWAYS or ::CW_CONSENT_NEVER; anything else clears the file. */
bool
cw_consent_store(cw_consent_t choice);

/**
 * The cached token and its expiry.
 *
 * @return `false` when there is none; the expiry is not checked.
 */
bool
cw_token_load(char* token, size_t cap, int64_t* expires);

/**
 * Write the proof as the finished body of the auth request, so the
 * watcher posts it verbatim.
 *
 * @return `false` when `store` is not a plain name or the file could
 *         not be written.
 */
bool
cw_proof_store(const char* store, const void* proof, size_t len);

bool
cw_token_store(const char* token, int64_t expires);

/**
 * Value of `key` in a text of `key value` lines, the format of server
 * replies and of the token file.
 *
 * @return `true` and the NUL-terminated value in `out`, or `false` when
 *         the key is absent or its value does not fit.
 */
bool
cw_reply_get(const char* text, const char* key, char* out, size_t cap);

/** File name letter of a report kind: `c`, `h`, or `a`. */
char
cw_report_kind_letter(cw_report_kind_t kind);

/**
 * One envelope in `pending/`, from its name alone.
 */
typedef struct {
	char name[96];             /**< File name with the `.json` extension. */
	int64_t ts;                /**< When it was written, Unix seconds. */
	cw_report_kind_t kind;
	bool approved;             /**< An `.ok` sidecar released it. */
} cw_pending_t;

/**
 * Describe an envelope from its file name, `<ts>_<kind>_<fp>_<id>.json`.
 *
 * @return `false` when `name` is not an envelope.
 */
bool
cw_pending_parse(const char* name, cw_pending_t* out);

/**
 * List the envelopes in `pending/`, oldest first.
 *
 * @return The number written to `out`, at most `cap`.
 */
int
cw_pending_list(cw_pending_t* out, int cap);

/** Full path of a listed envelope. */
void
cw_pending_path(const cw_pending_t* p, char* out, size_t cap);

/** Delete an envelope together with its sidecars and attachments. */
void
cw_pending_remove(const cw_pending_t* p);

/** Release every waiting envelope with an `.ok` sidecar. */
void
cw_pending_approve_all(void);

/** Delete everything in `pending/`. */
void
cw_pending_purge(void);

/** Count the envelopes awaiting a decision and describe the newest. */
void
cw_pending_summary(cw_consent_summary_t* out);

/* Watcher-side upload policy. */

/**
 * What the watcher knows about sending: the decision in force, the
 * token, whether the backlog of earlier runs has been drained, whether
 * the waiting proof is worth a try, and which reports already failed
 * this run and wait for the next launch.
 *
 * The consent and token files are read only here, at start and when the
 * game says they changed, never while the game may be writing them.
 */
typedef struct {
	cw_consent_t consent;      /**< From the file at start, then from messages. */
	char token[CW_TOKEN_CAP];  /**< From the file at start and on `auth refreshed`; empty when there is none. */
	int64_t token_expires;     /**< Unix seconds. */
	bool auth_seen;            /**< The game refreshed the token or handed over a proof. */
	bool proof_failed;         /**< The waiting proof could not be exchanged; not retried until a new one arrives. */
	bool caught_up;            /**< The backlog was drained once this run. */
	bool wrote_report;         /**< This run produced a report, so a native prompt is due. */
	bool locked;               /**< This watcher won the drainer lock. */
	uint64_t start_ms;
	char failed[CW_PENDING_CAP][CW_UUID_CAP]; /**< Ids of reports whose upload failed this run. */
	int failed_count;
} cw_drain_t;

void
cw_drain_init(cw_drain_t* d);

/** Whether reports may still be written; `false` under ::CW_CONSENT_NEVER. */
bool
cw_drain_accepts(const cw_drain_t* d);

/**
 * A report was just written; send it now when the decision allows.
 *
 * Delivered and rejected envelopes are deleted; failed ones stay in
 * `pending/` for the catch-up drain of a later run.
 */
void
cw_drain_report(cw_drain_t* d, const char* path);

/** The game sent `auth refreshed`: exchange the proof and drain. */
void
cw_drain_auth(cw_drain_t* d);

/** The game sent `consent(choice)`. */
void
cw_drain_consent(cw_drain_t* d, cw_consent_t choice);

/** Periodic check for the catch-up timeout; call from the watcher's poll loop. */
void
cw_drain_tick(cw_drain_t* d, uint64_t now_ms);

/** The game is gone: prompt if configured and due, then drain whatever the decision allows. */
void
cw_drain_finish(cw_drain_t* d);

#endif /* CW_INTERNAL_H */
