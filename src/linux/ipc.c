/**
 * @file linux/ipc.c
 * Fixed-size messages over the game/watcher socket.
 *
 * Both functions run inside the crash signal handler as well as in
 * ordinary code, so they touch only the stack and issue `write`, `poll`,
 * and `read`. Keep them free of logging and allocation.
 */
#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "linux/platform.h"

bool
cw_send_msg(int sock, uint8_t type, int32_t value) {
	cw_msg_t msg = { .type = type, .value = value };
	return write(sock, &msg, sizeof(msg)) == (ssize_t)sizeof(msg);
}

bool
cw_recv_msg(int sock, cw_msg_t* msg, int timeout_ms) {
	struct pollfd pfd = { .fd = sock, .events = POLLIN };
	int n;
	do {
		n = poll(&pfd, 1, timeout_ms);
	} while (n < 0 && errno == EINTR);
	if (n <= 0 || !(pfd.revents & POLLIN)) {
		return false;
	}
	return read(sock, msg, sizeof(*msg)) == (ssize_t)sizeof(*msg);
}
