/**
 * @file crashme.c
 * Test program: initialize the library, then crash on request.
 *
 * Usage: crashme [null|abort|assert|report|hang|none] [report_dir] [endpoint]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cw.h>
#include <cw_dialog.h>
#include <cw_host.h>
#include <cw_http.h>

#if defined(_WIN32)
#	include <process.h>
#	define getpid _getpid
#	define NOINLINE __declspec(noinline)
#else
#	include <unistd.h>
#	define NOINLINE __attribute__((noinline))
#endif

static volatile int sink;

/*
 * Loaded at run time so the compiler cannot prove the store is undefined
 * and delete it. Address 0 is ordinary memory in Wasm; only a store past
 * the end of memory traps there.
 */
#if defined(__EMSCRIPTEN__)
static int* volatile null_ptr = (int*)0xfffffff0u;
#else
static int* volatile null_ptr;
#endif

static void
log_to_stderr(cw_log_level_t level, const char* msg) {
	static const char* const names[] = { "error", "warn", "info", "debug" };
	fprintf(stderr, "[cw %s] %s\n", names[level], msg);
}

/* Runs in the watcher, after the game is gone. */
static cw_consent_t
ask_player(void* user, const cw_consent_summary_t* summary) {
	(void)user;
	static const char* const what[] = { "crashed", "froze", "ended unexpectedly" };
	char message[256];
	snprintf(
		message, sizeof(message),
		"crashme %s.\nSend %d report%s to the developers?",
		what[summary->newest_kind], summary->count, summary->count == 1 ? "" : "s"
	);
	return cw_dialog_show(&(cw_dialog_desc_t){
		.title = "crashme",
		.message = message,
		.buttons = {
			{ "Send", CW_CONSENT_ONCE },
			{ "Always send", CW_CONSENT_ALWAYS },
			{ "Don't send", CW_CONSENT_ASK },
		},
	});
}

static const cw_consent_dialog_t dialog = { .show = ask_player };

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
		.endpoint = argc > 3 ? argv[3] : "http://localhost:8787",
		.report_dir = argc > 2 ? argv[2] : NULL,
		.transport = &cw_transport_http,
		.collect_at_init = &cw_collector_host,
		.collect_at_report = &cw_collector_host,
		.consent_dialog = &dialog,
		.log = log_to_stderr,
	};

	cw_init(&cfg);

	printf("crashme: game pid %d, mode %s\n", (int)getpid(), mode);
	fflush(stdout);

	cw_set_state("mode", mode);
	cw_breadcrumb("main", "about to crash");
	cw_heartbeat();

	if (strcmp(mode, "none") == 0) {
		cw_shutdown();
		return 0;
	}
	if (strcmp(mode, "abort") == 0) {
		abort();
	}
	if (strcmp(mode, "assert") == 0) {
		cw_abort("ASSERT", "mode != assert");
	}
	if (strcmp(mode, "report") == 0) {
		cw_report("DESYNC", "state hash mismatch");
		cw_shutdown();
		return 0;
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
