/**
 * @file linux/platform.c
 * Platform services shared by the game and the watcher.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "linux/platform.h"

cw_linux_t cw_linux = { .sock = -1 };

/**
 * `$XDG_STATE_HOME/<app>/crash`, with the `~/.local/state` fallback.
 */
bool
cw_platform_default_report_dir(char* out, size_t cap) {
	const char* app = cw_ctx.cfg.app;
	const char* xdg = getenv("XDG_STATE_HOME");
	int len;
	if (xdg != NULL && xdg[0] == '/') {
		len = snprintf(out, cap, "%s/%s/crash", xdg, app);
	} else {
		const char* home = getenv("HOME");
		if (home == NULL || home[0] != '/') {
			return false;
		}
		len = snprintf(out, cap, "%s/.local/state/%s/crash", home, app);
	}
	return len > 0 && (size_t)len < cap;
}

bool
cw_platform_mkdir_p(const char* path) {
	char buf[CW_STR_CAP];
	size_t len = strnlen(path, sizeof(buf));
	if (len == 0 || len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, path, len + 1);
	for (size_t i = 1; i <= len; ++i) {
		if (buf[i] == '/' || buf[i] == '\0') {
			char saved = buf[i];
			buf[i] = '\0';
			if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
				return false;
			}
			buf[i] = saved;
		}
	}
	return true;
}

bool
cw_platform_random(void* buf, size_t len) {
	unsigned char* p = buf;
	while (len > 0) {
		ssize_t n = getrandom(p, len, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

uint64_t
cw_platform_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint32_t
cw_platform_tid(void) {
	return (uint32_t)gettid();
}
