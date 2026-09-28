/**
 * @file scenario.c
 * Runner side of the harness, plus the child's entry point and the
 * test uploader that runs inside the watcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <blog.h>
#include <cw.h>
#include "scenario.h"

AUTOLIST_IMPL(test_scenarios)

/* Child side {{{ */

/**
 * Forward a library message to blog at the matching level.
 */
static void
log_from_cw(cw_log_level_t level, const char* msg) {
	static const blog_level_t levels[] = {
		[CW_LOG_ERROR] = BLOG_LEVEL_ERROR,
		[CW_LOG_WARN]  = BLOG_LEVEL_WARN,
		[CW_LOG_INFO]  = BLOG_LEVEL_INFO,
		[CW_LOG_DEBUG] = BLOG_LEVEL_DEBUG,
	};
	BLOG_WRITE(levels[level], "%s", msg);
}

static cw_status_t
status_from_env(void) {
	const char* status = getenv("CW_TEST_STATUS");
	if (status != NULL && strcmp(status, "retry") == 0) {
		return CW_RETRY;
	}
	if (status != NULL && strcmp(status, "drop") == 0) {
		return CW_DROP;
	}
	return CW_OK;
}

/**
 * Append one event to the uploader log and free the document.
 */
static void
write_event(yyjson_mut_doc* doc) {
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

static cw_status_t
send_envelope(void* user, const cw_report_t* report, bool* want_attachments) {
	(void)user;
	yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_str(doc, root, "call", "envelope");
	yyjson_mut_obj_add_str(doc, root, "id", report->id);
	yyjson_mut_obj_add_int(doc, root, "attempts", report->attempts);
	if (report->token != NULL) {
		yyjson_mut_obj_add_str(doc, root, "token", report->token);
	} else {
		yyjson_mut_obj_add_null(doc, root, "token");
	}
	yyjson_mut_val* attachments = yyjson_mut_arr(doc);
	for (const char* const* p = report->attachments; *p != NULL; ++p) {
		yyjson_mut_arr_add_str(doc, attachments, *p);
	}
	yyjson_mut_obj_add_val(doc, root, "attachments", attachments);
	/* The envelope is one JSON object followed by a newline; splice it in verbatim. */
	size_t len = strlen(report->envelope_json);
	while (len > 0 && report->envelope_json[len - 1] == '\n') {
		--len;
	}
	yyjson_mut_obj_add_val(doc, root, "envelope", yyjson_mut_rawn(doc, report->envelope_json, len));
	write_event(doc);

	const char* want = getenv("CW_TEST_WANT_ATTACHMENTS");
	*want_attachments = want != NULL && strcmp(want, "1") == 0;
	return status_from_env();
}

/**
 * Copy `src` to `dst`. Returns the number of bytes copied, or -1.
 */
static long
copy_file(const char* src, const char* dst) {
	FILE* in = fopen(src, "rb");
	if (in == NULL) {
		return -1;
	}
	FILE* out = fopen(dst, "wb");
	if (out == NULL) {
		fclose(in);
		return -1;
	}
	long total = 0;
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		fwrite(buf, 1, n, out);
		total += (long)n;
	}
	fclose(in);
	fclose(out);
	return total;
}

static cw_status_t
send_attachment(void* user, const cw_report_t* report, const char* path) {
	(void)user;
	const char* ext = strrchr(path, '.');
	char copy[512];
	snprintf(copy, sizeof(copy), "%s/%s%s", getenv("CW_TEST_OUT"), report->id, ext != NULL ? ext : "");
	long size = copy_file(path, copy);

	yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_obj_add_str(doc, root, "call", "attachment");
	yyjson_mut_obj_add_str(doc, root, "id", report->id);
	yyjson_mut_obj_add_str(doc, root, "path", path);
	yyjson_mut_obj_add_str(doc, root, "copy", copy);
	yyjson_mut_obj_add_int(doc, root, "size", size);
	write_event(doc);
	return status_from_env();
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

	/* Copied by cw_init, so it may live on this stack. */
	cw_uploader_t uploader = {
		.send_envelope = send_envelope,
		.send_attachment = send_attachment,
	};
	cw_config_t cfg = {
		.app = "cw-test",
		.version = "0.0.1",
		.channel = "test",
		.endpoint = "http://127.0.0.1:9",
		.report_dir = report_dir,
		.uploader = &uploader,
		.log = log_from_cw,
	};
	cw_init(&cfg);

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
 * Parse the uploader log, one JSON document per line.
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
	char events[512];
	snprintf(events, sizeof(events), "%s/events.jsonl", run->dir);
	FILE* f = fopen(events, "w");
	if (f != NULL) {
		fclose(f);
	}

	char e_scenario[128];
	char e_out[512];
	char e_report[512];
	char e_status[64];
	char e_want[64];
	snprintf(e_scenario, sizeof(e_scenario), "CW_TEST_SCENARIO=%s", scenario->name);
	snprintf(e_out, sizeof(e_out), "CW_TEST_OUT=%s", run->dir);
	snprintf(e_report, sizeof(e_report), "CW_TEST_REPORT_DIR=%s/report", run->dir);
	snprintf(e_status, sizeof(e_status), "CW_TEST_STATUS=%s", o.status != NULL ? o.status : "ok");
	snprintf(e_want, sizeof(e_want), "CW_TEST_WANT_ATTACHMENTS=%d", o.want_attachments ? 1 : 0);
	const char* env[] = {
		e_scenario, e_out, e_report, e_status, e_want,
		/* A sanitizer build must let the crash reach the library's handlers. */
		"ASAN_OPTIONS=handle_segv=0:handle_abort=0:handle_sigbus=0:handle_sigfpe=0:handle_sigill=0",
		o.disable ? "CW_DISABLE=1" : NULL,
		NULL,
	};

	if (!test_spawn_self(env, &run->exit)) {
		BLOG_ERROR("cannot spawn scenario %s", scenario->name);
		return NULL;
	}
	read_events(run);
	return run;
}

/* }}} */
