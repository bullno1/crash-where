/**
 * @file main.c
 * Test runner, or crash fixture when started by the runner.
 *
 * Usage: cw_test [suite [test]]
 */
#include <stdlib.h>
#include <string.h>
#include <blog.h>
#include <btest.h>
#include "scenario.h"

#define BTEST_LOG_DEPTH 1 /* test/main.c */

int
main(int argc, const char* argv[]) {
	/* Set by a scenario on the helper that stops it from outside; it inherits the scenario variable too. */
	const char* stop = getenv("CW_TEST_STOP");
	if (stop != NULL) {
		return test_stop_helper_main(stop);
	}
	/* Set by the runner on the child; the watcher inherits it and never returns from cw_init. */
	const char* scenario = getenv("CW_TEST_SCENARIO");
	if (scenario != NULL) {
		return test_fixture_main(scenario);
	}

	const char* suite_filter = argc > 1 ? argv[1] : NULL;
	const char* test_filter = argc > 2 ? argv[2] : NULL;

	blog_init(&(blog_options_t){
		.current_filename = __FILE__,
		.current_depth_in_project = BTEST_LOG_DEPTH,
	});
	blog_add_file_logger(BLOG_LEVEL_TRACE, &(blog_file_logger_options_t){
		.file = stderr,
		.with_colors = true,
	});

	int num_tests = 0;
	int num_failed = 0;
	BTEST_FOREACH(test) {
		if (suite_filter != NULL && strcmp(suite_filter, test->suite->name) != 0) {
			continue;
		}
		if (test_filter != NULL && strcmp(test_filter, test->name) != 0) {
			continue;
		}
		++num_tests;
		BLOG_INFO("---- %s/%s: Running ----", test->suite->name, test->name);
		if (btest_run(test)) {
			BLOG_INFO("---- %s/%s: Passed  ----", test->suite->name, test->name);
		} else {
			BLOG_ERROR("---- %s/%s: Failed  ----", test->suite->name, test->name);
			++num_failed;
		}
	}
	BLOG_INFO("%d/%d tests passed", num_tests - num_failed, num_tests);
	return num_failed;
}

#define BLIB_IMPLEMENTATION
#include <blog.h>
#include <btest.h>
