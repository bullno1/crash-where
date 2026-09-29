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

bool
cw_platform_replace(const char* from, const char* to) {
	return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool
cw_platform_list_dir(const char* path, void (*fn)(void* user, const char* name), void* user) {
	char pattern[CW_STR_CAP + 32];
	int len = snprintf(pattern, sizeof(pattern), "%s\\*", path);
	if (len <= 0 || (size_t)len >= sizeof(pattern)) {
		return false;
	}
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE) {
		return false;
	}
	do {
		if (strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0) {
			fn(user, data.cFileName);
		}
	} while (FindNextFileA(find, &data));
	FindClose(find);
	return true;
}

bool
cw_platform_lock(const char* path) {
	/* No sharing: a second opener fails until this process exits and the handle closes. */
	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	return h != INVALID_HANDLE_VALUE;
}
