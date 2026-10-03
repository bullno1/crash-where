/**
 * @file scenario.h
 * Crash scenarios and the harness that runs them out of process.
 *
 * A scenario is a function that crashes, hangs, or exits. It runs in a
 * child process that is this same executable started again with
 * `CW_TEST_SCENARIO` set. The runner waits for the child and its watcher
 * to finish, then reads back every request the watcher made through the
 * test transport.
 */
#ifndef CW_TEST_SCENARIO_H
#define CW_TEST_SCENARIO_H

#include <stdbool.h>
#include <stdint.h>

#include "autolist.h"
#include "cw.h"
#include "platform.h"
#include "yyjson.h"

/**
 * One registered scenario.
 */
typedef struct {
	const char* name;
	void (*run)(void);
} test_scenario_t;

AUTOLIST_DECLARE(test_scenarios)

/**
 * Declare a scenario and begin its function body.
 */
#define CW_SCENARIO(NAME) \
	static void test_scenario_fn_##NAME(void); \
	AUTOLIST_ENTRY(test_scenarios, test_scenario_t, test_scenario_##NAME) = { \
		.name = #NAME, \
		.run = test_scenario_fn_##NAME, \
	}; \
	static void test_scenario_fn_##NAME(void)

/**
 * Reference a scenario declared in this file.
 */
#define SCENARIO_REF(NAME) (&test_scenario_##NAME)

/**
 * Options for one run. A zero-initialized value means the upload
 * succeeds, attachments are declined, the library is enabled, no
 * debugger is attached, the report directory starts empty, the child
 * consents to every upload, no authentication happens, and no native
 * prompt is configured.
 */
typedef struct {
	const char* status;    /**< HTTP status the test transport answers: "ok" (200), "retry" (503), or "drop" (400). `NULL` means "ok". */
	bool want_attachments; /**< Value of `want_attachments` in the test transport's reply. */
	bool disable;          /**< Run the child with `CW_DISABLE=1`. */
	bool debugger;         /**< Put the child under a debugger before it initializes the library. */
	bool force;            /**< Run the child with `CW_DISABLE=0`. */
	uint32_t hang_timeout_ms; /**< Passed to cw_config_t::hang_timeout_ms; 0 keeps the library default. */
	bool keep;             /**< Keep the report directory of this test's previous run instead of starting empty. */
	const char* auth;      /**< "proof" hands a proof to the watcher, "token" caches a token, "expired" caches one with a past expiry; `NULL` does neither. The child records the call's result in the `auth` state slot. */
	const char* consent;   /**< Decision the child records after init: "always" (`NULL`), "never", "once", "ask", or "skip" to leave the stored one alone. */
	const char* dialog;    /**< Answer of the native prompt: "always", "never", "once", or "ask"; `NULL` configures none. */
	bool host;             /**< Plug `cw_collector_host` into both collector slots instead of the test collectors. */
} test_run_opts_t;

#define TEST_MAX_EVENTS 8

/**
 * Outcome of one scenario run.
 *
 * Every transport call is one event, in call order:
 * `{"call":"request","method":...,"url":...,"content_type":...,
 * "token":...,"body_len":...,"envelope":{...}}`, where `envelope` is
 * present when the body is JSON. The native prompt records
 * `{"call":"dialog","count":...,"kind":...,"time":...}` and a scenario
 * may add events of its own with test_write_event().
 */
typedef struct {
	char dir[256];        /**< Directory holding the report store and the transport log. */
	test_exit_t exit;
	int num_events;
	yyjson_doc* events[TEST_MAX_EVENTS];
} test_run_t;

/**
 * Run a scenario in a child and collect the outcome.
 *
 * Files go under `work/<test>/`. The result stays valid until the next
 * run or test_run_cleanup().
 *
 * @param opts  `NULL` for the defaults.
 * @return `NULL` when the child could not be started.
 */
const test_run_t*
test_run_scenario(const char* test, const test_scenario_t* scenario, const test_run_opts_t* opts);

#define RUN_SCENARIO(REF) test_run_scenario(__func__, REF, NULL)
#define RUN_SCENARIO_WITH(REF, ...) test_run_scenario(__func__, REF, &(test_run_opts_t){ __VA_ARGS__ })

/**
 * Release the last run. Suitable as a suite's `cleanup_per_test`.
 */
void
test_run_cleanup(void);

/**
 * Whether a file exists in the run's report directory.
 */
bool
test_run_has(const test_run_t* run, const char* name);

/**
 * Number of files in the run's pending directory whose names end with
 * `suffix`, or -1 when there is no such directory.
 */
int
test_run_pending(const test_run_t* run, const char* suffix);

/**
 * Whether an event is a request to `route` with the given bearer token,
 * `NULL` for none.
 */
bool
test_event_is(yyjson_doc* ev, const char* route, const char* token);

/**
 * Library log sink that forwards to blog at the matching level.
 */
void
test_cw_log(cw_log_level_t level, const char* msg);

/**
 * Child side: append one event to the run's log and free the document.
 */
void
test_write_event(yyjson_mut_doc* doc);

/**
 * Decode a gzip stream as the library writes it.
 *
 * @param out_len  Receives the decoded size.
 * @return A buffer to free, or `NULL` when `in` is not such a stream or
 *         its checksum or size does not match.
 */
uint8_t*
test_gunzip(const void* in, size_t len, size_t* out_len);

/** The CRC-32 a gzip trailer carries. */
uint32_t
test_crc32(const void* data, size_t len);

/**
 * Child side: initialize the library and run the named scenario.
 *
 * @return The process exit status for a scenario that returns.
 */
int
test_fixture_main(const char* name);

/**
 * Address the child recorded in a state slot as hex, or 0 when absent.
 */
uintptr_t
test_state_hex(yyjson_doc* ev, const char* key);

/**
 * Absolute address of frame `i`, or 0 when it is not inside the main
 * module. The main module is the one whose base matches the `base` the
 * child recorded in a state slot.
 */
uintptr_t
test_frame_addr(yyjson_doc* ev, size_t i);

/**
 * String at a JSON pointer, or `NULL` when absent or not a string.
 */
static inline const char*
test_json_str(yyjson_doc* doc, const char* ptr) {
	return yyjson_get_str(yyjson_doc_ptr_get(doc, ptr));
}

/**
 * Value at a JSON pointer, or `NULL` when absent.
 */
static inline yyjson_val*
test_json_get(yyjson_doc* doc, const char* ptr) {
	return yyjson_doc_ptr_get(doc, ptr);
}

#endif /* CW_TEST_SCENARIO_H */
