/**
 * @file scenario.c
 * Runner side of the harness, plus the child's entry point and the
 * test transport that runs inside the watcher.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <blog.h>
#include <cw.h>
#include <cw_host.h>
#include "scenario.h"

AUTOLIST_IMPL(test_scenarios)

/* Child side {{{ */

void
test_cw_log(cw_log_level_t level, const char* msg) {
	static const blog_level_t levels[] = {
		[CW_LOG_ERROR] = BLOG_LEVEL_ERROR,
		[CW_LOG_WARN]  = BLOG_LEVEL_WARN,
		[CW_LOG_INFO]  = BLOG_LEVEL_INFO,
		[CW_LOG_DEBUG] = BLOG_LEVEL_DEBUG,
	};
	BLOG_WRITE(levels[level], "%s", msg);
}

/**
 * HTTP status the runner asked the transport to answer with.
 */
static int
http_status_from_env(void) {
	const char* status = getenv("CW_TEST_STATUS");
	if (status != NULL && strcmp(status, "retry") == 0) {
		return 503;
	}
	if (status != NULL && strcmp(status, "drop") == 0) {
		return 400;
	}
	return 200;
}

void
test_write_event(yyjson_mut_doc* doc) {
	char path[512];
	snprintf(path, sizeof(path), "%s/events.jsonl", getenv("CW_TEST_OUT"));
	FILE* f = fopen(path, "a");
	if (f != NULL) {
		char* text = yyjson_mut_write(doc, 0, NULL);
		if (text != NULL) {
			fputs(text, f);
			fputc('\n', f);
			free(text);
		}
		fclose(f);
	}
	yyjson_mut_doc_free(doc);
}

/**
 * Whether the request went to a route of the given name.
 */
static bool
route_is(const cw_request_t* req, const char* route) {
	size_t url_len = strlen(req->url);
	size_t route_len = strlen(route);
	return url_len > route_len && req->url[url_len - route_len - 1] == '/'
		&& strcmp(req->url + url_len - route_len, route) == 0;
}

/**
 * Test transport: log the request, then answer with the status the
 * runner asked for and a reply that grants or declines attachments, or
 * mints a token for a proof.
 */
static cw_status_t
test_send(void* user, const cw_request_t* req, cw_response_t* resp) {
	(void)user;
	yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_str(doc, root, "call", "request");
	yyjson_mut_obj_add_str(doc, root, "method", req->method);
	yyjson_mut_obj_add_str(doc, root, "url", req->url);
	if (req->content_type != NULL) {
		yyjson_mut_obj_add_str(doc, root, "content_type", req->content_type);
	} else {
		yyjson_mut_obj_add_null(doc, root, "content_type");
	}
	if (req->token != NULL) {
		yyjson_mut_obj_add_str(doc, root, "token", req->token);
	} else {
		yyjson_mut_obj_add_null(doc, root, "token");
	}
	yyjson_mut_obj_add_uint(doc, root, "body_len", req->body_len);
	/* A compressed body is recorded decoded, so an event reads the same whatever the wire carried. */
	const char* body = req->body;
	size_t len = req->body_len;
	uint8_t* decoded = NULL;
	if (req->content_encoding != NULL) {
		yyjson_mut_obj_add_str(doc, root, "content_encoding", req->content_encoding);
		decoded = strcmp(req->content_encoding, "gzip") == 0 ? test_gunzip(req->body, req->body_len, &len) : NULL;
		body = (const char*)decoded;
	} else {
		yyjson_mut_obj_add_null(doc, root, "content_encoding");
	}
	yyjson_mut_obj_add_bool(doc, root, "decodable", body != NULL);
	yyjson_mut_obj_add_uint(doc, root, "raw_len", body != NULL ? len : 0);
	if (body != NULL && req->content_type != NULL && strcmp(req->content_type, "application/json") == 0) {
		/* The envelope is one JSON object followed by a newline; splice it in verbatim. */
		while (len > 0 && body[len - 1] == '\n') {
			--len;
		}
		yyjson_mut_obj_add_val(doc, root, "envelope", yyjson_mut_rawn(doc, body, len));
	}
	test_write_event(doc);
	free(decoded);

	int n;
	if (route_is(req, "auth")) {
		n = snprintf(req->reply, req->reply_cap, "token watcher-token\nexpires %lld\n", (long long)time(NULL) + 3600);
	} else {
		const char* want = getenv("CW_TEST_WANT_ATTACHMENTS");
		n = snprintf(
			req->reply, req->reply_cap, "want_attachments %d\n",
			want != NULL && strcmp(want, "1") == 0 ? 1 : 0
		);
	}
	*resp = (cw_response_t){
		.status = http_status_from_env(),
		.reply_len = n > 0 && (size_t)n < req->reply_cap ? (size_t)n : 0,
	};
	return CW_OK;
}

static cw_consent_t
consent_from_name(const char* name) {
	if (strcmp(name, "always") == 0) {
		return CW_CONSENT_ALWAYS;
	}
	if (strcmp(name, "never") == 0) {
		return CW_CONSENT_NEVER;
	}
	if (strcmp(name, "once") == 0) {
		return CW_CONSENT_ONCE;
	}
	return CW_CONSENT_ASK;
}

/**
 * Native prompt, run in the watcher: log what it was shown and answer
 * what the runner asked for.
 */
static cw_consent_t
test_dialog_show(void* user, const cw_consent_summary_t* summary) {
	static const char* const kind_names[] = { "crash", "hang", "abnormal_exit" };
	yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_str(doc, root, "call", "dialog");
	yyjson_mut_obj_add_int(doc, root, "count", summary->count);
	yyjson_mut_obj_add_str(doc, root, "kind", kind_names[summary->newest_kind]);
	yyjson_mut_obj_add_sint(doc, root, "time", summary->newest_time);
	test_write_event(doc);
	return consent_from_name(user);
}

/**
 * Test collectors: each records which slot it ran from, so the runner
 * can see both in the envelope.
 */
static void
test_collect(void* user) {
	cw_set_env(user, "1");
}

int
test_fixture_main(const char* name) {
	/* A fresh process, and the watcher after it, so logging starts here. */
	blog_init(&(blog_options_t){
		.current_filename = __FILE__,
		.current_depth_in_project = 1, /* test/scenario.c */
	});
	blog_add_file_logger(BLOG_LEVEL_TRACE, &(blog_file_logger_options_t){
		.file = stderr,
		.with_colors = true,
	});

	const char* report_dir = getenv("CW_TEST_REPORT_DIR");
	if (report_dir == NULL || getenv("CW_TEST_OUT") == NULL) {
		BLOG_ERROR("CW_TEST_REPORT_DIR and CW_TEST_OUT must be set");
		return 2;
	}

	/* The watcher inherits the variable; only the game is debugged. */
	const char* debugger = getenv("CW_TEST_DEBUGGER");
	if (
		debugger != NULL && strcmp(debugger, "1") == 0
		&& getenv("CW_WATCHER") == NULL && !test_debug_self()
	) {
		BLOG_ERROR("cannot attach a debugger");
		return 2;
	}

	/* Copied by cw_init, so these may live on this stack. */
	cw_transport_t transport = { .send = test_send };
	const char* dialog_answer = getenv("CW_TEST_DIALOG");
	cw_consent_dialog_t dialog = { .show = test_dialog_show, .user = (void*)dialog_answer };
	cw_collector_t at_init = { .collect = test_collect, .user = "collected_at_init" };
	cw_collector_t at_report = { .collect = test_collect, .user = "collected_at_report" };
	const char* host = getenv("CW_TEST_HOST");
	bool use_host = host != NULL && strcmp(host, "1") == 0;
	const char* hang_ms = getenv("CW_TEST_HANG_MS");
	cw_config_t cfg = {
		.app = "cw-test",
		.version = "0.0.1",
		.channel = "test",
		.endpoint = "http://127.0.0.1:9",
		.report_dir = report_dir,
		.hang_timeout_ms = hang_ms != NULL ? (uint32_t)strtoul(hang_ms, NULL, 10) : 0,
		.transport = &transport,
		.consent_dialog = dialog_answer != NULL && dialog_answer[0] != '\0' ? &dialog : NULL,
		.collect_at_init = use_host ? &cw_collector_host : &at_init,
		.collect_at_report = use_host ? &cw_collector_host : &at_report,
		.log = test_cw_log,
	};
	cw_init(&cfg);

	const char* consent = getenv("CW_TEST_CONSENT");
	if (consent == NULL || consent[0] == '\0') {
		consent = "always";
	}
	if (strcmp(consent, "skip") != 0) {
		cw_consent_set(consent_from_name(consent));
	}

	const char* auth = getenv("CW_TEST_AUTH");
	if (auth != NULL && auth[0] != '\0') {
		bool ok;
		if (strcmp(auth, "proof") == 0) {
			ok = cw_auth_proof("test", "proof-bytes", sizeof("proof-bytes") - 1);
		} else {
			ok = cw_auth_token("local-token", strcmp(auth, "expired") == 0 ? 1 : (int64_t)time(NULL) + 3600);
		}
		cw_set_state("auth", ok ? "1" : "0");
	}

	AUTOLIST_FOREACH(entry, test_scenarios) {
		const test_scenario_t* scenario = entry->value_addr;
		if (strcmp(scenario->name, name) == 0) {
			scenario->run();
			return 0;
		}
	}
	BLOG_ERROR("unknown scenario %s", name);
	return 2;
}

/* }}} */

/* Runner side {{{ */

static test_run_t last_run;

void
test_run_cleanup(void) {
	for (int i = 0; i < last_run.num_events; ++i) {
		yyjson_doc_free(last_run.events[i]);
	}
	last_run = (test_run_t){ 0 };
}

/**
 * Parse the transport log, one JSON document per line.
 */
static void
read_events(test_run_t* run) {
	char path[512];
	snprintf(path, sizeof(path), "%s/events.jsonl", run->dir);
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* text = malloc((size_t)len + 1);
	if (text == NULL) {
		fclose(f);
		return;
	}
	size_t n = fread(text, 1, (size_t)len, f);
	text[n] = '\0';
	fclose(f);

	char* line = text;
	while (*line != '\0') {
		char* end = strchr(line, '\n');
		size_t line_len = end != NULL ? (size_t)(end - line) : strlen(line);
		if (line_len > 0) {
			if (run->num_events == TEST_MAX_EVENTS) {
				BLOG_WARN("more than %d events in %s, rest ignored", TEST_MAX_EVENTS, path);
				break;
			}
			yyjson_read_err err;
			yyjson_doc* doc = yyjson_read_opts(line, line_len, 0, NULL, &err);
			if (doc == NULL) {
				BLOG_ERROR("bad event line in %s: %s", path, err.msg);
			} else {
				run->events[run->num_events++] = doc;
			}
		}
		if (end == NULL) {
			break;
		}
		line = end + 1;
	}
	free(text);
}

const test_run_t*
test_run_scenario(const char* test, const test_scenario_t* scenario, const test_run_opts_t* opts) {
	test_run_cleanup();
	test_run_opts_t o = opts != NULL ? *opts : (test_run_opts_t){ 0 };
	test_run_t* run = &last_run;

	snprintf(run->dir, sizeof(run->dir), "work/%s", test);
	if (!test_mkdir("work") || !test_mkdir(run->dir)) {
		BLOG_ERROR("cannot create %s", run->dir);
		return NULL;
	}
	char report[512];
	snprintf(report, sizeof(report), "%s/report", run->dir);
	if (!o.keep && !test_remove_tree(report)) {
		BLOG_ERROR("cannot clear %s", report);
		return NULL;
	}
	char events[512];
	snprintf(events, sizeof(events), "%s/events.jsonl", run->dir);
	FILE* f = fopen(events, "w");
	if (f != NULL) {
		fclose(f);
	}

	char e_scenario[128];
	char e_out[512];
	char e_report[560];
	char e_status[64];
	char e_want[64];
	char e_hang[64];
	char e_auth[64];
	char e_consent[64];
	char e_dialog[64];
	char e_host[32];
	char e_debugger[32];
	snprintf(e_scenario, sizeof(e_scenario), "CW_TEST_SCENARIO=%s", scenario->name);
	snprintf(e_out, sizeof(e_out), "CW_TEST_OUT=%s", run->dir);
	snprintf(e_report, sizeof(e_report), "CW_TEST_REPORT_DIR=%s", report);
	snprintf(e_status, sizeof(e_status), "CW_TEST_STATUS=%s", o.status != NULL ? o.status : "ok");
	snprintf(e_want, sizeof(e_want), "CW_TEST_WANT_ATTACHMENTS=%d", o.want_attachments ? 1 : 0);
	snprintf(e_hang, sizeof(e_hang), "CW_TEST_HANG_MS=%" PRIu32, o.hang_timeout_ms);
	snprintf(e_auth, sizeof(e_auth), "CW_TEST_AUTH=%s", o.auth != NULL ? o.auth : "");
	snprintf(e_consent, sizeof(e_consent), "CW_TEST_CONSENT=%s", o.consent != NULL ? o.consent : "");
	snprintf(e_dialog, sizeof(e_dialog), "CW_TEST_DIALOG=%s", o.dialog != NULL ? o.dialog : "");
	snprintf(e_host, sizeof(e_host), "CW_TEST_HOST=%d", o.host ? 1 : 0);
	snprintf(e_debugger, sizeof(e_debugger), "CW_TEST_DEBUGGER=%d", o.debugger ? 1 : 0);
	const char* env[] = {
		e_scenario, e_out, e_report, e_status, e_want, e_hang, e_auth, e_consent, e_dialog, e_host,
		e_debugger,
		/* A sanitizer build must let the crash reach the library's handlers. */
		"ASAN_OPTIONS=handle_segv=0:handle_abort=0:handle_sigbus=0:handle_sigfpe=0:handle_sigill=0",
		o.disable ? "CW_DISABLE=1" : o.force ? "CW_DISABLE=0" : NULL,
		NULL,
	};

	if (!test_spawn_self(env, &run->exit)) {
		BLOG_ERROR("cannot spawn scenario %s", scenario->name);
		return NULL;
	}
	read_events(run);
	return run;
}

bool
test_run_has(const test_run_t* run, const char* name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/report/%s", run->dir, name);
	return test_file_size(path) >= 0;
}

int
test_run_pending(const test_run_t* run, const char* suffix) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/report/pending", run->dir);
	return test_count_files(dir, suffix);
}

bool
test_event_is(yyjson_doc* ev, const char* route, const char* token) {
	char url[128];
	snprintf(url, sizeof(url), "http://127.0.0.1:9/v1/cw-test/%s", route);
	const char* call = test_json_str(ev, "/call");
	const char* got_url = test_json_str(ev, "/url");
	const char* got = test_json_str(ev, "/token");
	return call != NULL && strcmp(call, "request") == 0
		&& got_url != NULL && strcmp(got_url, url) == 0
		&& (token == NULL ? got == NULL : got != NULL && strcmp(got, token) == 0);
}

uintptr_t
test_state_hex(yyjson_doc* ev, const char* key) {
	char ptr[64];
	snprintf(ptr, sizeof(ptr), "/envelope/state/%s", key);
	const char* s = test_json_str(ev, ptr);
	return s != NULL ? (uintptr_t)strtoull(s, NULL, 16) : 0;
}

uintptr_t
test_frame_addr(yyjson_doc* ev, size_t i) {
	uintptr_t base = test_state_hex(ev, "base");
	const char* main_name = NULL;
	yyjson_val* modules = test_json_get(ev, "/envelope/modules");
	size_t idx;
	size_t max;
	yyjson_val* m;
	yyjson_arr_foreach(modules, idx, max, m) {
		const char* s = yyjson_get_str(yyjson_obj_get(m, "base"));
		if (s != NULL && strtoull(s, NULL, 16) == base) {
			main_name = yyjson_get_str(yyjson_obj_get(m, "name"));
			break;
		}
	}
	char ptr[64];
	snprintf(ptr, sizeof(ptr), "/envelope/frames/%zu", i);
	yyjson_val* fr = test_json_get(ev, ptr);
	const char* name = yyjson_get_str(yyjson_obj_get(fr, "module"));
	if (base == 0 || main_name == NULL || name == NULL || strcmp(name, main_name) != 0) {
		return 0;
	}
	return base + (uintptr_t)yyjson_get_uint(yyjson_obj_get(fr, "offset"));
}

/* }}} */
