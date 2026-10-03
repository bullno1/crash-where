/**
 * @file windows/minidump.c
 * A minidump of the game, written by the watcher through dbghelp.
 */
#include "windows/platform.h"

#include <dbghelp.h>
#include <stdio.h>
#include <wchar.h>

/**
 * What the dump holds beyond the stacks: the memory the stacks point
 * at, thread details, the unloaded module list, the address space map,
 * the vector registers, and the data segments of the game's own
 * modules. Never the heap.
 */
#define CW_MINIDUMP_TYPE ( \
	MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo \
	| MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo \
	| MiniDumpWithAvxXStateContext | MiniDumpWithDataSegs \
)

/**
 * Keep data segments only for modules under the game's directory;
 * system DLLs would add megabytes nobody reads.
 *
 * @param user  The game's directory with its trailing backslash, empty when unknown.
 */
static BOOL CALLBACK
on_dump_item(PVOID user, PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
	if (in->CallbackType == ModuleCallback) {
		const wchar_t* dir = user;
		size_t len = wcslen(dir);
		bool own = len > 0 && in->Module.FullPath != NULL && _wcsnicmp(in->Module.FullPath, dir, len) == 0;
		if (!own) {
			out->ModuleWriteFlags &= ~(ULONG)ModuleWriteDataSeg;
		}
	}
	return TRUE;
}

bool
cw_write_minidump(HANDLE game, const cw_crash_t* crash, char* out, size_t cap) {
	int n = snprintf(out, cap, "%s/pending/%lu.dmp.tmp", cw_ctx.report_dir, GetCurrentProcessId());
	if (n <= 0 || (size_t)n >= cap) {
		out[0] = '\0';
		return false;
	}

	/* The game runs from this directory, since the watcher is the same binary. */
	wchar_t dir[MAX_PATH];
	DWORD dir_len = GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t* slash = dir_len > 0 && dir_len < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
	if (slash != NULL) {
		slash[1] = L'\0';
	} else {
		dir[0] = L'\0';
	}

	HANDLE file = CreateFileA(out, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		cw_log(CW_LOG_WARN, "cannot create %s (error %lu)", out, GetLastError());
		out[0] = '\0';
		return false;
	}

	/* The record and context are the handler's copies in the region, not pointers into the game. */
	EXCEPTION_POINTERS pointers = { 0 };
	MINIDUMP_EXCEPTION_INFORMATION exception = { .ExceptionPointers = &pointers, .ClientPointers = FALSE };
	if (crash != NULL) {
		pointers.ExceptionRecord = (EXCEPTION_RECORD*)&crash->record;
		pointers.ContextRecord = (CONTEXT*)&crash->context;
		exception.ThreadId = crash->tid;
	}
	MINIDUMP_CALLBACK_INFORMATION callback = { .CallbackRoutine = on_dump_item, .CallbackParam = dir };
	BOOL ok = MiniDumpWriteDump(
		game, GetProcessId(game), file, CW_MINIDUMP_TYPE,
		crash != NULL ? &exception : NULL, NULL, &callback
	);
	DWORD error = GetLastError();
	CloseHandle(file);
	if (!ok) {
		cw_log(CW_LOG_WARN, "cannot write a minidump (error 0x%lx)", error);
		cw_platform_remove(out);
		out[0] = '\0';
		return false;
	}
	return true;
}
