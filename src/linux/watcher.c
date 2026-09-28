/**
 * @file linux/watcher.c
 * Watcher side: adopt the region and socket, watch the game, write reports.
 */
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
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
 * End of the mapping that holds `addr` in the game, or 0 when unknown.
 */
static uint64_t
mapping_end(pid_t game, uint64_t addr) {
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/maps", (int)game);
	FILE* f = fopen(proc, "r");
	if (f == NULL) {
		return 0;
	}
	uint64_t end = 0;
	char line[CW_STR_CAP + 128];
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned long start;
		unsigned long stop;
		if (sscanf(line, "%lx-%lx", &start, &stop) == 2 && addr >= start && addr < stop) {
			end = stop;
			break;
		}
	}
	fclose(f);
	return end;
}

static void
regs_to_context(const struct user_regs_struct* regs, ucontext_t* uc) {
	*uc = (ucontext_t){ 0 };
#if defined(__x86_64__)
	uc->uc_mcontext.gregs[REG_RIP] = (greg_t)regs->rip;
	uc->uc_mcontext.gregs[REG_RSP] = (greg_t)regs->rsp;
	uc->uc_mcontext.gregs[REG_RBP] = (greg_t)regs->rbp;
#elif defined(__aarch64__)
	uc->uc_mcontext.pc = regs->pc;
	uc->uc_mcontext.sp = regs->sp;
	uc->uc_mcontext.regs[29] = regs->regs[29];
#else
#error "unsupported architecture"
#endif
}

/**
 * Stop one thread of the game under ptrace, copy its registers and
 * stack into `snap`, and let it run again.
 *
 * @return `true` when `snap` holds a usable snapshot.
 */
static bool
snapshot_thread(pid_t game, pid_t tid, cw_crash_t* snap) {
	if (ptrace(PTRACE_SEIZE, tid, NULL, NULL) != 0) {
		cw_log(CW_LOG_WARN, "cannot attach to thread %d (%s)", (int)tid, strerror(errno));
		return false;
	}
	bool ok = false;
	int sig = 0;
	if (ptrace(PTRACE_INTERRUPT, tid, NULL, NULL) == 0) {
		int status;
		pid_t w;
		while ((w = waitpid(tid, &status, __WALL)) < 0 && errno == EINTR) {
		}
		if (w == tid && WIFSTOPPED(status)) {
			/* A signal that arrived first must be delivered on detach, not swallowed. */
			sig = (status >> 16) == PTRACE_EVENT_STOP ? 0 : WSTOPSIG(status);
			struct user_regs_struct regs;
			struct iovec iov = { .iov_base = &regs, .iov_len = sizeof(regs) };
			if (ptrace(PTRACE_GETREGSET, tid, (void*)NT_PRSTATUS, &iov) == 0) {
				*snap = (cw_crash_t){ .tid = tid };
				regs_to_context(&regs, &snap->uc);
#if defined(__x86_64__)
				uint64_t sp = regs.rsp;
#else
				uint64_t sp = regs.sp;
#endif
				uint64_t top = mapping_end(game, sp);
				if (top == 0 || top - sp > CW_STACK_CAP) {
					top = sp + CW_STACK_CAP;
				}
				struct iovec local = { .iov_base = snap->stack, .iov_len = top - sp };
				struct iovec remote = { .iov_base = (void*)(uintptr_t)sp, .iov_len = top - sp };
				ssize_t n = process_vm_readv(game, &local, 1, &remote, 1, 0);
				snap->sp = sp;
				snap->stack_top = top;
				snap->stack_len = n > 0 ? (size_t)n : 0;
				ok = true;
			}
		}
	}
	ptrace(PTRACE_DETACH, tid, NULL, (void*)(intptr_t)sig);
	return ok;
}

/**
 * Report a game whose heartbeat has been silent for `silent_ms`.
 *
 * @param path  Receives the envelope path on success.
 * @return `true` when the envelope was written.
 */
static bool
write_hang_report(pid_t game, const char* report_dir, uint64_t silent_ms, char* path, size_t cap) {
	static cw_crash_info_t info;
	static cw_crash_t snap;
	info = (cw_crash_info_t){ .main_module = -1 };
	pid_t tid = (pid_t)atomic_load_explicit(&cw_linux.region->common.heartbeat_tid, memory_order_relaxed);
	if (snapshot_thread(game, tid, &snap) && !cw_unwind(game, &snap, &info)) {
		cw_log(CW_LOG_WARN, "unwind produced no frames");
	}
	snprintf(info.type, sizeof(info.type), "HANG");
	snprintf(info.message_raw, sizeof(info.message_raw), "no heartbeat for %" PRIu64 " ms", silent_ms);
	info.fault_addr = 0;
	info.tid = (uint32_t)tid;
	if (!cw_write_envelope(report_dir, &info, &cw_linux.region->common, path, cap)) {
		return false;
	}
	cw_log(CW_LOG_INFO, "report written to %s", path);
	return true;
}

/**
 * Whether the game is in a job-control or tracing stop, where it cannot
 * tick through no fault of its own.
 */
static bool
game_stopped(pid_t game) {
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/status", (int)game);
	FILE* f = fopen(proc, "r");
	if (f == NULL) {
		return false;
	}
	bool stopped = false;
	char line[128];
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "State:", 6) == 0) {
			const char* state = line + 6;
			while (*state == ' ' || *state == '\t') {
				++state;
			}
			stopped = *state == 'T' || *state == 't';
			break;
		}
	}
	fclose(f);
	return stopped;
}

/**
 * Sample the heartbeat once and act on what the detector says.
 */
static void
check_hang(pid_t game, cw_hang_t* hang, const char* report_dir) {
	uint64_t now = cw_platform_now_ms();
	uint64_t count = atomic_load_explicit(&cw_linux.region->common.heartbeat, memory_order_relaxed);
	if (count == hang->count && game_stopped(game)) {
		cw_hang_reset(hang, now);
		return;
	}
	switch (cw_hang_step(hang, count, now, cw_ctx.cfg.hang_timeout_ms)) {
	case CW_HANG_REPORT: {
		uint64_t silent_ms = now - hang->since_ms;
		cw_log(CW_LOG_WARN, "no heartbeat for %" PRIu64 " ms", silent_ms);
		char path[CW_STR_CAP + 64];
		if (write_hang_report(game, report_dir, silent_ms, path, sizeof(path))) {
			cw_upload_report(path);
		}
		break;
	}
	case CW_HANG_RECOVERED:
		cw_log(CW_LOG_INFO, "heartbeat resumed");
		break;
	case CW_HANG_NONE:
		break;
	}
}

/**
 * Wait for messages until the game is gone, sampling the heartbeat at
 * a quarter of the hang timeout in between.
 */
static void
cw_watch(pid_t game, int sock, const char* report_dir) {
	int pidfd = pidfd_open(game, 0);
	bool crashed = false;
	bool shutdown = false;
	int result = 0;
	bool game_gone = false;
	cw_hang_t hang = { 0 };
	int interval_ms = cw_hang_poll_ms(cw_ctx.cfg.hang_timeout_ms);

	while (!game_gone) {
		struct pollfd fds[2] = {
			{ .fd = sock, .events = POLLIN },
			{ .fd = pidfd, .events = POLLIN },
		};
		int n = poll(fds, pidfd >= 0 ? 2 : 1, crashed ? -1 : interval_ms);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (!crashed) {
			check_hang(game, &hang, report_dir);
		}
		if (n == 0) {
			continue;
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
