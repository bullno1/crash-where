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

typedef struct {
	void (*fn)(void);
} thread_arg_t;

static DWORD WINAPI
thread_main(void* arg) {
	((thread_arg_t*)arg)->fn();
	return 0;
}

bool
test_run_thread(void (*fn)(void)) {
	thread_arg_t arg = { .fn = fn };
	HANDLE thread = CreateThread(NULL, 0, thread_main, &arg, 0, NULL);
	if (thread == NULL) {
		return false;
	}
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
	return true;
}

static LONG WINAPI
end_on_crash(EXCEPTION_POINTERS* info) {
	(void)info;
	return EXCEPTION_EXECUTE_HANDLER;
}

void
test_platform_init(void) {
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
	/* Consulted before any debugger is started; the library replaces it with its own. */
	SetUnhandledExceptionFilter(end_on_crash);
	SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

bool
test_mkdir(const char* path) {
	return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool
test_remove_tree(const char* path) {
	char pattern[1024];
	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE) {
		return GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND;
	}
	bool ok = true;
	do {
		if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
			continue;
		}
		char child[1024];
		snprintf(child, sizeof(child), "%s\\%s", path, data.cFileName);
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			ok = test_remove_tree(child) && ok;
		} else {
			ok = DeleteFileA(child) && ok;
		}
	} while (FindNextFileA(find, &data));
	FindClose(find);
	return RemoveDirectoryA(path) && ok;
}

int
test_count_files(const char* path, const char* suffix) {
	char pattern[1024];
	snprintf(pattern, sizeof(pattern), "%s\\*", path);
	WIN32_FIND_DATAA data;
	HANDLE find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE) {
		return -1;
	}
	int count = 0;
	size_t suffix_len = strlen(suffix);
	do {
		size_t len = strlen(data.cFileName);
		if (len >= suffix_len && strcmp(data.cFileName + len - suffix_len, suffix) == 0) {
			++count;
		}
	} while (FindNextFileA(find, &data));
	FindClose(find);
	return count;
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

/**
 * Act as a debugger that lets its target run to the end.
 */
static int
debug_until_exit(void) {
	for (;;) {
		DEBUG_EVENT ev;
		if (!WaitForDebugEvent(&ev, INFINITE)) {
			return 3;
		}
		DWORD status = DBG_CONTINUE;
		switch (ev.dwDebugEventCode) {
		case CREATE_PROCESS_DEBUG_EVENT:
			CloseHandle(ev.u.CreateProcessInfo.hFile);
			break;
		case LOAD_DLL_DEBUG_EVENT:
			CloseHandle(ev.u.LoadDll.hFile);
			break;
		case EXCEPTION_DEBUG_EVENT:
			/* Attaching breaks in once; anything else belongs to the target. */
			if (ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) {
				status = DBG_EXCEPTION_NOT_HANDLED;
			}
			break;
		default:
			break;
		}
		ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
		if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
			return 0;
		}
	}
}

int
test_stop_helper_main(const char* spec) {
	unsigned long pid;
	unsigned ms;
	int fields = sscanf(spec, "%lu,%u", &pid, &ms);
	if (fields < 1) {
		return 2;
	}
	if (!DebugActiveProcess(pid)) {
		return 3;
	}
	DebugSetProcessKillOnExit(FALSE);
	/* Without a duration the target is debugged rather than stopped. */
	if (fields < 2) {
		int result = debug_until_exit();
		/*
		 * Outlive the target: a debugger that quits while its dead target is
		 * still being torn down leaves that process half alive under Wine,
		 * holding every handle it inherited.
		 */
		HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (target != NULL) {
			WaitForSingleObject(target, INFINITE);
			CloseHandle(target);
		}
		DebugActiveProcessStop(pid);
		return result;
	}
	/* Attaching queues a debug event that is never continued, so the target stays frozen. */
	Sleep(ms);
	return DebugActiveProcessStop(pid) ? 0 : 3;
}

/**
 * Start this executable again as the helper, with `spec` in `CW_TEST_STOP`.
 *
 * @return The helper's process handle, or `NULL` on failure.
 */
static HANDLE
spawn_helper(const char* spec) {
	const char* extra[] = { spec, NULL };
	char* block = build_env(extra);
	if (block == NULL) {
		return NULL;
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
		return NULL;
	}
	CloseHandle(pi.hThread);
	return pi.hProcess;
}

bool
test_stop_self(unsigned ms) {
	/* A helper attaches as our debugger; a process cannot break itself from outside. */
	char spec[64];
	snprintf(spec, sizeof(spec), "CW_TEST_STOP=%lu,%u", GetCurrentProcessId(), ms);
	HANDLE helper = spawn_helper(spec);
	if (helper == NULL) {
		return false;
	}
	WaitForSingleObject(helper, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(helper, &code);
	CloseHandle(helper);
	return code == 0;
}

bool
test_debug_self(void) {
	char spec[64];
	snprintf(spec, sizeof(spec), "CW_TEST_STOP=%lu", GetCurrentProcessId());
	HANDLE helper = spawn_helper(spec);
	if (helper == NULL) {
		return false;
	}
	/* The helper outlives this call; it ending early means the attach failed. */
	while (!IsDebuggerPresent() && WaitForSingleObject(helper, 10) == WAIT_TIMEOUT) {
	}
	CloseHandle(helper);
	return IsDebuggerPresent();
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
