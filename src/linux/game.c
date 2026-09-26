/**
 * @file linux/game.c
 * Game side: create the region and socket, spawn the watcher, arm capture.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linux/platform.h"

#define CW_READY_TIMEOUT 2000

extern char** environ;

/**
 * Build the watcher's argv from `/proc/self/cmdline`.
 *
 * @param storage  Receives a buffer to free after spawning.
 * @return A `NULL`-terminated array to free, or `NULL` on failure.
 */
static char**
build_argv(char** storage) {
	*storage = NULL;
	int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	size_t len = 0;
	size_t cap = 4096;
	char* buf = malloc(cap);
	while (buf != NULL) {
		ssize_t n = read(fd, buf + len, cap - len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		len += (size_t)n;
		if (len == cap) {
			char* grown = realloc(buf, cap * 2);
			if (grown == NULL) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = grown;
			cap *= 2;
		}
	}
	close(fd);
	if (buf == NULL || len == 0) {
		free(buf);
		return NULL;
	}
	int count = 0;
	for (size_t i = 0; i < len; ++i) {
		if (buf[i] == '\0') {
			++count;
		}
	}
	char** argv = calloc((size_t)count + 1, sizeof(char*));
	if (argv == NULL) {
		free(buf);
		return NULL;
	}
	count = 0;
	argv[count++] = buf;
	for (size_t i = 0; i + 1 < len; ++i) {
		if (buf[i] == '\0') {
			argv[count++] = buf + i + 1;
		}
	}
	argv[count] = NULL;
	*storage = buf;
	return argv;
}

/**
 * Copy `environ` without any watcher entry and append one.
 */
static char**
build_envp(const char* watcher_var) {
	size_t n = 0;
	while (environ[n] != NULL) {
		++n;
	}
	char** envp = calloc(n + 2, sizeof(char*));
	if (envp == NULL) {
		return NULL;
	}
	size_t out = 0;
	for (size_t i = 0; i < n; ++i) {
		if (strncmp(environ[i], CW_ENV_WATCHER "=", sizeof(CW_ENV_WATCHER)) != 0) {
			envp[out++] = environ[i];
		}
	}
	envp[out++] = (char*)watcher_var;
	envp[out] = NULL;
	return envp;
}

/**
 * vfork and exec this binary as the watcher.
 *
 * The child shares the parent's memory until it execs, so it only runs
 * syscalls and stores its exec `errno` into `err`, which the parent reads
 * after `vfork` returns. The parent's own `errno` is meaningless then.
 *
 * @param closefrom  First fd the child closes; everything above it goes.
 * @param err        Receives the child's exec `errno`, 0 on success.
 * @return The watcher pid, or -1 when `vfork` itself failed.
 */
__attribute__((noinline)) static pid_t
spawn_watcher(char* const* argv, char* const* envp, int closefrom, int* err) {
	volatile int child_err = 0;
	pid_t pid = vfork();
	if (pid == 0) {
		setsid();
		close_range((unsigned)closefrom, ~0u, 0);
		execve("/proc/self/exe", argv, envp);
		child_err = errno;
		_exit(127);
	}
	*err = pid < 0 ? errno : child_err;
	return pid;
}

/**
 * Game path: create the region and socket, spawn the watcher, wait
 * for it to report ready, arm the handlers.
 */
bool
cw_platform_run_game(void) {
	/* Non-NULL until the last step; any early exit leaves the reason here. */
	const char* fail = "unknown error";
	char** argv = NULL;
	char* argv_storage = NULL;
	char** envp = NULL;
	int sock[2] = { -1, -1 };
	cw_region_t* region = MAP_FAILED;

	int memfd = memfd_create("cw-region", MFD_ALLOW_SEALING);
	if (memfd < 0 || ftruncate(memfd, (off_t)sizeof(cw_region_t)) != 0) {
		fail = "cannot create shared region";
		goto end;
	}
	fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);

	region = mmap(NULL, sizeof(cw_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	if (region == MAP_FAILED) {
		fail = "cannot map shared region";
		goto end;
	}
	region->common = (cw_shared_t){ .magic = CW_REGION_MAGIC };

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock) != 0) {
		fail = "cannot create socketpair";
		goto end;
	}
	fcntl(sock[0], F_SETFD, FD_CLOEXEC);
	char watcher_var[96];
	snprintf(
		watcher_var, sizeof(watcher_var), CW_ENV_WATCHER "=%d,%d,%d",
		(int)getpid(), memfd, sock[1]
	);

	argv = build_argv(&argv_storage);
	envp = build_envp(watcher_var);
	if (argv == NULL || envp == NULL) {
		fail = "cannot build watcher command line";
		goto end;
	}

	int closefrom = (memfd > sock[1] ? memfd : sock[1]) + 1;
	int err = 0;
	pid_t pid = spawn_watcher(argv, envp, closefrom, &err);
	if (pid < 0 || err != 0) {
		errno = err;
		fail = "cannot exec watcher";
		if (pid > 0) {
			waitpid(pid, NULL, 0);
		}
		goto end;
	}

	if (prctl(PR_SET_PTRACER, (unsigned long)pid, 0, 0, 0) != 0) {
		cw_log(CW_LOG_DEBUG, "PR_SET_PTRACER failed (%s)", strerror(errno));
	}

	cw_msg_t msg;
	if (!cw_recv_msg(sock[0], &msg, CW_READY_TIMEOUT) || msg.type != CW_MSG_READY) {
		errno = ETIMEDOUT;
		fail = "watcher did not report ready";
		waitpid(pid, NULL, WNOHANG);
		goto end;
	}

	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void* stack_addr;
		size_t stack_size;
		if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0) {
			cw_linux.main_stack_lo = (uintptr_t)stack_addr;
			cw_linux.main_stack_hi = (uintptr_t)stack_addr + stack_size;
		}
		pthread_attr_destroy(&attr);
	}
	cw_linux.region = region;
	cw_linux.sock = sock[0];
	cw_linux.watcher = pid;
	cw_ctx.shared = &region->common;
	cw_signal_install();
	cw_log(CW_LOG_INFO, "watcher pid %d ready", (int)pid);
	fail = NULL;

end:
	/* The spawn inputs and the watcher's copies of the fds are not needed either way. */
	free(argv);
	free(argv_storage);
	free(envp);
	if (memfd >= 0) {
		close(memfd);
	}
	if (sock[1] >= 0) {
		close(sock[1]);
	}
	if (fail != NULL) {
		cw_log(CW_LOG_WARN, "%s (%s), library inactive", fail, strerror(errno));
		if (region != MAP_FAILED) {
			munmap(region, sizeof(cw_region_t));
		}
		if (sock[0] >= 0) {
			close(sock[0]);
		}
	}
	return fail == NULL;
}

void
cw_platform_shutdown(int result) {
	cw_send_msg(cw_linux.sock, CW_MSG_SHUTDOWN, result);
}
