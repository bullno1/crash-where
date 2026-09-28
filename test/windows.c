/**
 * @file windows.c
 * Windows implementation of the test platform functions.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"

extern IMAGE_DOS_HEADER __ImageBase;

/**
 * Concatenate the current environment block and `extra` into a fresh block.
 */
static char*
build_env(const char* const* extra) {
	char* env = GetEnvironmentStringsA();
	if (env == NULL) {
		return NULL;
	}
	size_t len = 0;
	while (env[len] != '\0') {
		len += strlen(env + len) + 1;
	}
	size_t added = 0;
	for (size_t i = 0; extra[i] != NULL; ++i) {
		added += strlen(extra[i]) + 1;
	}
	char* block = malloc(len + added + 1);
	if (block != NULL) {
		memcpy(block, env, len);
		size_t pos = len;
		for (size_t i = 0; extra[i] != NULL; ++i) {
			size_t n = strlen(extra[i]) + 1;
			memcpy(block + pos, extra[i], n);
			pos += n;
		}
		block[pos] = '\0';
	}
	FreeEnvironmentStringsA(env);
	return block;
}

/**
 * Fold the status a crashed process ends with into the signal number
 * the suite checks, so one set of expectations covers every platform.
 */
static test_exit_t
exit_from_code(DWORD code) {
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_STACK_OVERFLOW:
		return (test_exit_t){ .signaled = true, .code = SIGSEGV };
	case STATUS_STACK_BUFFER_OVERRUN: /* How abort() ends: a fast-fail. */
		return (test_exit_t){ .signaled = true, .code = SIGABRT };
	default:
		return (test_exit_t){ .code = (int)code };
	}
}

bool
test_spawn_self(const char* const* env, test_exit_t* out) {
	char* block = build_env(env);
	if (block == NULL) {
		return false;
	}

	/* The read end stays open until the last inheritor of stdout exits. */
	SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa), .bInheritHandle = TRUE };
	HANDLE rd;
	HANDLE wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		free(block);
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

	char exe[MAX_PATH];
	DWORD exe_len = GetModuleFileNameA(NULL, exe, sizeof(exe));
	char cmdline[] = "cw_test";
	STARTUPINFOA si = {
		.cb = sizeof(si),
		.dwFlags = STARTF_USESTDHANDLES,
		.hStdInput = GetStdHandle(STD_INPUT_HANDLE),
		.hStdOutput = wr,
		.hStdError = err,
	};
	PROCESS_INFORMATION pi = { 0 };
	bool ok = exe_len > 0 && exe_len < sizeof(exe)
		&& CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0, block, NULL, &si, &pi);
	free(block);
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		return false;
	}
	CloseHandle(pi.hThread);

	char buf[4096];
	DWORD got;
	while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got > 0) {
		fwrite(buf, 1, got, stderr);
	}
	CloseHandle(rd);

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	*out = exit_from_code(code);
	return true;
}

bool
test_sockets_init(void) {
	WSADATA data;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

bool
test_mkdir(const char* path) {
	return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void
test_sleep_ms(unsigned ms) {
	Sleep(ms);
}

long
test_file_size(const char* path) {
	struct _stat64 st;
	return _stat64(path, &st) == 0 ? (long)st.st_size : -1;
}

int
test_stop_helper_main(const char* spec) {
	unsigned long pid;
	unsigned ms;
	if (sscanf(spec, "%lu,%u", &pid, &ms) != 2) {
		return 2;
	}
	/* Attaching queues a debug event that is never continued, so the target stays frozen. */
	if (!DebugActiveProcess(pid)) {
		return 3;
	}
	DebugSetProcessKillOnExit(FALSE);
	Sleep(ms);
	return DebugActiveProcessStop(pid) ? 0 : 3;
}

bool
test_stop_self(unsigned ms) {
	/* A helper attaches as our debugger; a process cannot break itself from outside. */
	char spec[64];
	snprintf(spec, sizeof(spec), "CW_TEST_STOP=%lu,%u", GetCurrentProcessId(), ms);
	const char* extra[] = { spec, NULL };
	char* block = build_env(extra);
	if (block == NULL) {
		return false;
	}
	char exe[MAX_PATH];
	DWORD exe_len = GetModuleFileNameA(NULL, exe, sizeof(exe));
	char cmdline[] = "cw_test";
	STARTUPINFOA si = { .cb = sizeof(si) };
	PROCESS_INFORMATION pi = { 0 };
	bool ok = exe_len > 0 && exe_len < sizeof(exe)
		&& CreateProcessA(exe, cmdline, NULL, NULL, FALSE, 0, block, NULL, &si, &pi);
	free(block);
	if (!ok) {
		return false;
	}
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return code == 0;
}

uintptr_t
test_image_base(void) {
	return (uintptr_t)&__ImageBase;
}

bool
test_self_path(char* buf, size_t cap) {
	DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)cap);
	return len > 0 && len < cap;
}

bool
test_under_wine(void) {
	return GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
}

static LONG WINAPI
inert_filter(EXCEPTION_POINTERS* ep) {
	(void)ep;
	return EXCEPTION_CONTINUE_SEARCH;
}

bool
test_displace_crash_handler(void) {
	SetUnhandledExceptionFilter(inert_filter);
	return true;
}
