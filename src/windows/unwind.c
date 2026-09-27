/**
 * @file windows/unwind.c
 * Module table from a toolhelp snapshot, CodeView build ids, and a
 * dbghelp stack walk over the parked game.
 */
#include "windows/platform.h"

#include <dbghelp.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

#define CW_MAX_DEPTH      64
#define CW_CODEVIEW_RSDS  0x53445352u /* "RSDS", the PDB 7.0 CodeView record. */

/**
 * Build the module table from a snapshot of the game. The first entry
 * of a snapshot is the executable itself.
 */
static void
read_modules(HANDLE game, cw_crash_info_t* info) {
	info->module_count = 0;
	info->main_module = -1;

	/* The snapshot fails with ERROR_BAD_LENGTH while the module list changes. */
	HANDLE snap = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 5 && snap == INVALID_HANDLE_VALUE; ++attempt) {
		snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(game));
		if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) {
			return;
		}
	}
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}

	MODULEENTRY32 me = { .dwSize = sizeof(me) };
	for (BOOL ok = Module32First(snap, &me); ok && info->module_count < CW_MAX_MODULES; ok = Module32Next(snap, &me)) {
		cw_module_t* m = &info->modules[info->module_count];
		*m = (cw_module_t){ .base = (uintptr_t)me.modBaseAddr, .size = me.modBaseSize };
		snprintf(m->path, sizeof(m->path), "%s", me.szExePath);
		if (info->main_module < 0) {
			info->main_module = info->module_count;
		}
		++info->module_count;
	}
	CloseHandle(snap);
}

/**
 * Read the CodeView record of a PE file: the PDB GUID followed by the
 * age, as hex.
 */
static void
read_build_id(const char* path, char* out, size_t cap) {
	out[0] = '\0';
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return;
	}

	IMAGE_DOS_HEADER dos;
	DWORD signature;
	IMAGE_FILE_HEADER fh;
	union {
		IMAGE_OPTIONAL_HEADER32 h32;
		IMAGE_OPTIONAL_HEADER64 h64;
	} opt;
	if (fread(&dos, sizeof(dos), 1, f) != 1 || dos.e_magic != IMAGE_DOS_SIGNATURE
		|| fseek(f, dos.e_lfanew, SEEK_SET) != 0
		|| fread(&signature, sizeof(signature), 1, f) != 1 || signature != IMAGE_NT_SIGNATURE
		|| fread(&fh, sizeof(fh), 1, f) != 1
		|| fh.SizeOfOptionalHeader > sizeof(opt)
		|| fread(&opt, fh.SizeOfOptionalHeader, 1, f) != 1) {
		fclose(f);
		return;
	}
	IMAGE_DATA_DIRECTORY dir;
	if (opt.h32.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		dir = opt.h32.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
	} else if (opt.h64.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		dir = opt.h64.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
	} else {
		fclose(f);
		return;
	}

	/* The directory is given as an RVA; find the section holding it. */
	long dir_off = -1;
	for (int i = 0; i < fh.NumberOfSections && dir_off < 0; ++i) {
		IMAGE_SECTION_HEADER sec;
		if (fread(&sec, sizeof(sec), 1, f) != 1) {
			break;
		}
		if (dir.VirtualAddress >= sec.VirtualAddress && dir.VirtualAddress < sec.VirtualAddress + sec.Misc.VirtualSize) {
			dir_off = (long)(dir.VirtualAddress - sec.VirtualAddress + sec.PointerToRawData);
		}
	}

	for (DWORD i = 0; dir_off >= 0 && (i + 1) * sizeof(IMAGE_DEBUG_DIRECTORY) <= dir.Size; ++i) {
		IMAGE_DEBUG_DIRECTORY dd;
		if (fseek(f, dir_off + (long)(i * sizeof(dd)), SEEK_SET) != 0 || fread(&dd, sizeof(dd), 1, f) != 1) {
			break;
		}
		if (dd.Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
			continue;
		}
		struct {
			DWORD signature;
			GUID guid;
			DWORD age;
		} cv;
		if (dd.PointerToRawData == 0
			|| fseek(f, (long)dd.PointerToRawData, SEEK_SET) != 0
			|| fread(&cv, sizeof(cv), 1, f) != 1
			|| cv.signature != CW_CODEVIEW_RSDS) {
			break;
		}
		snprintf(
			out, cap, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x%lx",
			cv.guid.Data1, cv.guid.Data2, cv.guid.Data3,
			cv.guid.Data4[0], cv.guid.Data4[1], cv.guid.Data4[2], cv.guid.Data4[3],
			cv.guid.Data4[4], cv.guid.Data4[5], cv.guid.Data4[6], cv.guid.Data4[7],
			cv.age
		);
		break;
	}
	fclose(f);
}

static void
add_frame(cw_crash_info_t* info, uint64_t addr) {
	if (info->frame_count >= CW_MAX_FRAMES) {
		return;
	}
	cw_frame_t* fr = &info->frames[info->frame_count++];
	fr->module = -1;
	fr->offset = addr;
	for (int i = 0; i < info->module_count; ++i) {
		const cw_module_t* m = &info->modules[i];
		if (addr >= m->base && addr < m->base + m->size) {
			fr->module = i;
			fr->offset = addr - m->base;
			return;
		}
	}
}

static const char*
exception_name(DWORD code) {
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
	case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
	case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
	case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
	case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
	case EXCEPTION_BREAKPOINT:            return "EXCEPTION_BREAKPOINT";
	case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
	case EXCEPTION_INT_OVERFLOW:          return "EXCEPTION_INT_OVERFLOW";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
	case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
	case CW_STATUS_CPP_EXCEPTION:         return "CPP_EXCEPTION";
	case CW_STATUS_ABORT:                 return "ABORT";
	default:                              return NULL;
	}
}

bool
cw_unwind(HANDLE game, const cw_crash_t* crash, cw_crash_info_t* out) {
	read_modules(game, out);
	for (int i = 0; i < out->module_count; ++i) {
		read_build_id(out->modules[i].path, out->modules[i].build_id, sizeof(out->modules[i].build_id));
	}

	const EXCEPTION_RECORD* rec = &crash->record;
	const char* name = exception_name(rec->ExceptionCode);
	if (name != NULL) {
		snprintf(out->type, sizeof(out->type), "%s", name);
	} else {
		snprintf(out->type, sizeof(out->type), "EXCEPTION_0x%08lx", rec->ExceptionCode);
	}
	out->tid = crash->tid;
	if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
		ULONG_PTR kind = rec->ExceptionInformation[0];
		const char* verb = kind == EXCEPTION_EXECUTE_FAULT ? "executing"
			: kind == EXCEPTION_WRITE_FAULT ? "writing"
			: "reading";
		out->fault_addr = rec->ExceptionInformation[1];
		snprintf(out->message_raw, sizeof(out->message_raw), "%s address 0x%" PRIx64, verb, out->fault_addr);
	} else {
		snprintf(out->message_raw, sizeof(out->message_raw), "code 0x%08lx", rec->ExceptionCode);
	}

	CONTEXT ctx = crash->context;
	STACKFRAME64 frame = { 0 };
	DWORD machine;
#if defined(_M_X64)
	machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
	machine = IMAGE_FILE_MACHINE_ARM64;
	frame.AddrPC.Offset = ctx.Pc;
	frame.AddrFrame.Offset = ctx.Fp;
	frame.AddrStack.Offset = ctx.Sp;
#elif defined(_M_IX86)
	machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset = ctx.Eip;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrStack.Offset = ctx.Esp;
#else
#error "unsupported architecture"
#endif
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;

	/* Function tables come from the game's modules; symbols are never loaded. */
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
	bool sym = SymInitialize(game, NULL, TRUE);
	HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, crash->tid);

	/* The first step reports the starting frame itself; later ones return addresses. */
	for (int depth = 0; depth < CW_MAX_DEPTH; ++depth) {
		if (!StackWalk64(machine, game, thread, &frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
			break;
		}
		uint64_t pc = frame.AddrPC.Offset;
		if (pc == 0) {
			break;
		}
		add_frame(out, depth == 0 ? pc : pc - 1);
	}

	if (thread != NULL) {
		CloseHandle(thread);
	}
	if (sym) {
		SymCleanup(game);
	}
	return out->frame_count > 0;
}
