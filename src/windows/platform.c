/**
 * @file windows/platform.c
 * Platform services shared by the game and the watcher.
 */
#include "windows/platform.h"

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cw_win_t cw_win;

/**
 * `%LOCALAPPDATA%\<app>\crash`.
 */
bool
cw_platform_default_report_dir(char* out, size_t cap) {
	const char* local = getenv("LOCALAPPDATA");
	if (local == NULL || local[0] == '\0') {
		return false;
	}
	int len = snprintf(out, cap, "%s\\%s\\crash", local, cw_ctx.cfg.app);
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

	/* A drive letter is not a directory to create. */
	size_t i = len >= 2 && buf[1] == ':' ? 3 : 1;
	for (; i <= len; ++i) {
		if (buf[i] == '\\' || buf[i] == '/' || buf[i] == '\0') {
			char saved = buf[i];
			buf[i] = '\0';
			if (!CreateDirectoryA(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
				return false;
			}
			buf[i] = saved;
		}
	}
	return true;
}

bool
cw_platform_random(void* buf, size_t len) {
	return BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

uint64_t
cw_platform_now_ms(void) {
	return GetTickCount64();
}

uint32_t
cw_platform_tid(void) {
	return GetCurrentThreadId();
}
