/**
 * @file drain.c
 * The catch-up drain: reports an earlier run could not send go out once
 * the game has authenticated, or when it exits. The runs rely on the
 * stored decision, since a fresh one would drain at once.
 */
#include <stdio.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

static int* volatile null_ptr;

CW_SCENARIO(drain_crash) {
	cw_set_state("mode", "drain_crash");
	*null_ptr = 1;
}

CW_SCENARIO(drain_exit) {
	cw_shutdown(0);
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t drain = {
	.name = "drain",
	.cleanup_per_test = test_run_cleanup,
};

BTEST(drain, backlog_drains_after_auth) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(drain_crash), .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(drain_exit), .auth = "proof", .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	BTEST_EXPECT(test_event_is(run->events[0], "auth", NULL));
	BTEST_EXPECT(test_event_is(run->events[1], "report", "watcher-token"));
	BTEST_EXPECT(strcmp(test_json_str(run->events[1], "/envelope/state/mode"), "drain_crash") == 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
}

BTEST(drain, backlog_drains_at_exit_without_auth) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(drain_crash), .status = "retry");
	BTEST_ASSERT(run != NULL);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(drain_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(test_event_is(run->events[0], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
}

BTEST(drain, stale_report_is_deleted) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(drain_exit));
	BTEST_ASSERT(run != NULL);
	/* An envelope written at the epoch, long past the retention limit. */
	char path[512];
	snprintf(path, sizeof(path), "%s/report/pending/1_c_0000000000000000_00000000-0000-4000-8000-000000000000.json", run->dir);
	FILE* f = fopen(path, "wb");
	BTEST_ASSERT(f != NULL);
	fputs("{}\n", f);
	fclose(f);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(drain_exit), .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
}

/* }}} */
