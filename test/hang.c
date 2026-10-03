/**
 * @file hang.c
 * Hang detection: the detector's rules against synthetic time, then
 * scenarios that stall in a child and check the report.
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <blog.h>
#include "btest.h"
#include "cw.h"
#include "internal.h"
#include "scenario.h"

/* Detector rules (in process) {{{ */

static btest_suite_t hang = {
	.name = "hang",
	.cleanup_per_test = test_run_cleanup,
};

#define STEP(H, COUNT, NOW) ((int)cw_hang_step(&(H), (COUNT), (NOW), 100))

BTEST(hang, unarmed_until_first_tick) {
	cw_hang_t h = { 0 };
	BTEST_EXPECT_EQUAL("%d", STEP(h, 0, 0), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 0, 10000), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 10000), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 10100), CW_HANG_REPORT);
}

BTEST(hang, reports_at_timeout) {
	cw_hang_t h = { 0 };
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 10), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 109), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 110), CW_HANG_REPORT);
}

BTEST(hang, ticks_restart_clock) {
	cw_hang_t h = { 0 };
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 0), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 2, 90), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 3, 180), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 3, 279), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 3, 280), CW_HANG_REPORT);
}

BTEST(hang, reports_once_per_stall) {
	cw_hang_t h = { 0 };
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 0), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 100), CW_HANG_REPORT);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 200), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 5000), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 2, 5010), CW_HANG_RECOVERED);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 2, 5100), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 2, 5110), CW_HANG_REPORT);
}

BTEST(hang, capped_per_session) {
	cw_hang_t h = { 0 };
	uint64_t now = 0;
	uint64_t count = 1;
	for (int i = 0; i < CW_HANG_MAX_REPORTS; ++i) {
		int on_tick = i == 0 ? CW_HANG_NONE : CW_HANG_RECOVERED;
		BTEST_EXPECT_EQUAL("%d", STEP(h, count, now), on_tick);
		now += 100;
		BTEST_EXPECT_EQUAL("%d", STEP(h, count, now), CW_HANG_REPORT);
		now += 10;
		++count;
	}
	BTEST_EXPECT_EQUAL("%d", STEP(h, count, now), CW_HANG_RECOVERED);
	now += 100;
	BTEST_EXPECT_EQUAL("%d", STEP(h, count, now), CW_HANG_NONE);
	now += 10;
	++count;
	/* A stall that was never reported has nothing to recover from. */
	BTEST_EXPECT_EQUAL("%d", STEP(h, count, now), CW_HANG_NONE);
}

BTEST(hang, reset_restarts_clock) {
	cw_hang_t h = { 0 };
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 0), CW_HANG_NONE);
	cw_hang_reset(&h, 90);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 150), CW_HANG_NONE);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 1, 190), CW_HANG_REPORT);
}

BTEST(hang, poll_interval_clamped) {
	BTEST_EXPECT_EQUAL("%d", cw_hang_poll_ms(0), 10);
	BTEST_EXPECT_EQUAL("%d", cw_hang_poll_ms(100), 25);
	BTEST_EXPECT_EQUAL("%d", cw_hang_poll_ms(10000), 1000);
}

BTEST(hang, reset_does_not_arm) {
	cw_hang_t h = { 0 };
	cw_hang_reset(&h, 0);
	BTEST_EXPECT_EQUAL("%d", STEP(h, 0, 1000), CW_HANG_NONE);
}

/* }}} */

/* Scenarios (child side) {{{ */

/** Seconds a stalling scenario waits for its report before giving up. */
#define STALL_CAP_S 20

/** Exit status of a stalling scenario that was never reported. */
#define STALL_EXIT_UNREPORTED 4

static volatile int sink;

/* Resolved at run time so the compiler cannot prove the store undefined and delete it. */
static int* volatile bad_ptr = (int*)TEST_BAD_ADDRESS;

static void
record_addr(const char* key, uintptr_t addr) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIxPTR, addr);
	cw_set_state(key, buf);
}

/**
 * Spin until the watcher has uploaded a report, which the test transport
 * records in the events file, so the stall lasts exactly as long as
 * detection takes. The file is checked only every 64K iterations so
 * the watcher's snapshot lands in this function nearly every time.
 */
TEST_NOINLINE static void
stall_until_reported(void) {
	record_addr("ret1", TEST_RETURN_ADDRESS());
	char path[512];
	snprintf(path, sizeof(path), "%s/events.jsonl", getenv("CW_TEST_OUT"));
	time_t start = time(NULL);
	for (uint64_t i = 1;; ++i) {
		sink++;
		if ((i & 0xffff) != 0) {
			continue;
		}
		if (test_file_size(path) > 0) {
			break;
		}
		if (time(NULL) - start > STALL_CAP_S) {
			exit(STALL_EXIT_UNREPORTED);
		}
	}
	sink++;
}

CW_SCENARIO(stall_then_exit) {
	cw_set_state("mode", "stall_then_exit");
	record_addr("base", test_image_base());
	cw_breadcrumb("test", "about to stall");
	for (int i = 0; i < 3; ++i) {
		cw_heartbeat();
	}
	stall_until_reported();
	cw_shutdown();
}

CW_SCENARIO(stall_then_crash) {
	cw_set_state("mode", "stall_then_crash");
	record_addr("base", test_image_base());
	for (int i = 0; i < 3; ++i) {
		cw_heartbeat();
	}
	stall_until_reported();
	*bad_ptr = 1;
}

CW_SCENARIO(no_heartbeat) {
	cw_set_state("mode", "no_heartbeat");
	test_sleep_ms(500);
	cw_shutdown();
}

CW_SCENARIO(steady_heartbeat) {
	cw_set_state("mode", "steady_heartbeat");
	for (int i = 0; i < 150; ++i) {
		cw_heartbeat();
		test_sleep_ms(10);
	}
	cw_shutdown();
}

CW_SCENARIO(stopped) {
	cw_set_state("mode", "stopped");
	for (int i = 0; i < 3; ++i) {
		cw_heartbeat();
	}
	if (!test_stop_self(500)) {
		exit(STALL_EXIT_UNREPORTED);
	}
	cw_heartbeat();
	cw_shutdown();
}

/* }}} */

/* Checks (runner side) {{{ */

/**
 * The return address the stalling function recorded must be among the
 * frames. Frame 0 may be the function itself or, when the snapshot
 * caught it inside a libc call, that leaf; either way the walk reaches
 * the caller.
 */
static void
check_stall_frames(yyjson_doc* ev) {
	size_t num_frames = yyjson_arr_size(test_json_get(ev, "/envelope/frames"));
	BTEST_ASSERT_RELATION("%zu", num_frames, >=, 2);
	uintptr_t want = test_state_hex(ev, "ret1") - 1; /* The unwinder points inside the call instruction. */
	size_t i = 0;
	while (i < num_frames && test_frame_addr(ev, i) != want) {
		++i;
	}
	BTEST_EXPECT_EX(i < num_frames, "return address %" PRIxPTR " missing from the frames", want);
}

static void
check_hang_envelope(yyjson_doc* ev, const char* mode) {
	BTEST_ASSERT(strcmp(test_json_str(ev, "/url"), "http://127.0.0.1:9/v1/cw-test/report") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), "HANG") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/message_norm"), "no heartbeat for <N> ms") == 0);
	BTEST_EXPECT_RELATION("%" PRIu64, yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread")), >, 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/mode"), mode) == 0);
}

BTEST(hang, stall_then_exit) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(stall_then_exit), .hang_timeout_ms = 100);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	check_hang_envelope(ev, "stall_then_exit");
	yyjson_val* crumbs = test_json_get(ev, "/envelope/breadcrumbs");
	BTEST_ASSERT_EQUAL("%zu", yyjson_arr_size(crumbs), (size_t)1);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/breadcrumbs/0/m"), "about to stall") == 0);
	check_stall_frames(ev);
}

BTEST(hang, stall_then_crash) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(stall_then_crash), .hang_timeout_ms = 100);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 2);
	check_hang_envelope(run->events[0], "stall_then_crash");
	check_stall_frames(run->events[0]);
	yyjson_doc* crash = run->events[1];
	BTEST_EXPECT(strcmp(test_json_str(crash, "/envelope/exception/type"), TEST_EXC_SEGV) == 0);
	BTEST_EXPECT_RELATION("%zu", yyjson_arr_size(test_json_get(crash, "/envelope/frames")), >=, 1);
	/* Two reports from one session must not share an id. */
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/envelope/report_id"), test_json_str(crash, "/envelope/report_id")) != 0);
}

BTEST(hang, no_heartbeat) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(no_heartbeat), .hang_timeout_ms = 100);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

BTEST(hang, steady_heartbeat) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(steady_heartbeat), .hang_timeout_ms = 1000);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

BTEST(hang, stopped) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(stopped), .hang_timeout_ms = 100);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

/* }}} */
