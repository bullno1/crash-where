/**
 * @file report.c
 * Reports of failures the game survives: cw_report() in a child that
 * runs on to a clean exit, then the report the watcher produced.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

static btest_suite_t report = {
	.name = "report",
	.cleanup_per_test = test_run_cleanup,
};

/* Scenarios (child side) {{{ */

static void
record_addr(const char* key, uintptr_t addr) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIxPTR, addr);
	cw_set_state(key, buf);
}

/* Reports from a frame whose return address the runner can look for. */
TEST_NOINLINE static void
report_here(const char* type, const char* msg) {
	record_addr("ret1", TEST_RETURN_ADDRESS());
	cw_report(type, msg);
}

CW_SCENARIO(report_then_exit) {
	cw_set_state("mode", "report_then_exit");
	record_addr("base", test_image_base());
	cw_breadcrumb("test", "about to report");
	report_here("DESYNC", "state hash mismatch at tick 42");
	/* Written after the call returned, so it must be missing from the report. */
	cw_breadcrumb("test", "reported");
	cw_shutdown();
}

CW_SCENARIO(report_twice) {
	cw_set_state("mode", "report_twice");
	report_here("DESYNC", "first");
	report_here("DESYNC", "second");
	cw_shutdown();
}

/* }}} */

/* Checks (runner side) {{{ */

static void
check_report_envelope(yyjson_doc* ev, const char* type, const char* msg, const char* mode) {
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), type) == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/message_raw"), msg) == 0);
	BTEST_EXPECT_RELATION("%" PRIu64, yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread")), >, 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/mode"), mode) == 0);
	BTEST_EXPECT_RELATION("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), >=, 1);
}

BTEST(report, survives) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(report_then_exit));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	check_report_envelope(ev, "DESYNC", "state hash mismatch at tick 42", "report_then_exit");
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/message_norm"), "state hash mismatch at tick <N>") == 0);
	yyjson_val* crumbs = test_json_get(ev, "/envelope/breadcrumbs");
	BTEST_ASSERT_EQUAL("%zu", yyjson_arr_size(crumbs), (size_t)1);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/breadcrumbs/0/m"), "about to report") == 0);
}

/** The frames reach past the library into the function that reported. */
BTEST(report, frames_name_the_caller) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(report_then_exit));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	size_t num_frames = yyjson_arr_size(test_json_get(ev, "/envelope/frames"));
	BTEST_ASSERT_RELATION("%zu", num_frames, >=, 2);
	uintptr_t want = test_state_hex(ev, "ret1") - 1; /* The unwinder points inside the call instruction. */
	size_t i = 0;
	while (i < num_frames && test_frame_addr(ev, i) != want) {
		++i;
	}
	BTEST_EXPECT_EX(i < num_frames, "return address %" PRIxPTR " missing from the frames", want);
}

/** The slot is free again once a report is written, so the next one goes through. */
BTEST(report, twice) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(report_twice));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	check_report_envelope(run->events[0], "DESYNC", "first", "report_twice");
	check_report_envelope(run->events[1], "DESYNC", "second", "report_twice");
}

/* }}} */
