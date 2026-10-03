/**
 * @file web.c
 * Web implementation of the test platform functions.
 *
 * The runner is a page. A child is this same binary in a frame of its
 * own, with a watcher of its own, and the run directory comes back from
 * that watcher when the child ends.
 */
#include <dirent.h>
#include <emscripten.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

#define TEST_WEB_SIGNALED 0x10000

/**
 * Run a child to its end and bring its run directory back.
 *
 * Suspends the caller. Implemented by the test library.
 *
 * @param env_json  The variables to add, as one JSON object.
 * @return The exit status, with ::TEST_WEB_SIGNALED set when a trap ended the child.
 */
int
test_web_spawn(const char* env_json);

int
main(int argc, const char* argv[]);

/**
 * The entry point as a table index, which the page needs to start a
 * run that can suspend.
 */
EMSCRIPTEN_KEEPALIVE uintptr_t
test_web_main(void) {
	return (uintptr_t)main;
}

bool
test_spawn_self(const char* const* env, test_exit_t* out) {
	/* Names and values come from the runner and hold nothing to escape. */
	char json[4096];
	size_t len = (size_t)snprintf(json, sizeof(json), "{");
	for (int i = 0; env[i] != NULL; ++i) {
		const char* eq = strchr(env[i], '=');
		if (eq == NULL) {
			continue;
		}
		int n = snprintf(
			json + len, sizeof(json) - len, "%s\"%.*s\":\"%s\"",
			len > 1 ? "," : "", (int)(eq - env[i]), env[i], eq + 1
		);
		if (n < 0 || (size_t)n >= sizeof(json) - len - 1) {
			return false;
		}
		len += (size_t)n;
	}
	snprintf(json + len, sizeof(json) - len, "}");

	int status = test_web_spawn(json);
	if (status < 0) {
		return false;
	}
	*out = (test_exit_t){
		.signaled = (status & TEST_WEB_SIGNALED) != 0,
		.code = status & (TEST_WEB_SIGNALED - 1),
	};
	return true;
}

#ifdef __EMSCRIPTEN_PTHREADS__

typedef struct {
	void (*fn)(void);
} thread_arg_t;

static void*
thread_main(void* arg) {
	((thread_arg_t*)arg)->fn();
	return NULL;
}

bool
test_run_thread(void (*fn)(void)) {
	thread_arg_t arg = { .fn = fn };
	pthread_t thread;
	if (pthread_create(&thread, NULL, thread_main, &arg) != 0) {
		return false;
	}
	pthread_join(thread, NULL);
	return true;
}

#else

bool
test_run_thread(void (*fn)(void)) {
	(void)fn;
	return false;
}

#endif

void
test_platform_init(void) {
}

bool
test_mkdir(const char* path) {
	return mkdir(path, 0777) == 0 || errno == EEXIST;
}

bool
test_remove_tree(const char* path) {
	DIR* dir = opendir(path);
	if (dir == NULL) {
		return errno == ENOENT;
	}
	bool ok = true;
	for (struct dirent* e = readdir(dir); e != NULL; e = readdir(dir)) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		char child[1024];
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		struct stat st;
		if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
			ok = test_remove_tree(child) && ok;
		} else {
			ok = unlink(child) == 0 && ok;
		}
	}
	closedir(dir);
	return rmdir(path) == 0 && ok;
}

int
test_count_files(const char* path, const char* suffix) {
	DIR* dir = opendir(path);
	if (dir == NULL) {
		return -1;
	}
	int count = 0;
	size_t suffix_len = strlen(suffix);
	for (struct dirent* e = readdir(dir); e != NULL; e = readdir(dir)) {
		size_t len = strlen(e->d_name);
		if (len >= suffix_len && strcmp(e->d_name + len - suffix_len, suffix) == 0) {
			++count;
		}
	}
	closedir(dir);
	return count;
}

void
test_sleep_ms(unsigned ms) {
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

long
test_file_size(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int
test_stop_helper_main(const char* spec) {
	(void)spec;
	return 2;
}

bool
test_stop_self(unsigned ms) {
	(void)ms;
	return false;
}

bool
test_debug_self(void) {
	return false;
}

uintptr_t
test_image_base(void) {
	return 0;
}

bool
test_self_path(char* buf, size_t cap) {
	(void)buf;
	(void)cap;
	return false;
}

bool
test_under_wine(void) {
	return false;
}

bool
test_displace_crash_handler(void) {
	return false;
}

EM_JS(void, test_web_host_throw, (void), {
	throw new TypeError('host');
});

bool
test_host_can_throw(void) {
	return true;
}

void
test_host_throw(void) {
	test_web_host_throw();
}
