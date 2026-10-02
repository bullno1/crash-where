/**
 * @file web/platform.c
 * Platform services shared by the game and the watcher.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "web/platform.h"

bool
cw_platform_default_report_dir(char* out, size_t cap) {
	int len = snprintf(out, cap, "/cw/%s/crash", cw_ctx.cfg.app);
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
		/* getentropy() takes at most 256 bytes per call. */
		size_t n = len < 256 ? len : 256;
		if (getentropy(p, n) != 0) {
			return false;
		}
		p += n;
		len -= n;
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
	return 1;
}

bool
cw_platform_replace(const char* from, const char* to) {
	if (rename(from, to) != 0) {
		return false;
	}
	cw_web_store_put(to);
	return true;
}

void
cw_platform_remove(const char* path) {
	remove(path);
	cw_web_store_delete(path);
}

bool
cw_platform_list_dir(const char* path, void (*fn)(void* user, const char* name), void* user) {
	DIR* dir = opendir(path);
	if (dir == NULL) {
		return false;
	}
	for (struct dirent* e = readdir(dir); e != NULL; e = readdir(dir)) {
		if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
			fn(user, e->d_name);
		}
	}
	closedir(dir);
	return true;
}

bool
cw_platform_lock(const char* path) {
	return cw_web_store_lock(path);
}
