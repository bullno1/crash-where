/**
 * @file linux/platform.c
 * Platform services shared by the game and the watcher.
 */
#include <errno.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "linux/platform.h"

cw_linux_t cw_linux = { .sock = -1 };

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
