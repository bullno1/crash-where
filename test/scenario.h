/**
 * @file scenario.h
 * Crash scenarios and the harness that runs them out of process.
 *
 * A scenario is a function that crashes, hangs, or exits. It runs in a
 * child process that is this same executable started again with
 * `CW_TEST_SCENARIO` set. The runner waits for the child and its watcher
 * to finish, then reads back every call the watcher made to the test
 * uploader.
 */
#ifndef CW_TEST_SCENARIO_H
#define CW_TEST_SCENARIO_H

#include <stdbool.h>
#include <stdint.h>

#include "autolist.h"
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
 * succeeds, attachments are declined, and the library is enabled.
 */
typedef struct {
	const char* status;    /**< Status the test uploader returns: "ok", "retry", or "drop". `NULL` means "ok". */
	bool want_attachments; /**< Value the test uploader reports for `want_attachments`. */
	bool disable;          /**< Run the child with `CW_DISABLE=1`. */
	uint32_t hang_timeout_ms; /**< Passed to cw_config_t::hang_timeout_ms; 0 keeps the library default. */
} test_run_opts_t;

#define TEST_MAX_EVENTS 8

/**
 * Outcome of one scenario run.
 *
 * Every uploader call is one event, in call order. An envelope event
 * looks like `{"call":"envelope","id":...,"attempts":...,"token":...,
 * "attachments":[...],"envelope":{...}}`; an attachment event like
 * `{"call":"attachment","id":...,"path":...,"copy":...,"size":...}`.
 */
typedef struct {
	char dir[256];        /**< Directory holding the report store and the uploader log. */
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
