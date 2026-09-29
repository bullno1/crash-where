/**
 * @file crashme.c
 * Test program: initialize the library, then crash on request.
 *
 * Usage: crashme [null|abort|hang|none] [report_dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cw.h"
#include "cw_host.h"
#include "cw_http.h"

#if defined(_WIN32)
#	include <process.h>
#	define getpid _getpid
#	define NOINLINE __declspec(noinline)
#else
#	include <unistd.h>
#	define NOINLINE __attribute__((noinline))
#endif

static volatile int sink;

/* Loaded at run time so the compiler cannot prove the store is undefined and delete it. */
static int* volatile null_ptr;

static void
log_to_stderr(cw_log_level_t level, const char* msg) {
	static const char* const names[] = { "error", "warn", "info", "debug" };
	fprintf(stderr, "[cw %s] %s\n", names[level], msg);
}

NOINLINE static void
crash_here(void) {
	*null_ptr = 42;
	sink++;
}

NOINLINE static void
level_two(void) {
	crash_here();
	sink++;
}

NOINLINE static void
level_three(void) {
	level_two();
	sink++;
}

int
main(int argc, char** argv) {
	const char* mode = argc > 1 ? argv[1] : "null";
	cw_config_t cfg = {
		.app = "crashme",
		.version = "0.0.1",
		.channel = "dev",
		.endpoint = "http://localhost:8080",
		.report_dir = argc > 2 ? argv[2] : NULL,
		.transport = &cw_transport_http,
		.collect_at_init = &cw_collector_host,
		.collect_at_report = &cw_collector_host,
		.log = log_to_stderr,
	};

	cw_init(&cfg);
	/* A real game should ask the player. */
	cw_consent_set(CW_CONSENT_ALWAYS);

	printf("crashme: game pid %d, mode %s\n", (int)getpid(), mode);
	fflush(stdout);

	cw_set_state("mode", mode);
	cw_breadcrumb("main", "about to crash");
	cw_heartbeat();

	if (strcmp(mode, "none") == 0) {
		cw_shutdown(0);
		return 0;
	}
	if (strcmp(mode, "abort") == 0) {
		abort();
	}
	if (strcmp(mode, "hang") == 0) {
		/* Ticked once above, so the watcher reports this after the default timeout. */
		for (;;) {
			sink++;
		}
	}
	level_three();
	return 0;
}
