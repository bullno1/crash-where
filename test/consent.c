/**
 * @file consent.c
 * Consent: nothing is sent, the proof included, until the player agrees,
 * and each answer does what it says.
 */
#include <stdio.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

static const char* const consent_names[] = { "ask", "always", "never", "once" };
static const char* const kind_names[] = { "crash", "hang", "abnormal_exit" };

/**
 * Log what a consent screen would be shown, so the runner can check
 * the summary the library computes from the store.
 */
static void
record_summary(void) {
	cw_consent_summary_t s;
	bool pending = cw_consent_pending(&s);
	yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_str(doc, root, "call", "pending");
	yyjson_mut_obj_add_bool(doc, root, "pending", pending);
	yyjson_mut_obj_add_int(doc, root, "count", s.count);
	yyjson_mut_obj_add_str(doc, root, "kind", kind_names[s.newest_kind]);
	yyjson_mut_obj_add_sint(doc, root, "time", s.newest_time);
	yyjson_mut_obj_add_str(doc, root, "decision", consent_names[cw_consent_get()]);
	test_write_event(doc);
}

CW_SCENARIO(summary_then_exit) {
	record_summary();
	cw_shutdown(0);
}

CW_SCENARIO(once_then_exit) {
	record_summary();
	cw_consent_set(CW_CONSENT_ONCE);
	cw_shutdown(0);
}

static int* volatile null_ptr;

CW_SCENARIO(consent_crash) {
	cw_set_state("mode", "consent_crash");
	*null_ptr = 1;
}

CW_SCENARIO(consent_exit) {
	cw_shutdown(0);
}

CW_SCENARIO(consent_vanish) {
	_Exit(3);
}

/* }}} */

/* Checks (runner side) {{{ */

/**
 * First word of a file in the report directory, or an empty string.
 */
static void
store_word(const test_run_t* run, const char* name, char out[16]) {
	char path[512];
	snprintf(path, sizeof(path), "%s/report/%s", run->dir, name);
	out[0] = '\0';
	FILE* f = fopen(path, "rb");
	if (f != NULL) {
		if (fscanf(f, "%15s", out) != 1) {
			out[0] = '\0';
		}
		fclose(f);
	}
}

static btest_suite_t consent = {
	.name = "consent",
	.cleanup_per_test = test_run_cleanup,
};

BTEST(consent, ask_keeps_report) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip");
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".ok"), 0);
	BTEST_EXPECT(!test_run_has(run, "consent"));
}

BTEST(consent, ask_holds_proof_back) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .auth = "proof");
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT(test_run_has(run, "proof"));
	BTEST_EXPECT(!test_run_has(run, "token"));
}

BTEST(consent, summary_counts_waiting_reports) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_vanish), .consent = "skip");
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(summary_then_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/call"), "pending") == 0);
	BTEST_EXPECT(yyjson_get_bool(test_json_get(ev, "/pending")));
	BTEST_EXPECT_EQUAL("%d", (int)yyjson_get_int(test_json_get(ev, "/count")), 1);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/kind"), "abnormal_exit") == 0);
	BTEST_EXPECT_RELATION("%lld", (long long)yyjson_get_sint(test_json_get(ev, "/time")), >, 0LL);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/decision"), "ask") == 0);
	/* Still waiting: a summary decides nothing. */
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
}

BTEST(consent, once_releases_waiting_reports) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(once_then_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/kind"), "crash") == 0);
	BTEST_EXPECT(test_event_is(run->events[1], "report", NULL));
	BTEST_EXPECT(strcmp(test_json_str(run->events[1], "/envelope/state/mode"), "consent_crash") == 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".ok"), 0);
	BTEST_EXPECT(!test_run_has(run, "consent"));
}

/** A one-shot approval that could not be sent is retried without asking again. */
BTEST(consent, once_sticks_to_released_reports) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip");
	BTEST_ASSERT(run != NULL);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(once_then_exit), .consent = "skip", .keep = true, .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	BTEST_EXPECT(test_event_is(run->events[1], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".ok"), 1);

	/* The approved report goes out when the next run ends; the new one waits. */
	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(test_event_is(run->events[0], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".ok"), 0);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(summary_then_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT_EQUAL("%d", (int)yyjson_get_int(test_json_get(run->events[0], "/count")), 1);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
}

BTEST(consent, never_purges_and_stops_reporting) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_exit), .consent = "never", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	char word[16];
	store_word(run, "consent", word);
	BTEST_EXPECT_EX(strcmp(word, "never") == 0, "consent file holds '%s'", word);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(summary_then_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(!yyjson_get_bool(test_json_get(run->events[0], "/pending")));
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/decision"), "never") == 0);
}

BTEST(consent, always_persists) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(consent_exit));
	BTEST_ASSERT(run != NULL);
	char word[16];
	store_word(run, "consent", word);
	BTEST_EXPECT_EX(strcmp(word, "always") == 0, "consent file holds '%s'", word);

	/* The stored decision alone lets the next run send. */
	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(test_event_is(run->events[0], "report", NULL));

	/* A stored decision settles a waiting report, so no screen is due. */
	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .keep = true, .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);
	run = RUN_SCENARIO_WITH(SCENARIO_REF(summary_then_exit), .consent = "skip", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	BTEST_EXPECT(!yyjson_get_bool(test_json_get(run->events[0], "/pending")));
	BTEST_EXPECT_EQUAL("%d", (int)yyjson_get_int(test_json_get(run->events[0], "/count")), 0);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/decision"), "always") == 0);
	BTEST_EXPECT(test_event_is(run->events[1], "report", NULL));

	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_exit), .consent = "ask", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!test_run_has(run, "consent"));
}

BTEST(consent, dialog_once) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .dialog = "once");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	yyjson_doc* dlg = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(dlg, "/call"), "dialog") == 0);
	BTEST_EXPECT_EQUAL("%d", (int)yyjson_get_int(test_json_get(dlg, "/count")), 1);
	BTEST_EXPECT(strcmp(test_json_str(dlg, "/kind"), "crash") == 0);
	BTEST_EXPECT(test_event_is(run->events[1], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	BTEST_EXPECT(!test_run_has(run, "consent"));
}

BTEST(consent, dialog_never) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .dialog = "never");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/call"), "dialog") == 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	char word[16];
	store_word(run, "consent", word);
	BTEST_EXPECT_EX(strcmp(word, "never") == 0, "consent file holds '%s'", word);
}

BTEST(consent, dialog_dismissed) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip", .dialog = "ask");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/call"), "dialog") == 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".ok"), 0);
}

/** A run without a report of its own leaves the backlog to the in-game prompt. */
BTEST(consent, dialog_only_after_own_report) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_crash), .consent = "skip");
	BTEST_ASSERT(run != NULL);

	run = RUN_SCENARIO_WITH(SCENARIO_REF(consent_exit), .consent = "skip", .dialog = "always", .keep = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 1);
}

/* }}} */
