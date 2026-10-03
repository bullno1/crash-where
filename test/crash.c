/**
 * @file crash.c
 * Crash handling: each scenario dies in a child and the suite checks
 * the report the watcher produced.
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

/**
 * Upper bound on the size of a crashing marker function. The child can
 * record where such a function starts but not where it ends, so frame 0
 * is checked to lie within this many bytes of the start.
 */
#define MARKER_MAX_BYTES 256

static volatile int sink;

/* Resolved at run time so the compiler cannot prove the store undefined and delete it. */
static int* volatile bad_ptr = (int*)TEST_BAD_ADDRESS;

/**
 * Record an address in a state slot, so it reaches the runner inside
 * the envelope for comparison against the recorded frames.
 */
static void
record_addr(const char* key, uintptr_t addr) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIxPTR, addr);
	cw_set_state(key, buf);
}

/* The `sink++` after each call keeps it from becoming a tail jump. */

TEST_NOINLINE static void
write_null(void) {
	record_addr("ret1", TEST_RETURN_ADDRESS());
	*bad_ptr = 1;
	sink++;
}

/* Makes no calls, so GCC may omit its frame pointer. */
TEST_NOINLINE static void
write_null_leaf(void) {
	*bad_ptr = 1;
	sink++;
}

TEST_NOINLINE static void
level_two(void (*crash)(void)) {
	record_addr("ret2", TEST_RETURN_ADDRESS());
	crash();
	sink++;
}

TEST_NOINLINE static void
level_three(void (*crash)(void)) {
	record_addr("ret3", TEST_RETURN_ADDRESS());
	level_two(crash);
	sink++;
}

/* The crash starts in the host, with C frames below it. */
TEST_NOINLINE static void
throw_from_host(void) {
	test_host_throw();
	sink++;
}

TEST_NOINLINE static int
recurse(int depth) {
	volatile char pad[1024];
	pad[0] = (char)depth;
	if (sink < 0) {
		return depth; /* Never taken; keeps the compiler from proving the recursion infinite. */
	}
	return recurse(depth + 1) + pad[0];
}

CW_SCENARIO(null_write) {
	cw_breadcrumb("test", "about to crash");
	cw_set_state("mode", "null_write");
	cw_set_env("renderer", "none");
	cw_set_env("renderer", "test");
	record_addr("base", test_image_base());
	record_addr("fn0", (uintptr_t)write_null);
	level_three(write_null);
}

CW_SCENARIO(null_write_displaced) {
	cw_set_state("mode", "null_write_displaced");
	record_addr("base", test_image_base());
	record_addr("fn0", (uintptr_t)write_null);
	test_displace_crash_handler();
	cw_heartbeat();
	level_three(write_null);
}

CW_SCENARIO(host_throw) {
	cw_set_state("mode", "host_throw");
	level_three(throw_from_host);
}

CW_SCENARIO(null_write_leaf) {
	cw_set_state("mode", "null_write_leaf");
	record_addr("base", test_image_base());
	record_addr("fn0", (uintptr_t)write_null_leaf);
	record_addr("fn1", (uintptr_t)level_two); /* The leaf cannot record its own return address. */
	level_three(write_null_leaf);
}

CW_SCENARIO(abort) {
	cw_set_state("mode", "abort");
	abort();
}

CW_SCENARIO(stack_overflow) {
	cw_set_state("mode", "stack_overflow");
	sink = recurse(0);
}

static void
attached_overflow(void) {
	cw_attach_thread();
	sink = recurse(0);
}

/* The breadcrumb records the main thread's id, to tell the crashing thread from it. */
CW_SCENARIO(thread_stack_overflow) {
	cw_set_state("mode", "thread_stack_overflow");
	cw_breadcrumb("test", "starting thread");
	test_run_thread(attached_overflow);
}

static void
unattached_null_write(void) {
	level_three(write_null);
}

CW_SCENARIO(thread_null_write) {
	cw_set_state("mode", "thread_null_write");
	record_addr("base", test_image_base());
	record_addr("fn0", (uintptr_t)write_null);
	cw_breadcrumb("test", "starting thread");
	test_run_thread(unattached_null_write);
}

CW_SCENARIO(clean_exit) {
	cw_set_state("mode", "clean_exit");
	cw_shutdown();
}

CW_SCENARIO(exit_without_shutdown) {
	cw_set_state("mode", "exit_without_shutdown");
	_Exit(3);
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t crash = {
	.name = "crash",
	.cleanup_per_test = test_run_cleanup,
};

/**
 * Check the module base against the image start the child recorded,
 * then frames 1 to 3 against the three return addresses. Where a
 * scenario could not record `retN` it records `fnN`, the function
 * that frame returns into, and the frame must lie inside it.
 */
static void
check_frames(yyjson_doc* ev) {
	uintptr_t base = test_state_hex(ev, "base");
	BTEST_ASSERT(base != 0);
	size_t num_frames = yyjson_arr_size(test_json_get(ev, "/envelope/frames"));
	BTEST_ASSERT_RELATION("%zu", num_frames, >=, 4);

	uintptr_t fn0 = test_state_hex(ev, "fn0");
	uintptr_t pc = test_frame_addr(ev, 0);
	BTEST_ASSERT_EX(pc != 0, "frame 0 is not in the module at base %" PRIxPTR, base);
	BTEST_EXPECT_EX(
		pc >= fn0 && pc < fn0 + MARKER_MAX_BYTES,
		"frame 0 at %" PRIxPTR ", crashing function at %" PRIxPTR, pc, fn0
	);

	static const char* const rets[] = { "ret1", "ret2", "ret3" };
	static const char* const fns[] = { "fn1", "fn2", "fn3" };
	for (size_t k = 0; k < 3; ++k) {
		size_t i = k + 1;
		uintptr_t got = test_frame_addr(ev, i);
		uintptr_t want = test_state_hex(ev, rets[k]);
		if (want != 0) {
			want -= 1; /* The unwinder points inside the call instruction. */
			BTEST_EXPECT_EX(got == want, "frame %zu is %" PRIxPTR ", want %" PRIxPTR, i, got, want);
			continue;
		}
		uintptr_t fn = test_state_hex(ev, fns[k]);
		BTEST_ASSERT_EX(fn != 0, "scenario recorded neither %s nor %s", rets[k], fns[k]);
		BTEST_EXPECT_EX(
			got >= fn && got < fn + MARKER_MAX_BYTES,
			"frame %zu at %" PRIxPTR ", want inside %s at %" PRIxPTR, i, got, fns[k], fn
		);
	}
}

/**
 * Path of the pending envelope, rebuilt from fields the child reported.
 */
static void
pending_path(const test_run_t* run, yyjson_doc* ev, char* out, size_t cap) {
	snprintf(
		out, cap, "%s/report/pending/%" PRIu64 "_c_%s_%s.json",
		run->dir,
		yyjson_get_uint(test_json_get(ev, "/envelope/sent_at")),
		test_json_str(ev, "/envelope/client_fp"),
		test_json_str(ev, "/envelope/report_id")
	);
}

static bool
file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if (f != NULL) {
		fclose(f);
	}
	return f != NULL;
}

BTEST(crash, null_write) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(null_write));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);

	yyjson_doc* ev = run->events[0];
	BTEST_ASSERT(strcmp(test_json_str(ev, "/url"), "http://127.0.0.1:9/v1/cw-test/report") == 0);
	BTEST_EXPECT_EQUAL("%d", (int)yyjson_get_int(test_json_get(ev, "/attempts")), 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/method"), "POST") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/app/name"), "cw-test") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_SEGV) == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/mode"), "null_write") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/env/renderer"), "test") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/env/collected_at_init"), "1") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/env/collected_at_report"), "1") == 0);
	BTEST_EXPECT_EQUAL("%zu", yyjson_obj_size(test_json_get(ev, "/envelope/env")), (size_t)3);

	yyjson_val* crumbs = test_json_get(ev, "/envelope/breadcrumbs");
	BTEST_ASSERT_EQUAL("%zu", yyjson_arr_size(crumbs), (size_t)1);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/breadcrumbs/0/c"), "test") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/breadcrumbs/0/m"), "about to crash") == 0);

	check_frames(ev);

	char path[512];
	pending_path(run, ev, path, sizeof(path));
	BTEST_EXPECT_EX(!file_exists(path), "%s still exists after a successful upload", path);
}

BTEST(crash, null_write_displaced) {
	if (!test_displace_crash_handler()) {
		BLOG_WARN("skipped: no crash handler recovery on this platform");
		return;
	}
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(null_write_displaced));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_SEGV) == 0);
	check_frames(ev);
}

BTEST(crash, null_write_leaf) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(null_write_leaf));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_SEGV) == 0);
	check_frames(ev);
}

BTEST(crash, abort) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(abort));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGABRT);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_ABORT) == 0);
	BTEST_EXPECT_RELATION("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), >=, 1);
}

/** An error thrown by the host through C frames is a crash: named frame first, C caller below. */
BTEST(crash, host_throw) {
	if (!test_host_can_throw()) {
		BLOG_WARN("skipped: the host cannot throw through C frames on this platform");
		return;
	}
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(host_throw));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), "TypeError") == 0);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/message_raw"), "host") == 0);
	BTEST_ASSERT_RELATION("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), >=, 2);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/frames/0/module"), "javascript:test_web_host_throw") == 0);
	BTEST_EXPECT_EQUAL("%" PRIu64, yyjson_get_uint(test_json_get(ev, "/envelope/frames/0/offset")), (uint64_t)0);
	BTEST_EXPECT(test_json_str(ev, "/envelope/frames/0/raw") != NULL);
	const char* below = test_json_str(ev, "/envelope/frames/1/module");
	BTEST_EXPECT_EX(
		below != NULL && strlen(below) > 5 && strcmp(below + strlen(below) - 5, ".wasm") == 0,
		"frame 1 is in %s, want the module", below != NULL ? below : "(none)"
	);
	BTEST_EXPECT_RELATION("%" PRIu64, yyjson_get_uint(test_json_get(ev, "/envelope/frames/1/offset")), >, (uint64_t)0);
}

BTEST(crash, stack_overflow) {
	if (test_under_wine()) {
		BLOG_WARN("skipped: Wine cannot deliver a stack overflow to the filter");
		return;
	}
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(stack_overflow));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_STACK_OVERFLOW) == 0);
	BTEST_EXPECT_RELATION("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), >=, 2);
}

/** The crashing thread is not the one that left the breadcrumb. */
static void
expect_other_thread(yyjson_doc* ev) {
	uint64_t crashed = yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread"));
	uint64_t main_tid = yyjson_get_uint(test_json_get(ev, "/envelope/breadcrumbs/0/th"));
	BTEST_EXPECT_EX(main_tid != 0 && crashed != main_tid, "crashed on thread %" PRIu64 ", main is %" PRIu64, crashed, main_tid);
}

BTEST(crash, thread_stack_overflow) {
	if (test_under_wine()) {
		BLOG_WARN("skipped: Wine cannot deliver a stack overflow to the filter");
		return;
	}
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(thread_stack_overflow));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_STACK_OVERFLOW) == 0);
	BTEST_EXPECT_RELATION("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), >=, 2);
	expect_other_thread(ev);
}

/** An unattached thread still reports with its frames. */
BTEST(crash, thread_null_write) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(thread_null_write));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), TEST_EXC_SEGV) == 0);
	expect_other_thread(ev);
	check_frames(ev);
}

BTEST(crash, clean_exit) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(clean_exit));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

BTEST(crash, exit_without_shutdown) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(exit_without_shutdown));
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 3);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), "KILLED") == 0);
	/* The region outlives the game, so state is still there. */
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/mode"), "exit_without_shutdown") == 0);
	BTEST_EXPECT_EQUAL("%zu", yyjson_arr_size(test_json_get(ev, "/envelope/frames")), (size_t)0);
}

BTEST(crash, disabled) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(null_write), .disable = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

BTEST(crash, debugger) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(exit_without_shutdown), .debugger = true);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 3);
	BTEST_EXPECT_EQUAL("%d", run->num_events, 0);
}

BTEST(crash, debugger_forced) {
	const test_run_t* run = RUN_SCENARIO_WITH(
		SCENARIO_REF(exit_without_shutdown), .debugger = true, .force = true
	);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 3);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(strcmp(test_json_str(run->events[0], "/envelope/exception/type"), "KILLED") == 0);
}

BTEST(crash, upload_retry_keeps_report) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(null_write), .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	char path[512];
	pending_path(run, run->events[0], path, sizeof(path));
	BTEST_EXPECT_EX(file_exists(path), "%s missing after a failed upload", path);
}

BTEST(crash, upload_drop_deletes_report) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(null_write), .status = "drop");
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	char path[512];
	pending_path(run, run->events[0], path, sizeof(path));
	BTEST_EXPECT_EX(!file_exists(path), "%s still exists after a rejected upload", path);
}

/* }}} */
