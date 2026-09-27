/**
 * @file windows/platform.c
 * Platform services shared by the game and the watcher.
 */
#include "windows/platform.h"

#include <bcrypt.h>

cw_win_t cw_win;

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
