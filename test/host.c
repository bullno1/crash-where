/**
 * @file host.c
 * The standard collector: a crash with `cw_collector_host` in both
 * slots carries the machine facts of this host.
 */
#include <stdlib.h>
#include <string.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

#if defined(_WIN32)
#	define TEST_HOST_OS "windows"
#else
#	define TEST_HOST_OS "linux"
#endif

/* Scenarios (child side) {{{ */

static int* volatile null_ptr;

CW_SCENARIO(host_crash) {
	cw_set_state("mode", "host_crash");
	*null_ptr = 1;
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t host = {
	.name = "host",
	.cleanup_per_test = test_run_cleanup,
};

/** Number in an env slot, or 0 when absent. */
static unsigned long long
env_number(yyjson_doc* ev, const char* key) {
	char ptr[64];
	snprintf(ptr, sizeof(ptr), "/envelope/env/%s", key);
	const char* s = test_json_str(ev, ptr);
	return s != NULL ? strtoull(s, NULL, 10) : 0;
}

BTEST(host, collects_standard_keys) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(host_crash), .host = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/state/mode"), "host_crash") == 0);

	/* At init, in the game. */
	BTEST_ASSERT(test_json_str(ev, "/envelope/env/os") != NULL);
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/env/os"), TEST_HOST_OS) == 0);
	BTEST_EXPECT(test_json_str(ev, "/envelope/env/os_version") != NULL);
	BTEST_EXPECT(test_json_str(ev, "/envelope/env/arch") != NULL);
	BTEST_EXPECT_RELATION("%llu", env_number(ev, "cpu_cores"), >, 0ull);
	BTEST_EXPECT_RELATION("%llu", env_number(ev, "ram_mb"), >, 0ull);

	/* At report time, in the watcher, about the parked game. */
	BTEST_EXPECT_RELATION("%llu", env_number(ev, "rss_mb"), >, 0ull);
	BTEST_EXPECT_RELATION("%llu", env_number(ev, "threads"), >, 0ull);
	BTEST_EXPECT_RELATION("%llu", env_number(ev, "free_mb"), >, 0ull);
	BTEST_EXPECT(test_json_str(ev, "/envelope/env/collected_at_init") == NULL);
}

/* }}} */
