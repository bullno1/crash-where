/**
 * @file hang.c
 * Hang detection over heartbeat readings, independent of any clock.
 */
#include "internal.h"

cw_hang_event_t
cw_hang_step(cw_hang_t* hang, uint64_t count, uint64_t now_ms, uint64_t timeout_ms) {
	if (count != hang->count) {
		hang->count = count;
		hang->since_ms = now_ms;
		hang->armed = true;
		if (hang->reported) {
			hang->reported = false;
			return CW_HANG_RECOVERED;
		}
		return CW_HANG_NONE;
	}
	uint64_t silent_ms = now_ms > hang->since_ms ? now_ms - hang->since_ms : 0;
	if (!hang->armed || hang->reported || hang->reports >= CW_HANG_MAX_REPORTS || silent_ms < timeout_ms) {
		return CW_HANG_NONE;
	}
	hang->reported = true;
	++hang->reports;
	return CW_HANG_REPORT;
}

void
cw_hang_reset(cw_hang_t* hang, uint64_t now_ms) {
	hang->since_ms = now_ms;
}

int
cw_hang_poll_ms(uint64_t timeout_ms) {
	uint64_t interval = timeout_ms / 4;
	return interval < 10 ? 10 : interval > 1000 ? 1000 : (int)interval;
}
