/**
 * @file auth.c
 * Authentication: how a proof becomes a token, which token a report
 * carries, and what a refused one costs.
 */
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

static int* volatile null_ptr;

CW_SCENARIO(crash_now) {
	cw_set_state("mode", "crash_now");
	*null_ptr = 1;
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t auth = {
	.name = "auth",
	.cleanup_per_test = test_run_cleanup,
};

BTEST(auth, watcher_exchanges_proof) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .auth = "proof");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	yyjson_doc* ex = run->events[0];
	BTEST_EXPECT(test_event_is(ex, "auth", NULL));
	BTEST_EXPECT(strcmp(test_json_str(ex, "/content_type"), "application/json") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ex, "/envelope/store"), "test") == 0);
	/* "proof-bytes" in base64. */
	BTEST_EXPECT(strcmp(test_json_str(ex, "/envelope/proof"), "cHJvb2YtYnl0ZXM=") == 0);
	yyjson_doc* ev = run->events[1];
	BTEST_EXPECT(test_event_is(ev, "report", "watcher-token"));
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/auth"), "1") == 0);
	BTEST_EXPECT(!test_run_has(run, "proof"));
	BTEST_EXPECT(test_run_has(run, "token"));
}

BTEST(auth, game_supplies_token) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .auth = "token");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(test_event_is(ev, "report", "local-token"));
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/auth"), "1") == 0);
	BTEST_EXPECT(test_run_has(run, "token"));
	BTEST_EXPECT(!test_run_has(run, "proof"));
}

BTEST(auth, expired_token_is_not_sent) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .auth = "expired");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(test_event_is(run->events[0], "report", NULL));
}

/**
 * The decision and the proof may reach the watcher in either order, so
 * a failing exchange is tried once for each; the report itself never
 * triggers another attempt.
 */
BTEST(auth, proof_kept_when_exchange_fails) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .auth = "proof", .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_RELATION("%d", run->num_events, >=, 2);
	BTEST_ASSERT_RELATION("%d", run->num_events, <=, 3);
	for (int i = 0; i + 1 < run->num_events; ++i) {
		BTEST_EXPECT(test_event_is(run->events[i], "auth", NULL));
	}
	BTEST_EXPECT(test_event_is(run->events[run->num_events - 1], "report", NULL));
	BTEST_EXPECT(test_run_has(run, "proof"));
	BTEST_EXPECT(!test_run_has(run, "token"));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
}

BTEST(auth, refused_token_bounces_once) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .auth = "token", .status = "unauthorized");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	BTEST_EXPECT(test_event_is(run->events[0], "report", "local-token"));
	BTEST_EXPECT(test_event_is(run->events[1], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
}

/* }}} */
