/**
 * @file main.c
 * Test runner, or crash fixture when started by the runner.
 *
 * Usage: cw_test [suite [test]]
 *
 * `CW_TEST_SKIP` lists tests to leave out as `suite/test`, separated by
 * spaces. A test named on the command line runs regardless.
 */
#include <stdlib.h>
#include <string.h>
#include <blog.h>
#include <btest.h>
#include "scenario.h"

#define BTEST_LOG_DEPTH 1 /* test/main.c */

/**
 * Whether `list` holds `suite/test` as one of its space-separated words.
 */
static bool
test_listed(const char* list, const char* suite, const char* test) {
	size_t suite_len = strlen(suite);
	size_t test_len = strlen(test);
	while (list != NULL && *list != '\0') {
		size_t len = strcspn(list, " ");
		if (
			len == suite_len + 1 + test_len
			&& memcmp(list, suite, suite_len) == 0
			&& list[suite_len] == '/'
			&& memcmp(list + suite_len + 1, test, test_len) == 0
		) {
			return true;
		}
		list += len + strspn(list + len, " ");
	}
	return false;
}

int
main(int argc, const char* argv[]) {
	test_platform_init();
	/* Set by a scenario on the helper that stops or debugs it from outside; it inherits the scenario variable too. */
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

	const char* skip = getenv("CW_TEST_SKIP");
	int num_tests = 0;
	int num_failed = 0;
	int num_skipped = 0;
	BTEST_FOREACH(test) {
		if (suite_filter != NULL && strcmp(suite_filter, test->suite->name) != 0) {
			continue;
		}
		if (test_filter != NULL && strcmp(test_filter, test->name) != 0) {
			continue;
		}
		if (test_filter == NULL && test_listed(skip, test->suite->name, test->name)) {
			BLOG_WARN("---- %s/%s: Skipped ----", test->suite->name, test->name);
			++num_skipped;
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
	if (num_skipped > 0) {
		BLOG_INFO("%d/%d tests passed, %d skipped", num_tests - num_failed, num_tests, num_skipped);
	} else {
		BLOG_INFO("%d/%d tests passed", num_tests - num_failed, num_tests);
	}
	return num_failed;
}

#define BLIB_IMPLEMENTATION
#include <blog.h>
#include <btest.h>
