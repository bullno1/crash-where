/**
 * @file cw.h
 * @brief Public API of crash-where, a crash reporter.
 *
 * The first process that calls @ref cw_init becomes the supervisor. It
 * re-executes the same binary as the game process, monitors it, and submits
 * a report when it crashes or stops responding.
 *
 * Only the game process returns from @ref cw_init. The supervisor will call
 * `exit` and never execute the rest of the program.
 *
 * Everything in this header except the cw_uploader_t callbacks runs in
 * the game process.
 *
 * Two environment variables affect behaviour:
 * - `CW_CHILD` is set by the supervisor on the child to carry the process
 *   role and inherited handles. Never set it yourself.
 * - `CW_DISABLE=1` disables the library.
 *   Use it when running under a debugger.
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
 * @brief Result of an authentication or upload step.
 *
 * The value decides what happens to the report or proof afterwards.
 */
typedef enum {
	CW_OK,      /**< Done. */
	CW_RETRY,   /**< Transient failure such as network, HTTP 429, HTTP 5xx, or a bad clock. Tried again later. */
	CW_DROP,    /**< Permanent failure such as another HTTP 4xx or malformed input. Discarded. */
} cw_status_t;

/**
 * @brief Severity passed to cw_config_t::log.
 */
typedef enum {
	CW_LOG_ERROR = 0,
	CW_LOG_WARN  = 1,
	CW_LOG_INFO  = 2,
	CW_LOG_DEBUG = 3
} cw_log_level_t;

/**
 * @brief Storefront authentication hooks.
 *
 * Authentication filters noise from pirated copies and bots; it is not a
 * security boundary.
 *
 * Both callbacks run in the game process only.
 */
typedef struct {
	const char* store; /**< Storefront name such as "steam", "epic", "gog", "itch", or "beta". Sent with the proof. */

	/**
	 * @brief Produce the storefront proof, for example a Steam encrypted app ticket.
	 *
	 * @param user  The `user` member of this struct.
	 * @param buf   Output buffer for the proof bytes.
	 * @param len   On entry the capacity of `buf`; on ::CW_OK the number of bytes written.
	 * @return ::CW_OK on success, ::CW_RETRY if the storefront SDK is not ready yet,
	 *         ::CW_DROP if no proof can ever be produced.
	 */
	cw_status_t (*get_proof)(void* user, void* buf, size_t* len);

	/**
	 * @brief Exchange the proof for a token yourself.
	 *
	 * Optional; `NULL` lets the library POST the proof to `<endpoint>/v1/auth`.
	 *
	 * @param user     The `user` member of this struct.
	 * @param proof    Bytes produced by get_proof.
	 * @param len      Length of `proof`.
	 * @param token    Output buffer; receives a NUL-terminated token.
	 * @param cap      Capacity of `token` in bytes, including the terminator.
	 * @param expires  Receives the token expiry as Unix seconds.
	 * @return ::CW_OK on success, ::CW_RETRY on a transient failure, ::CW_DROP otherwise.
	 */
	cw_status_t (*exchange)(
		void* user,
		const void* proof, size_t len,
		char* token, size_t cap, int64_t* expires
	);

	void* user; /**< Passed unchanged as the first argument of every callback. */
} cw_auth_t;

/**
 * @brief One pending report as handed to an uploader.
 *
 * All strings and paths are owned by the library and remain valid only
 * for the duration of the callback that receives them.
 */
typedef struct {
	const char* id;                 /**< Client-generated UUID v4. Retries reuse it so the server can deduplicate. */
	const char* envelope_json;      /**< The JSON envelope, uncompressed. */
	const char* token;              /**< Cached authentication token, or `NULL` when unauthenticated. */
	const char* const* attachments; /**< `NULL`-terminated list of file paths (minidump, log tail, snapshots). */
	int attempts;                   /**< Number of earlier delivery attempts for this report. */
} cw_report_t;

/**
 * @brief Upload transport hooks.
 *
 * Optional; a `NULL` cw_config_t::uploader selects the built-in transport.
 *
 * The callbacks run in a process owned by the library, never in the game
 * process. They cannot rely on anything the game initialized, and the
 * `user` pointer must be valid in that process.
 */
typedef struct {
	/**
	 * @brief POST the envelope.
	 *
	 * Map the HTTP status as follows: 2xx is ::CW_OK; 429, 5xx, and network
	 * errors are ::CW_RETRY; every other 4xx, including 410, is ::CW_DROP.
	 *
	 * @param user              The `user` member of this struct.
	 * @param report            The report to send.
	 * @param want_attachments  On ::CW_OK, set from the server reply. `false` makes
	 *                          the library delete the attachments locally without
	 *                          sending them.
	 * @return The mapped status.
	 */
	cw_status_t (*send_envelope)(
		void* user, const cw_report_t* report,
		bool* want_attachments
	);

	/**
	 * @brief POST one attachment file under the same report id.
	 *
	 * Called once per entry of cw_report_t::attachments, only after
	 * send_envelope returned ::CW_OK with `want_attachments` set.
	 *
	 * @param user    The `user` member of this struct.
	 * @param report  The report the attachment belongs to.
	 * @param path    Path of the file to send.
	 * @return Status mapped as for send_envelope.
	 */
	cw_status_t (*send_attachment)(
		void* user, const cw_report_t* report,
		const char* path
	);

	void* user; /**< Passed unchanged as the first argument of every callback. */
} cw_uploader_t;

/**
 * @brief Initialization parameters for cw_init().
 *
 * Zero-initialize the struct, then set the fields you need. `size` lets
 * the library detect a caller built against a different header revision.
 */
typedef struct {
	size_t size;             /**< Must be `sizeof(cw_config_t)`. */

	const char* version;     /**< Application version such as "1.4.2". The server rejects reports from unknown versions. */
	const char* channel;     /**< Distribution channel such as "steam" or "beta". */
	const char* endpoint;    /**< Base URL of the ingest service, without a trailing slash. */
	const char* report_dir;  /**< Directory for pending reports, or `NULL` for the platform default. */

	/**
	 * @brief Command line to re-execute the game with.
	 *
	 * Optional. When `argv` is `NULL` the library reads the command line
	 * from the operating system. Pass it when the program modifies its own
	 * arguments or when the OS does not expose them.
	 */
	int argc;
	const char** argv;

	/**
	 * @brief The process is already running game code; do not re-execute it.
	 *
	 * Set when a framework initialized a window or SDL before this call.
	 * The supervisor then runs beside the game instead of as its parent,
	 * and the game must call cw_shutdown().
	 */
	bool late;

	const cw_auth_t* auth;         /**< Storefront authentication, or `NULL` for unauthenticated reports. */
	const cw_uploader_t* uploader; /**< Upload transport, or `NULL` for the built-in one. */

	/**
	 * @brief Diagnostic log sink.
	 *
	 * Optional; may be `NULL`. Called from both the game and the supervisor.
	 *
	 * @param level  Severity of the message.
	 * @param msg    NUL-terminated message without a trailing newline.
	 */
	void (*log)(cw_log_level_t level, const char* msg);
} cw_config_t;

/**
 * @brief Initialize crash reporting.
 *
 * Call as early as possible in `main`, before any window, GPU, or audio
 * initialization.
 *
 * The first process does not return from this call; it exits with the
 * game's exit status. The game process returns and continues.
 *
 * The game always runs. If the supervisor cannot be started, or if
 * `CW_DISABLE=1` is set, the call returns with the library inactive and
 * every other function becomes a no-op.
 *
 * A second call is also a no-op.
 *
 * @param cfg  Configuration. Copied; the caller may discard it after the call.
 */
void
cw_init(const cw_config_t* cfg);

/**
 * @brief Run storefront authentication again.
 *
 * Call after the storefront SDK is initialized, which usually happens
 * after cw_init(). Safe to call repeatedly. On success, later uploads
 * carry the token.
 *
 * @return ::CW_OK when a valid token is cached, ::CW_RETRY when the proof
 *         is not obtainable yet, ::CW_DROP when authentication is not
 *         configured or was refused.
 */
cw_status_t
cw_auth_refresh(void);

/**
 * @brief Mark the game alive.
 *
 * Call once per frame or tick. A hang is reported when hearbeat stops for
 * several seconds.
 */
void
cw_heartbeat(void);

/**
 * @brief Record a transition such as a level load, menu change, device
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
 * @brief Set or overwrite a key/value slot describing current state.
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
 * @brief Publish a binary snapshot such as level state or an RNG seed.
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
 * @brief Append one record to a named byte ring.
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
 * @brief Report a clean shutdown.
 *
 * Optional except when cw_config_t::late is set.
 * Call this at the end of `main` or from the framework's quit callback.
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
