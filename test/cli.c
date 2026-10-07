/**
 * @file cli.c
 * The `cwsym` command line driven in-process: dispatch and usage, each
 * command against the golden synthetic table, and upload against the
 * loopback server.
 */
#include <stdlib.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "cli.h"
#include "http_server.h"
#include "platform.h"

#define FIXTURE TEST_FIXTURE_DIR "/synthetic.cwsym"
#define OUT_PATH "work/cli/out.txt"
#define ERR_PATH "work/cli/err.txt"

static test_http_server_t* server;
static const char* token = "ci-token";
static char* out_text;
static char* err_text;

static void
cleanup(void) {
	if (server != NULL) {
		test_http_stop(server);
		server = NULL;
	}
	free(out_text);
	free(err_text);
	out_text = err_text = NULL;
	token = "ci-token";
}

static btest_suite_t cli = {
	.name = "cli",
	.cleanup_per_test = cleanup,
};

/* Helpers {{{ */

static char*
read_file(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = malloc((size_t)n + 1);
	*len = fread(buf, 1, (size_t)n, f);
	buf[*len] = '\0';
	fclose(f);
	return buf;
}

static bool
write_file(const char* path, const void* data, size_t len) {
	FILE* f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	fclose(f);
	return ok;
}

/** Run the dispatcher with `argv` (`NULL`-terminated, without the program name) and capture both streams. */
static int
run_argv(const char* const* args) {
	const char* argv[32] = { "cwsym" };
	int argc = 1;
	for (; args[argc - 1] != NULL && argc < 32; ++argc) {
		argv[argc] = args[argc - 1];
	}
	test_mkdir("work");
	test_mkdir("work/cli");
	FILE* out = fopen(OUT_PATH, "wb");
	FILE* err = fopen(ERR_PATH, "wb");
	cwsym_cli_t ctx = {
		.out = out, .err = err,
		.transport = &cw_transport_http,
		.token = token,
	};
	int status = cwsym_cli_main(&ctx, argc, argv);
	fclose(out);
	fclose(err);
	free(out_text);
	free(err_text);
	size_t len;
	out_text = read_file(OUT_PATH, &len);
	err_text = read_file(ERR_PATH, &len);
	BLOG_INFO("cwsym exited %d\n--- out ---\n%s--- err ---\n%s", status, out_text, err_text);
	return status;
}

#define RUN(...) run_argv((const char* const[]){ __VA_ARGS__, NULL })

static const char*
self_path(void) {
	static char path[1024];
	return test_self_path(path, sizeof(path)) ? path : "";
}

static bool
same_file(const char* a, const char* b) {
	size_t la, lb;
	char* ta = read_file(a, &la);
	char* tb = read_file(b, &lb);
	bool same = ta != NULL && tb != NULL && la == lb && memcmp(ta, tb, la) == 0;
	free(ta);
	free(tb);
	return same;
}

/* }}} */

BTEST(cli, usage) {
	BTEST_EXPECT_EQUAL("%d", RUN("--help"), CWSYM_CLI_OK);
	BTEST_EXPECT(strstr(out_text, "Commands:") != NULL && strstr(out_text, "upload") != NULL);
	BTEST_EXPECT(strstr(out_text, "CWSYM_TOKEN") != NULL);

	const char* none[] = { NULL };
	BTEST_EXPECT_EQUAL("%d", run_argv(none), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "expected a command") != NULL && strstr(err_text, "Usage:") != NULL);

	BTEST_EXPECT_EQUAL("%d", RUN("frobnicate", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "unknown command 'frobnicate'") != NULL);

	BTEST_EXPECT_EQUAL("%d", RUN("--bogus", "dump", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "Unknown option") != NULL);

	BTEST_EXPECT_EQUAL("%d", RUN("dump", "--help"), CWSYM_CLI_OK);
	BTEST_EXPECT(strstr(out_text, "Usage: cwsym dump") != NULL);

	BTEST_EXPECT_EQUAL("%d", RUN("dump", "--bogus", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "Unknown option") != NULL && strstr(err_text, "Usage: cwsym dump") != NULL);

	BTEST_EXPECT_EQUAL("%d", RUN("dump"), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EQUAL("%d", RUN("dump", FIXTURE, FIXTURE), CWSYM_CLI_USAGE);

	BTEST_EXPECT_EQUAL("%d", RUN("upload", "--endpoint", "http://x", "--version", "1", "--channel", "stable", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "--app is required") != NULL);

	/* Global options are accepted before the command and do not disturb a table input. */
	BTEST_EXPECT_EQUAL("%d", RUN("--symbol-map", "unused.map", "--source-map", "unused.wasm.map", "dump", FIXTURE), CWSYM_CLI_OK);
}

BTEST(cli, dump_matches_golden) {
	BTEST_ASSERT_EQUAL("%d", RUN("dump", FIXTURE), CWSYM_CLI_OK);
	BTEST_EXPECT_EX(same_file(OUT_PATH, TEST_FIXTURE_DIR "/synthetic.dump"), "dump differs:\n%s", out_text);
	BTEST_EXPECT(err_text[0] == '\0');
}

BTEST(cli, convert) {
	BTEST_EXPECT_EQUAL("%d", RUN("convert", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "--out is required") != NULL);

	BTEST_ASSERT_EQUAL("%d", RUN("convert", "-o", "work/cli/copy.cwsym", FIXTURE), CWSYM_CLI_OK);
	BTEST_EXPECT(same_file("work/cli/copy.cwsym", FIXTURE));

	BTEST_EXPECT_EQUAL("%d", RUN("convert", "-o", "work/cli/none.cwsym", "work/cli/does-not-exist"), CWSYM_CLI_FAILED);
	BTEST_EXPECT(strstr(err_text, "does-not-exist") != NULL);

	BTEST_ASSERT(write_file("work/cli/text.txt", "hello\n", 6));
	BTEST_EXPECT_EQUAL("%d", RUN("convert", "-o", "work/cli/none.cwsym", "work/cli/text.txt"), CWSYM_CLI_FAILED);
	BTEST_EXPECT_EX(strstr(err_text, "not a PE, ELF, or Wasm file") != NULL, "err: %s", err_text);

	BTEST_ASSERT(write_file("work/cli/bad.cwsym", "CWSYM\0\0\0", 8));
	BTEST_EXPECT_EQUAL("%d", RUN("convert", "-o", "work/cli/none.cwsym", "work/cli/bad.cwsym"), CWSYM_CLI_FAILED);
}

/** Convert this executable through the CLI and through the library directly; the bytes must agree. */
BTEST(cli, convert_debug_file) {
	cwsym_table_builder_t* b = cwsym_table_begin();
	cwsym_sink_t sink = cwsym_table_sink(b, true);
	cwsym_status_t read = cwsym_read(self_path(), NULL, &sink, NULL);
	cwsym_table_t direct;
	cwsym_status_t built = cwsym_table_end(b, &direct, NULL);
	if (read == CWSYM_ERR_UNSUPPORTED || read == CWSYM_ERR_NO_DEBUG) {
		BLOG_INFO("no reader for this executable on this host; skipped");
		return;
	}
	BTEST_ASSERT_EQUAL("%d", read, CWSYM_OK);
	BTEST_ASSERT_EQUAL("%d", built, CWSYM_OK);

	BTEST_ASSERT_EQUAL("%d", RUN("convert", "-o", "work/cli/self.cwsym", self_path()), CWSYM_CLI_OK);
	size_t len;
	char* bytes = read_file("work/cli/self.cwsym", &len);
	BTEST_ASSERT(bytes != NULL);
	BTEST_EXPECT_EQUAL("%zu", len, direct.len);
	BTEST_EXPECT(len == direct.len && memcmp(bytes, direct.data, len) == 0);
	free(bytes);
	cwsym_table_free(&direct);

	/* A table input round-trips through the same command. */
	BTEST_ASSERT_EQUAL("%d", RUN("convert", "-o", "work/cli/self2.cwsym", "work/cli/self.cwsym"), CWSYM_CLI_OK);
	BTEST_EXPECT(same_file("work/cli/self.cwsym", "work/cli/self2.cwsym"));
}

BTEST(cli, lookup) {
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE, "0x1000", "zz"), CWSYM_CLI_USAGE);
	BTEST_EXPECT(strstr(err_text, "'zz' is not an offset") != NULL);
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE, "0x100000000"), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE, "-1"), CWSYM_CLI_USAGE);

	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE, "0x1010", "4160", "0x2050"), CWSYM_CLI_OK);
	BTEST_EXPECT_EX(strcmp(out_text,
		"0x00001010 main 0x00001000 0x00000040\n"
		"0x00001040 a:helper 0x00001040 0x00000020\n"
		"0x00002050 ns::Foo::bar 0x00002000 0x00000100\n") == 0, "out: %s", out_text);

	/* A miss prints, the rest still resolve, and the status says so. */
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", FIXTURE, "0x5000", "0x1000"), CWSYM_CLI_FAILED);
	BTEST_EXPECT_EX(strcmp(out_text,
		"0x00005000 <unknown>\n"
		"0x00001000 main 0x00001000 0x00000040\n") == 0, "out: %s", out_text);
}

BTEST(cli, symbolize) {
	BTEST_EXPECT_EQUAL("%d", RUN("symbolize", FIXTURE), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EQUAL("%d", RUN("symbolize", FIXTURE, "0x"), CWSYM_CLI_USAGE);

	BTEST_EXPECT_EQUAL("%d", RUN("symbolize", FIXTURE, "0x2028", "0x1000", "0x1040"), CWSYM_CLI_OK);
	BTEST_EXPECT_EX(strcmp(out_text,
		"0x00002028\n"
		"  deep at inl.h:3\n"
		"  inl at inl.h:4\n"
		"  ns::Foo::bar(int) at foo.cpp:6\n"
		"0x00001000\n"
		"  main at a.c:10\n"
		"0x00001040\n"
		"  a:helper\n") == 0, "out: %s", out_text);

	BTEST_EXPECT_EQUAL("%d", RUN("symbolize", FIXTURE, "0x5000", "0x2000"), CWSYM_CLI_FAILED);
	BTEST_EXPECT_EX(strcmp(out_text,
		"0x00005000\n"
		"  <unknown>\n"
		"0x00002000\n"
		"  ns::Foo::bar(int) at foo.cpp:5\n") == 0, "out: %s", out_text);
}

BTEST(cli, upload) {
	static const char reply[] = "ok\n";
	server = test_http_start(&(test_http_reply_t){ .status = 200, .body = reply, .body_len = sizeof(reply) - 1 });
	BTEST_ASSERT(server != NULL);

	BTEST_ASSERT_EQUAL("%d", RUN(
		"upload", "--endpoint", test_http_url(server), "--app", "forest-quest",
		"--version", "1.4.2", "--channel", "stable", "-o", "work/cli/sent.cwsym", FIXTURE
	), CWSYM_CLI_OK);
	BTEST_ASSERT_EQUAL("%d", test_http_count(server), 1);
	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_EXPECT(strcmp(req->method, "PUT") == 0);
	BTEST_EXPECT_EX(strcmp(req->uri, "/v1/forest-quest/releases/1.4.2") == 0, "uri is %s", req->uri);
	BTEST_EXPECT_EX(strcmp(req->query, "channel=stable") == 0, "query is %s", req->query);
	BTEST_EXPECT(strcmp(req->authorization, "Bearer ci-token") == 0);
	size_t len;
	char* fixture = read_file(FIXTURE, &len);
	BTEST_ASSERT(fixture != NULL);
	BTEST_EXPECT_EQUAL("%zu", req->body_len, len);
	BTEST_EXPECT(req->body_len == len && memcmp(req->body, fixture, len) == 0);
	free(fixture);
	BTEST_EXPECT(same_file("work/cli/sent.cwsym", FIXTURE));
	BTEST_EXPECT_EX(strstr(err_text, "uploaded") != NULL && strstr(err_text, "200") != NULL, "err: %s", err_text);
	BTEST_EXPECT(strstr(err_text, "ci-token") == NULL);
}

BTEST(cli, upload_with_source) {
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	BTEST_ASSERT_EQUAL("%d", RUN(
		"upload", "--endpoint", test_http_url(server), "--app", "forest-quest",
		"--version", "1.4.2", "--channel", "stable", "--commit", "8f3c2a1", "--source-root", "/home/ci/game", FIXTURE
	), CWSYM_CLI_OK);
	BTEST_ASSERT_EQUAL("%d", test_http_count(server), 1);
	const test_http_request_t* req = test_http_request(server, 0);
	BTEST_EXPECT_EX(strcmp(req->query, "channel=stable&commit=8f3c2a1&source_root=%2Fhome%2Fci%2Fgame") == 0, "query is %s", req->query);
}

BTEST(cli, upload_requires_token) {
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	const char* args[] = {
		"upload", "--endpoint", test_http_url(server), "--app", "forest-quest",
		"--version", "1.4.2", "--channel", "stable", FIXTURE, NULL,
	};
	token = NULL;
	BTEST_EXPECT_EQUAL("%d", run_argv(args), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EX(strstr(err_text, "CWSYM_TOKEN") != NULL, "err: %s", err_text);
	token = "";
	BTEST_EXPECT_EQUAL("%d", run_argv(args), CWSYM_CLI_USAGE);
	BTEST_EXPECT_EQUAL("%d", test_http_count(server), 0);
}

BTEST(cli, upload_reports_refusal) {
	static const char reply[] = "conflict build_id\n";
	server = test_http_start(&(test_http_reply_t){ .status = 409, .body = reply, .body_len = sizeof(reply) - 1 });
	BTEST_ASSERT(server != NULL);
	BTEST_EXPECT_EQUAL("%d", RUN(
		"upload", "--endpoint", test_http_url(server), "--app", "forest-quest",
		"--version", "1.4.2", "--channel", "stable", FIXTURE
	), CWSYM_CLI_FAILED);
	BTEST_EXPECT_EX(strstr(err_text, "409") != NULL && strstr(err_text, "conflict build_id") != NULL, "err: %s", err_text);
}

BTEST(cli, upload_refuses_prefix) {
	server = test_http_start(NULL);
	BTEST_ASSERT(server != NULL);
	size_t len;
	char* fixture = read_file(FIXTURE, &len);
	BTEST_ASSERT(fixture != NULL);
	cwsym_table_t t;
	BTEST_ASSERT_EQUAL("%d", cwsym_table_parse(fixture, len, &t, NULL), CWSYM_OK);
	size_t prefix_len = (size_t)((const char*)t.strings - (const char*)t.data) + t.strings_len;
	test_mkdir("work");
	test_mkdir("work/cli");
	BTEST_ASSERT(write_file("work/cli/prefix.cwsym", fixture, prefix_len));
	cwsym_table_free(&t);
	free(fixture);

	/* The prefix is a valid table for dump and lookup, not for upload. */
	BTEST_EXPECT_EQUAL("%d", RUN("lookup", "work/cli/prefix.cwsym", "0x1000"), CWSYM_CLI_OK);
	BTEST_EXPECT_EQUAL("%d", RUN(
		"upload", "--endpoint", test_http_url(server), "--app", "forest-quest",
		"--version", "1.4.2", "--channel", "stable", "work/cli/prefix.cwsym"
	), CWSYM_CLI_FAILED);
	BTEST_EXPECT_EQUAL("%d", test_http_count(server), 0);
}
