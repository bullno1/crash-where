/**
 * @file linux.c
 * Linux implementation of the test platform functions.
 */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

extern char** environ;
extern char __executable_start;

/**
 * Concatenate the current environment and `extra` into a fresh array.
 */
static char**
build_envp(const char* const* extra) {
	size_t base = 0;
	while (environ[base] != NULL) {
		++base;
	}
	size_t added = 0;
	while (extra[added] != NULL) {
		++added;
	}
	char** envp = malloc((base + added + 1) * sizeof(char*));
	if (envp == NULL) {
		return NULL;
	}
	memcpy(envp, environ, base * sizeof(char*));
	for (size_t i = 0; i < added; ++i) {
		envp[base + i] = (char*)extra[i];
	}
	envp[base + added] = NULL;
	return envp;
}

bool
test_spawn_self(const char* const* env, test_exit_t* out) {
	char** envp = build_envp(env);
	if (envp == NULL) {
		return false;
	}

	/* The read end stays open until the last inheritor of stdout exits. */
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		free(envp);
		return false;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, pipefd[1]);

	char* const argv[] = { "cw_test", NULL };
	pid_t pid;
	int err = posix_spawn(&pid, "/proc/self/exe", &actions, NULL, argv, envp);
	posix_spawn_file_actions_destroy(&actions);
	free(envp);
	close(pipefd[1]);
	if (err != 0) {
		close(pipefd[0]);
		errno = err;
		return false;
	}

	char buf[4096];
	for (;;) {
		ssize_t n = read(pipefd[0], buf, sizeof(buf));
		if (n > 0) {
			fwrite(buf, 1, (size_t)n, stderr);
		} else if (n == 0 || errno != EINTR) {
			break;
		}
	}
	close(pipefd[0]);

	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return false;
		}
	}
	if (WIFSIGNALED(status)) {
		*out = (test_exit_t){ .signaled = true, .code = WTERMSIG(status) };
	} else {
		*out = (test_exit_t){ .code = WEXITSTATUS(status) };
	}
	return true;
}

bool
test_sockets_init(void) {
	return true;
}

bool
test_mkdir(const char* path) {
	return mkdir(path, 0777) == 0 || errno == EEXIST;
}

void
test_sleep_ms(unsigned ms) {
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

long
test_file_size(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int
test_stop_helper_main(const char* spec) {
	(void)spec;
	return 2; /* Never spawned: the fork below stands in for a helper. */
}

bool
test_stop_self(unsigned ms) {
	/* A forked helper resumes us, since a stopped process cannot resume itself. */
	pid_t helper = fork();
	if (helper < 0) {
		return false;
	}
	if (helper == 0) {
		test_sleep_ms(ms);
		kill(getppid(), SIGCONT);
		_exit(0);
	}
	raise(SIGSTOP);
	while (waitpid(helper, NULL, 0) < 0 && errno == EINTR) {
	}
	return true;
}

uintptr_t
test_image_base(void) {
	return (uintptr_t)&__executable_start;
}

bool
test_under_wine(void) {
	return false;
}

bool
test_displace_crash_handler(void) {
	return false;
}
