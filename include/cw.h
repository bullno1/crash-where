/**
 * @file cw.h
 * Public API of crash-where, a crash reporter.
 *
 * @ref cw_init spawns a watcher process from the same binary. The watcher
 * monitors the game and submits a report when it crashes, stops
 * responding, or disappears without calling @ref cw_shutdown.
 *
 * Only the game process returns from @ref cw_init. The watcher will call
 * `exit` and never execute the rest of the program.
 *
 * Two environment variables affect behaviour:
 * - `CW_WATCHER` is set by the game on the watcher to carry inherited
 *   handles. Never set it yourself.
 * - `CW_DISABLE=1` disables the library. `CW_DISABLE=0` keeps it enabled
 *   under a debugger.
 *
 * The library disables itself when a debugger is attached to the game at
 * @ref cw_init, so that the debugger gets every crash. One attached later
 * changes nothing. On the web the library stays enabled.
 */
#ifndef CW_H
#define CW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result of a transport call or an upload step.
 *
 * The value decides what happens to the report or proof afterwards.
 */
typedef enum {
	CW_OK,      /**< Done. */
	CW_RETRY,   /**< Transient failure such as network, HTTP 429, HTTP 5xx, or a bad clock. Tried again later. */
	CW_DROP,    /**< Permanent failure such as another HTTP 4xx or malformed input. Discarded. */
} cw_status_t;

/**
 * Severity passed to cw_config_t::log.
 */
typedef enum {
	CW_LOG_ERROR = 0,
	CW_LOG_WARN  = 1,
	CW_LOG_INFO  = 2,
	CW_LOG_DEBUG = 3
} cw_log_level_t;

/**
 * The player's decision about sending reports.
 *
 * Nothing leaves the machine, the storefront proof included, until the
 * decision is ::CW_CONSENT_ALWAYS or a report was released with
 * ::CW_CONSENT_ONCE.
 */
typedef enum {
	CW_CONSENT_ASK,    /**< No decision yet, or the prompt was dismissed. Reports wait on disk. */
	CW_CONSENT_ALWAYS, /**< Send every report, now and in later runs. */
	CW_CONSENT_NEVER,  /**< Send nothing; pending reports are deleted and no new ones are written. */
	CW_CONSENT_ONCE,   /**< Send the reports waiting right now, then ask again for later ones. Never stored. */
} cw_consent_t;

/**
 * What a report describes.
 */
typedef enum {
	CW_REPORT_CRASH,
	CW_REPORT_HANG,
	CW_REPORT_ABNORMAL_EXIT, /**< The game ended without cw_shutdown(). */
} cw_report_kind_t;

/**
 * Reports waiting for a decision, for a consent screen.
 */
typedef struct {
	int count;                   /**< Reports awaiting a decision. */
	cw_report_kind_t newest_kind; /**< Kind of the newest of them. Meaningless when `count` is 0. */
	int64_t newest_time;         /**< When the newest was written, as Unix seconds. */
} cw_consent_summary_t;

/**
 * Native consent prompt shown by the watcher.
 *
 * The callback runs in the watcher process, never in the game process,
 * and only after the game process is gone: after a crash, or at exit
 * for a hang or an abnormal end. It cannot rely on anything the game
 * initialized, and the `user` pointer must be valid in that process.
 * A game without a native prompt asks in game on the next launch
 * instead, using cw_consent_pending().
 */
typedef struct {
	/**
	 * Ask the player what to do with the waiting reports.
	 *
	 * @param user     The `user` member of this struct.
	 * @param summary  The reports awaiting a decision.
	 * @return The player's choice. ::CW_CONSENT_ASK means the prompt was
	 *         not shown or was dismissed; the reports then wait for the
	 *         in-game prompt on a later launch.
	 */
	cw_consent_t (*show)(void* user, const cw_consent_summary_t* summary);

	void* user; /**< Passed unchanged as the first argument of the callback. */
} cw_consent_dialog_t;

/**
 * One HTTP request as handed to the transport.
 *
 * Every pointer is owned by the library and valid only for the duration
 * of the call that receives it.
 */
typedef struct {
	const char* method;       /**< "POST", "PUT", or "GET". */
	const char* url;          /**< Absolute URL. */
	const char* content_type; /**< Media type of `body`, or `NULL` when there is no body. */
	const char* token;        /**< Bearer token, or `NULL` when unauthenticated. */
	const void* body;         /**< Request body, or `NULL`. */
	size_t body_len;          /**< Length of `body` in bytes. */
	char* reply;              /**< Buffer that receives the reply body. */
	size_t reply_cap;         /**< Capacity of `reply` in bytes. */
} cw_request_t;

/**
 * Outcome of one transport call.
 */
typedef struct {
	int status;       /**< HTTP status, or 0 when no reply arrived. */
	size_t reply_len; /**< Bytes written to cw_request_t::reply. A longer reply is truncated. */
} cw_response_t;

/**
 * HTTP transport hook.
 *
 * Optional; with a `NULL` cw_config_t::transport every report stays in
 * the report directory, nothing is sent, and no proof is exchanged.
 *
 * The callback runs in the watcher process, never in the game process.
 * It cannot rely on anything the game initialized, and the `user`
 * pointer must be valid in that process.
 */
typedef struct {
	/**
	 * Perform one HTTP request and wait for the reply.
	 *
	 * Send `body`, when present, with its `Content-Type` and
	 * `Content-Length`.
	 * Send `Authorization: Bearer <token>` when a token is given.
	 * Never follow redirects.
	 * Copy at most `reply_cap` bytes of the reply body into `reply`, reading
	 * and discarding the rest so the status still arrives.
	 *
	 * @param user  The `user` member of this struct.
	 * @param req   The request.
	 * @param resp  Filled in on ::CW_OK.
	 * @return ::CW_OK when an HTTP reply arrived, whatever its status;
	 *         ::CW_RETRY when the network, name resolution, or TLS failed;
	 *         ::CW_DROP when the request can never succeed, such as a
	 *         malformed URL.
	 */
	cw_status_t (*send)(
		void* user, const cw_request_t* req, cw_response_t* resp
	);

	void* user; /**< Passed unchanged as the first argument of the callback. */
} cw_transport_t;

/**
 * Gathers facts about the run into the report environment.
 *
 * The callback writes with cw_set_env(). Which process calls it, and
 * when, depends on the cw_config_t slot it is plugged into.
 */
typedef struct {
	/**
	 * @param user  The `user` member of this struct.
	 */
	void (*collect)(void* user);

	void* user; /**< Passed unchanged as the first argument of the callback. */
} cw_collector_t;

/**
 * Initialization parameters for cw_init().
 *
 * Zero-initialize the struct, then set the fields you need.
 */
typedef struct {
	/**
	 * Application identifier such as "forest-quest".
	 *
	 * Required. Lowercase letters, digits, `-`, and `_` only, at most 63
	 * bytes. Names the default report directory and identifies the game
	 * to the server, so several games can share a machine and a backend.
	 * Keep it the same across releases.
	 */
	const char* app;
	const char* version;     /**< Application version such as "1.4.2". The server rejects reports from unknown versions. */
	const char* channel;     /**< Build stream such as "stable" or "beta". The server rejects reports whose channel differs from the one the release was registered with. */
	const char* endpoint;    /**< Base URL of the ingest service, without a trailing slash. */
	const char* report_dir;  /**< Directory for pending reports, or `NULL` for the platform default. */

	/**
	 * Heartbeat silence reported as a hang, in milliseconds.
	 *
	 * 0 selects the default of 10 seconds. Detection starts with the
	 * first cw_heartbeat() call; a game that never calls it is never
	 * reported as hung.
	 */
	uint32_t hang_timeout_ms;

	const cw_transport_t* transport; /**< HTTP transport, or `NULL` to keep reports on disk unsent. */
	const cw_consent_dialog_t* consent_dialog; /**< Native consent prompt, or `NULL` to ask in game only. */

	/**
	 * Collector called once from the game at the end of cw_init(),
	 * before any window or GPU exists. `NULL` collects nothing.
	 */
	const cw_collector_t* collect_at_init;

	/**
	 * Collector called from the watcher before each report is written,
	 * never from the game. The game is frozen or gone: the callback
	 * must not signal it, read its memory, or use any of its
	 * subsystems, and the `user` pointer must be valid in the watcher.
	 * `NULL` collects nothing.
	 */
	const cw_collector_t* collect_at_report;

	/**
	 * Diagnostic log sink.
	 *
	 * Optional; may be `NULL`. Called from both the game and the watcher.
	 *
	 * @param level  Severity of the message.
	 * @param msg    NUL-terminated message without a trailing newline.
	 */
	void (*log)(cw_log_level_t level, const char* msg);
} cw_config_t;

/**
 * Initialize crash reporting.
 *
 * Call as early as possible in `main`, before any window, GPU, or audio
 * initialization.
 *
 * From this point onwards, all crashes will be reported.
 *
 * @param cfg  Configuration. Copied; the caller may discard it after the call.
 */
void
cw_init(const cw_config_t* cfg);

/**
 * Attach the calling thread to crash reporting.
 *
 * A crash on any thread can already be reported.
 * However, a stack overflow can only be reliably reported from an attached thread.
 *
 * On Linux, only an attached thread's stack is captured in full.
 *
 * On the web the call has is no-op.
 * Errors are always recorded in full even without attaching.
 *
 * The thread that calls @ref cw_init is already attached.
 *
 * Call once per thread, before doing any work.
 * At most 256 threads can be attached at once; a further call logs a warning
 * and does nothing.
 *
 * There is no need to detach.
 */
void
cw_attach_thread(void);

/**
 * Submit a storefront proof (e.g: a Steam encrypted app ticket) to the watcher.
 *
 * The watcher will use the configured @ref cw_config_t::transport for
 * authentication.
 *
 * Authentication serves as a noise filter. It is not meant to be a security
 * boundary.
 *
 * Call this after the storefront SDK is initialized.
 * A newer proof replaces one still waiting.
 * The exchange happens once the consent decision allows it.
 *
 * @param store  Storefront name such as "steam", "epic", "gog", "itch",
 *               or "beta": lowercase letters, digits, `-`, and `_`.
 * @param proof  Proof bytes. Copied.
 * @param len    Length of `proof`, at most 4 KB.
 * @return `false` when the library is inactive, no transport is
 *         configured, or the proof could not be stored.
 */
bool
cw_auth_proof(const char* store, const void* proof, size_t len);

/**
 * Cache a token the game has obtained itself.
 *
 * For custom authentication methods.
 *
 * @param token    NUL-terminated token.
 * @param expires  Expiry as Unix seconds.
 * @return `false` when the library is inactive or the token could not
 *         be stored.
 */
bool
cw_auth_token(const char* token, int64_t expires);

/**
 * Check whether to show a consent screen to the user.
 *
 * This will only return true if:
 *
 * - No permanent decision was made (@ref CW_CONSENT_ALWAYS or @ref CW_CONSENT_NEVER).
 * - There are pending reports.
 *
 * Otherwise, return false.
 *
 * @param summary  Filled with info about pending reports.
 *                 May be `NULL`. Zeroed when the answer is `false`.
 * @return `true` when the player should be asked.
 */
bool
cw_consent_pending(cw_consent_summary_t* summary);

/**
 * Record the player's decision.
 *
 * - ::CW_CONSENT_ALWAYS and ::CW_CONSENT_NEVER persist across runs
 * - ::CW_CONSENT_ASK clears a persisted decision
 * - ::CW_CONSENT_ONCE releases the reports waiting right now and persists nothing
 * - ::CW_CONSENT_NEVER deletes every waiting report.
 *
 * Safe to call at every launch, for a game that keeps the decision in
 * its own settings. Takes effect at once; the watcher starts or stops
 * sending without a restart.
 *
 * @param choice  The decision.
 */
void
cw_consent_set(cw_consent_t choice);

/**
 * Retrieve the persisted decision.
 *
 * @return ::CW_CONSENT_ALWAYS, ::CW_CONSENT_NEVER, or ::CW_CONSENT_ASK
 *         when none was recorded. Never ::CW_CONSENT_ONCE.
 */
cw_consent_t
cw_consent_get(void);

/**
 * Mark the game alive.
 *
 * Call once per frame or tick. When the calls stop for
 * @ref cw_config_t::hang_timeout_ms a hang report is submitted and the game
 * is left running. A later stall is reported again once the calls have
 * resumed.
 *
 * The report describes the thread that called this function last.
 */
void
cw_heartbeat(void);

/**
 * Record a transition such as a level load, menu change, device
 * loss, or focus change.
 *
 * Breadcrumbs describe the recent sequence of events, not per-tick state.
 * Aim for at most a few per second; the ring holds the newest 128.
 *
 * Never allocates or blocks. Safe from any thread, including a signal
 * handler.
 *
 * @param category  Short tag, truncated to 7 bytes.
 * @param msg       Message, truncated to 47 bytes.
 */
void
cw_breadcrumb(const char* category, const char* msg);

/**
 * Set or overwrite a key/value slot describing current state.
 *
 * State answers "what was the game doing" at crash time, for example
 * `"level"` = `"forest_02"`. Setting an existing key replaces its value.
 * There are about 32 slots of 64 bytes each; when all are in use the
 * least recently written key is evicted.
 *
 * Same safety guarantees as cw_breadcrumb().
 *
 * @param key    Slot name.
 * @param value  New value.
 */
void
cw_set_state(const char* key, const char* value);

/**
 * Set or overwrite a fact about the environment of this run.
 *
 * Environment answers "what machine and configuration was this", for
 * example `"gpu"` = `"NVIDIA GeForce RTX 3070"` or `"renderer"` =
 * `"vulkan"`.
 *
 * Setting an existing key replaces its value.
 * There are 32 slots and entries are never evicted.
 * A new key when all are in use is dropped and logged.
 * Keys are truncated to 23 bytes and values to 95 bytes.
 *
 * Values appear in dashboards, so never put a username, path, or other
 * personal data in one.
 *
 * Same safety guarantees as cw_breadcrumb(). Also callable from a
 * cw_config_t::collect_at_report callback in the watcher.
 *
 * @param key    Slot name.
 * @param value  New value.
 */
void
cw_set_env(const char* key, const char* value);

/**
 * Publish a binary snapshot such as level state or an RNG seed.
 *
 * The data is copied; the caller's buffer may be reused as soon as the
 * call returns. A report never contains a partially written snapshot.
 * Total capacity across all slots is a few megabytes; an oversized
 * snapshot is rejected and logged.
 *
 * @param name  Slot name. Reusing a name replaces the earlier snapshot.
 * @param data  Bytes to copy.
 * @param len   Number of bytes.
 */
void
cw_set_snapshot(const char* name, const void* data, size_t len);

/**
 * Append one record to a named byte ring.
 *
 * Intended for an action log since the last checkpoint, streaming events,
 * and similar high-volume data. Each ring is capped by bytes, not by
 * record count; the oldest bytes are overwritten.
 *
 * @param name  Ring name.
 * @param rec   Record bytes to append.
 * @param len   Length of `rec`.
 */
void
cw_append_log(const char* name, const void* rec, size_t len);

/**
 * Report a clean shutdown.
 *
 * Required; without it the exit is reported as abnormal. Call this at
 * the end of `main` or from the framework's quit callback.
 *
 * @param result  Process exit status. A nonzero value is recorded as a
 *                controlled failure rather than a crash.
 */
void
cw_shutdown(int result);

#ifdef __cplusplus
}
#endif

#endif /* CW_H */
