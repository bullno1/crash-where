/**
 * @file linux/watcher.c
 * Watcher side: adopt the region and socket, watch the game, write reports.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linux/platform.h"

static bool
mkdir_p(const char* path) {
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

/**
 * Resolve the report directory: the configured one, or
 * `$XDG_STATE_HOME/<app>/crash` with the `~/.local/state` fallback.
 */
static bool
resolve_report_dir(char* out, size_t cap) {
	if (cw_ctx.cfg.report_dir != NULL) {
		int len = snprintf(out, cap, "%s", cw_ctx.cfg.report_dir);
		return len > 0 && (size_t)len < cap;
	}
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

/**
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_crash_report(pid_t game, const char* report_dir, char* path, size_t cap) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){ .main_module = -1 };
	if (!cw_unwind(game, &cw_linux.region->crash, &info)) {
		cw_log(CW_LOG_WARN, "unwind produced no frames");
	}
	if (!cw_write_envelope(report_dir, &info, &cw_linux.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Report a game that vanished without a crash or a shutdown.
 */
static void
write_killed_report(const char* report_dir) {
	static cw_crash_info_t info;
	info = (cw_crash_info_t){
		.type = "KILLED",
		.message_raw = "game ended without cw_shutdown",
		.main_module = -1,
	};
	char path[CW_STR_CAP + 64];
	if (cw_write_envelope(report_dir, &info, &cw_linux.region->common, path, sizeof(path))) {
		cw_log(CW_LOG_INFO, "report written to %s", path);
		cw_upload_report(path);
	}
}

/**
 * Wait for messages until the game is gone.
 */
static void
cw_watch(pid_t game, int sock, const char* report_dir) {
	int pidfd = pidfd_open(game, 0);
	bool crashed = false;
	bool shutdown = false;
	int result = 0;
	bool game_gone = false;

	while (!game_gone) {
		struct pollfd fds[2] = {
			{ .fd = sock, .events = POLLIN },
			{ .fd = pidfd, .events = POLLIN },
		};
		int n = poll(fds, pidfd >= 0 ? 2 : 1, -1);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents & POLLIN) {
			cw_msg_t msg;
			if (read(sock, &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
				switch (msg.type) {
				case CW_MSG_CRASH: {
					/* Reply first: the game is parked until it hears back. */
					char path[CW_STR_CAP + 64];
					bool written = write_crash_report(game, report_dir, path, sizeof(path));
					crashed = true;
					cw_send_msg(sock, CW_MSG_DONE, 0);
					if (written) {
						cw_upload_report(path);
					}
					break;
				}
				case CW_MSG_SHUTDOWN:
					shutdown = true;
					result = msg.value;
					break;
				case CW_MSG_AUTH:
					cw_log(CW_LOG_DEBUG, "token refreshed");
					break;
				default:
					break;
				}
				continue;
			}
		}
		if (fds[0].revents & (POLLHUP | POLLERR)) {
			/* The game closed its end: it exited, or gave up on the handshake. */
			game_gone = pidfd < 0 || poll(&fds[1], 1, 100) > 0;
			if (!game_gone) {
				cw_log(CW_LOG_WARN, "game dropped the socket, watcher exiting");
				return;
			}
		}
		if (pidfd >= 0 && (fds[1].revents & POLLIN)) {
			game_gone = true;
		}
	}

	if (pidfd >= 0) {
		close(pidfd);
	}

	if (crashed) {
		return;
	}

	if (shutdown) {
		cw_log(CW_LOG_INFO, "game exited cleanly with result %d", result);
		return;
	}

	cw_log(CW_LOG_WARN, "game ended without cw_shutdown");
	write_killed_report(report_dir);
}

/**
 * Watcher path: adopt the inherited region and socket, report ready,
 * watch, exit.
 */
_Noreturn void
cw_platform_run_watcher(const char* spec) {
	unsetenv(CW_ENV_WATCHER);
	signal(SIGPIPE, SIG_IGN);
	int game;
	int memfd;
	int sock;

	if (sscanf(spec, "%d,%d,%d", &game, &memfd, &sock) != 3) {
		cw_log(CW_LOG_ERROR, "malformed " CW_ENV_WATCHER);
		exit(1);
	}

	struct stat st;
	if (fstat(memfd, &st) != 0 || (size_t)st.st_size < sizeof(cw_region_t)) {
		cw_log(CW_LOG_ERROR, "shared region too small");
		exit(1);
	}

	cw_region_t* region = mmap(NULL, sizeof(cw_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	close(memfd);
	if (region == MAP_FAILED) {
		cw_log(CW_LOG_ERROR, "cannot map shared region");
		exit(1);
	}
	if (region->common.magic != CW_REGION_MAGIC) {
		cw_log(CW_LOG_ERROR, "shared region header mismatch");
		exit(1);
	}
	cw_linux.region = region;
	cw_ctx.shared = &region->common;

	char report_dir[CW_STR_CAP];
	if (!resolve_report_dir(report_dir, sizeof(report_dir))) {
		cw_log(CW_LOG_ERROR, "cannot resolve report directory");
		exit(1);
	}

	char sub_dir[CW_STR_CAP + 16];
	snprintf(sub_dir, sizeof(sub_dir), "%s/pending", report_dir);
	if (!mkdir_p(sub_dir)) {
		cw_log(CW_LOG_ERROR, "cannot create %s", sub_dir);
		exit(1);
	}

	cw_send_msg(sock, CW_MSG_READY, 0);
	cw_log(CW_LOG_INFO, "watching game pid %d", game);
	cw_watch(game, sock, report_dir);
	exit(0);
}
