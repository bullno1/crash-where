/**
 * @file windows/game.c
 * Game side: create the section and events, spawn the watcher, arm capture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "windows/platform.h"

#define CW_READY_TIMEOUT 2000
#define CW_MAX_INHERIT   11 /* Eight shared handles and three standard ones. */

bool
cw_platform_debugger_present(void) {
	return IsDebuggerPresent();
}

/**
 * Copy the environment block without any watcher entry and append one.
 *
 * @return A block to free, or `NULL` on failure.
 */
static wchar_t*
build_env(const wchar_t* watcher_var) {
	static const wchar_t prefix[] = L"" CW_ENV_WATCHER L"=";
	size_t prefix_len = sizeof(prefix) / sizeof(prefix[0]) - 1;

	wchar_t* env = GetEnvironmentStringsW();
	if (env == NULL) {
		return NULL;
	}
	size_t len = 0;
	while (env[len] != L'\0') {
		len += wcslen(env + len) + 1;
	}
	size_t extra = wcslen(watcher_var) + 1;
	wchar_t* out = malloc((len + extra + 1) * sizeof(wchar_t));
	if (out != NULL) {
		size_t pos = 0;
		for (size_t i = 0; env[i] != L'\0';) {
			size_t n = wcslen(env + i) + 1;
			if (_wcsnicmp(env + i, prefix, prefix_len) != 0) {
				memcpy(out + pos, env + i, n * sizeof(wchar_t));
				pos += n;
			}
			i += n;
		}
		memcpy(out + pos, watcher_var, extra * sizeof(wchar_t));
		out[pos + extra] = L'\0';
	}
	FreeEnvironmentStringsW(env);
	return out;
}

static HANDLE
make_event(void) {
	SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa), .bInheritHandle = TRUE };
	return CreateEventW(&sa, FALSE, FALSE, NULL);
}

/**
 * Make a standard handle inheritable and add it to `list` once.
 *
 * The watcher writes its diagnostics to the game's stdio, and a test
 * runner learns that it exited from the pipe closing.
 *
 * @return The handle, or `NULL` when there is none to inherit.
 */
static HANDLE
add_std_handle(DWORD id, HANDLE* list, DWORD* count) {
	HANDLE h = GetStdHandle(id);
	if (h == NULL || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
		return NULL;
	}
	if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
		return NULL;
	}
	for (DWORD i = 0; i < *count; ++i) {
		if (list[i] == h) {
			return h;
		}
	}
	list[(*count)++] = h;
	return h;
}

/**
 * Start this binary again as the watcher.
 *
 * Only the listed handles are inherited. A launcher's job object would
 * kill the watcher together with the game, so it breaks away when the
 * job allows it.
 *
 * @return The watcher process handle, or `NULL` on failure.
 */
static HANDLE
spawn_watcher(const wchar_t* env, HANDLE* inherit, DWORD count, const STARTUPINFOW* std) {
	wchar_t exe[MAX_PATH];
	DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
	if (exe_len == 0 || exe_len >= MAX_PATH) {
		return NULL;
	}
	/* CreateProcessW may edit the command line, so it gets a copy. */
	wchar_t* cmdline = _wcsdup(GetCommandLineW());

	SIZE_T size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(size);
	if (cmdline == NULL || attrs == NULL || !InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
		free(cmdline);
		free(attrs);
		return NULL;
	}

	STARTUPINFOEXW si = { .StartupInfo = *std, .lpAttributeList = attrs };
	si.StartupInfo.cb = sizeof(si);
	PROCESS_INFORMATION pi = { 0 };
	DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
	bool ok = UpdateProcThreadAttribute(
		attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		inherit, count * sizeof(HANDLE), NULL, NULL
	);
	ok = ok && (
		CreateProcessW(exe, cmdline, NULL, NULL, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, (void*)env, NULL, &si.StartupInfo, &pi)
		|| CreateProcessW(exe, cmdline, NULL, NULL, TRUE, flags, (void*)env, NULL, &si.StartupInfo, &pi)
	);

	DeleteProcThreadAttributeList(attrs);
	free(attrs);
	free(cmdline);
	if (!ok) {
		return NULL;
	}
	CloseHandle(pi.hThread);
	return pi.hProcess;
}

/**
 * Game path: create the section and events, spawn the watcher, wait
 * for it to report ready, arm the handlers.
 */
bool
cw_platform_run_game(void) {
	/* Non-NULL until the last step; any early exit leaves the reason here. */
	const char* fail = "unknown error";
	cw_handles_t h = { 0 };
	cw_region_t* region = NULL;
	HANDLE watcher = NULL;
	wchar_t* env = NULL;
	DWORD err = 0;

	SECURITY_ATTRIBUTES sa = { .nLength = sizeof(sa), .bInheritHandle = TRUE };
	h.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)sizeof(cw_region_t), NULL);
	if (h.section == NULL) {
		fail = "cannot create shared region";
		goto end;
	}
	region = MapViewOfFile(h.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cw_region_t));
	if (region == NULL) {
		fail = "cannot map shared region";
		goto end;
	}
	*region = (cw_region_t){ .common = { .magic = CW_REGION_MAGIC } };

	/* What the watcher needs to wait on the game, read its memory, and dump it. */
	DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE;
	HANDLE current_process = GetCurrentProcess();
	if (!DuplicateHandle(current_process, current_process, current_process, &h.game, access, TRUE, 0)) {
		fail = "cannot duplicate process handle";
		goto end;
	}
	h.ev_crash = make_event();
	h.ev_done = make_event();
	h.ev_ready = make_event();
	h.ev_shutdown = make_event();
	h.ev_auth = make_event();
	h.ev_consent = make_event();
	if (
		h.ev_crash == NULL || h.ev_done == NULL || h.ev_ready == NULL
		|| h.ev_shutdown == NULL || h.ev_auth == NULL || h.ev_consent == NULL
	) {
		fail = "cannot create events";
		goto end;
	}

	wchar_t watcher_var[200];
	swprintf(
		watcher_var, sizeof(watcher_var) / sizeof(watcher_var[0]),
		L"" CW_ENV_WATCHER L"=%lu,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx",
		GetCurrentProcessId(),
		(unsigned long long)(uintptr_t)h.section, (unsigned long long)(uintptr_t)h.game,
		(unsigned long long)(uintptr_t)h.ev_crash, (unsigned long long)(uintptr_t)h.ev_done,
		(unsigned long long)(uintptr_t)h.ev_ready, (unsigned long long)(uintptr_t)h.ev_shutdown,
		(unsigned long long)(uintptr_t)h.ev_auth, (unsigned long long)(uintptr_t)h.ev_consent
	);
	env = build_env(watcher_var);
	if (env == NULL) {
		fail = "cannot build watcher environment";
		goto end;
	}

	HANDLE inherit[CW_MAX_INHERIT] = {
		h.section, h.game, h.ev_crash, h.ev_done, h.ev_ready, h.ev_shutdown, h.ev_auth, h.ev_consent,
	};
	DWORD count = 8;
	STARTUPINFOW std = { .dwFlags = STARTF_USESTDHANDLES };
	std.hStdInput = add_std_handle(STD_INPUT_HANDLE, inherit, &count);
	std.hStdOutput = add_std_handle(STD_OUTPUT_HANDLE, inherit, &count);
	std.hStdError = add_std_handle(STD_ERROR_HANDLE, inherit, &count);

	watcher = spawn_watcher(env, inherit, count, &std);
	if (watcher == NULL) {
		fail = "cannot start watcher";
		goto end;
	}

	HANDLE ready[2] = { h.ev_ready, watcher };
	if (WaitForMultipleObjects(2, ready, FALSE, CW_READY_TIMEOUT) != WAIT_OBJECT_0) {
		fail = "watcher did not report ready";
		goto end;
	}

	/* The watcher holds its own copies; the game keeps the mapping and the events it still signals or waits on. */
	CloseHandle(h.section);
	CloseHandle(h.game);
	CloseHandle(h.ev_ready);
	h.section = NULL;
	h.game = NULL;
	h.ev_ready = NULL;

	cw_win.region = region;
	cw_win.handles = h;
	cw_win.watcher = watcher;
	cw_ctx.shared = &region->common;
	cw_install_exception_handler();
	cw_platform_attach_thread();
	cw_log(CW_LOG_INFO, "watcher pid %lu ready", GetProcessId(watcher));
	/* Paths pass through the ANSI code page; only UTF-8 survives into the report intact. */
	if (GetACP() != CP_UTF8) {
		cw_log(
			CW_LOG_WARN, "code page %u is not UTF-8, non-ASCII paths will be misreported;"
			" declare activeCodePage UTF-8 in the application manifest", GetACP()
		);
	}
	fail = NULL;

end:
	err = GetLastError();
	free(env);
	if (fail != NULL) {
		cw_log(CW_LOG_WARN, "%s (error %lu), library inactive", fail, err);
		if (watcher != NULL) {
			TerminateProcess(watcher, 1);
			CloseHandle(watcher);
		}
		if (region != NULL) {
			UnmapViewOfFile(region);
		}
		HANDLE* handles[] = {
			&h.section, &h.game, &h.ev_crash, &h.ev_done, &h.ev_ready, &h.ev_shutdown, &h.ev_auth, &h.ev_consent,
		};
		for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
			if (*handles[i] != NULL) {
				CloseHandle(*handles[i]);
			}
		}
	}
	return fail == NULL;
}

void
cw_platform_shutdown(int result) {
	cw_win.region->shutdown_result = result;
	SetEvent(cw_win.handles.ev_shutdown);
}

void
cw_platform_notify_auth(unsigned what) {
	atomic_fetch_or_explicit(&cw_win.region->auth, what, memory_order_release);
	SetEvent(cw_win.handles.ev_auth);
}

void
cw_platform_notify_consent(cw_consent_t choice) {
	atomic_store_explicit(&cw_win.region->consent, (int32_t)choice, memory_order_release);
	SetEvent(cw_win.handles.ev_consent);
}
