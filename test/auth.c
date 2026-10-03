/**
 * @file auth.c
 * Identity: how a proof becomes a token, which token a report carries,
 * and the install id every report names.
 */
#include <stdio.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

static int* volatile bad_ptr = (int*)TEST_BAD_ADDRESS;

CW_SCENARIO(crash_now) {
	cw_set_state("mode", "crash_now");
	*bad_ptr = 1;
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

static bool
is_uuid(const char* s) {
	if (s == NULL || strlen(s) != 36) {
		return false;
	}
	for (size_t i = 0; i < 36; ++i) {
		bool dash = i == 8 || i == 13 || i == 18 || i == 23;
		bool hex = (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f');
		if (dash ? s[i] != '-' : !hex) {
			return false;
		}
	}
	return true;
}

/**
 * The install id is a uuid of its own, distinct from the report id, and
 * is written whether or not the run authenticated.
 */
BTEST(auth, envelope_carries_install_id) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(crash_now));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(test_event_is(ev, "report", NULL));
	const char* id = test_json_str(ev, "/envelope/install_id");
	BTEST_EXPECT_EX(is_uuid(id), "install_id: %s", id != NULL ? id : "(null)");
	BTEST_EXPECT(strcmp(id, test_json_str(ev, "/envelope/report_id")) != 0);
	BTEST_EXPECT(test_run_has(run, "install_id"));
}

/**
 * The id lives in the report directory: a kept directory keeps it, a
 * fresh one gets a new one.
 */
BTEST(auth, install_id_follows_report_dir) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(crash_now));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	char first[64];
	snprintf(first, sizeof(first), "%s", test_json_str(run->events[0], "/envelope/install_id"));
	BTEST_ASSERT(is_uuid(first));

	run = RUN_SCENARIO_WITH(SCENARIO_REF(crash_now), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/envelope/install_id"), first) == 0);

	run = RUN_SCENARIO(SCENARIO_REF(crash_now));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	const char* fresh = test_json_str(run->events[0], "/envelope/install_id");
	BTEST_EXPECT(is_uuid(fresh));
	BTEST_EXPECT(strcmp(fresh, first) != 0);
}

/* }}} */
