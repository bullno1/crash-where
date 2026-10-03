import { hex } from "./bytes";
import type { RawFrame } from "./symbols";

/**
 * Version of everything this module decides: the fault classes, the frame
 * count, the built-in skip list and which faults hash their message. A
 * change bumps it and remaps the stored groups.
 */
export const GROUPING = 1;

/** Frames that enter the hash, after the skip list. */
export const HASHED_FRAMES = 4;

/** Raw frames stored on a group, before the skip list, so a rule change can rehash it. */
export const STORED_FRAMES = 16;

/** Longest message hashed and stored. */
export const MAX_MESSAGE = 512;

/** A fault class the Worker recognizes, and whether its message tells groups apart. */
export interface Fault {
	fault: string;
	withMessage: boolean;
}

/** Platform exception names mapped to one class per kind of fault. */
const FAULTS: Record<string, string> = {
	SIGSEGV: "memory",
	SIGBUS: "memory",
	EXCEPTION_ACCESS_VIOLATION: "memory",
	EXCEPTION_IN_PAGE_ERROR: "memory",
	EXCEPTION_DATATYPE_MISALIGNMENT: "memory",
	EXCEPTION_ARRAY_BOUNDS_EXCEEDED: "memory",
	SIGFPE: "arithmetic",
	SIGILL: "illegal",
	EXCEPTION_ILLEGAL_INSTRUCTION: "illegal",
	EXCEPTION_PRIV_INSTRUCTION: "illegal",
	EXCEPTION_STACK_OVERFLOW: "stack_overflow",
	SIGTRAP: "trap",
	EXCEPTION_BREAKPOINT: "trap",
	SIGABRT: "abort",
	abort: "abort",
	SIGNAL: "other",
	EXCEPTION_NONCONTINUABLE_EXCEPTION: "other",
	HANG: "hang",
	KILLED: "exit",
	ERROR: "error",
};

/** Longest type kept when the game named it. */
const MAX_TYPE = 64;

/**
 * The class of an exception type. A platform name maps to its class and
 * only `abort` and `error` hash the message. Any other type was chosen
 * by the game in `cw_abort` or `cw_report`, so it is a class of its own,
 * the same on every platform, and its message is the game's too.
 */
export function classify(type: string): Fault {
	const known = FAULTS[type] ?? (/^EXCEPTION_(INT|FLT)_/.test(type) ? "arithmetic" : undefined);
	if (known !== undefined) return { fault: known, withMessage: known === "abort" || known === "error" };
	return { fault: type.slice(0, MAX_TYPE), withMessage: true };
}

/** Frames whose module alone, or module and function, match are noise. */
export type SkipList = { module: RegExp; name: RegExp | null }[];

/**
 * The frames every game shares as noise: the operating system and the
 * language runtimes, the allocator, the standard library and the crash
 * library's own handler.
 */
export const DEFAULT_SKIP_LIST = `
# Windows
ntdll.dll
kernel32.dll
kernelbase.dll
user32.dll
ucrtbase*.dll
vcruntime*.dll
msvcp*.dll
msvcrt.dll
# Linux
libc.so*
libc-*.so*
libpthread*.so*
libm.so*
libm-*.so*
libdl*.so*
librt*.so*
ld-linux*.so*
libgcc_s.so*
libstdc++.so*
libc++.so*
libc++abi.so*
# macOS
libsystem_*.dylib
libdyld.dylib
libobjc.*.dylib
libc++.*.dylib
libc++abi.dylib
# the crash library, the C++ standard library and the allocator in any module
*!cw_*
*!std::*
*!__cxa_*
*!operator new*
*!operator delete*
*!malloc
*!calloc
*!realloc
*!free
`;

function glob(pattern: string, flags: string): RegExp {
	const escaped = pattern.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".");
	return new RegExp(`^${escaped}$`, flags);
}

/**
 * Compiles skip list text: one pattern per line, `<module glob>!<function
 * glob>` with the function part optional, `#` for comments. Module names
 * match without case, as Windows spells them either way.
 */
export function compileSkipList(text: string): SkipList {
	const list: SkipList = [];
	for (const raw of text.split("\n")) {
		const line = raw.trim();
		if (line === "" || line.startsWith("#")) continue;
		const bang = line.indexOf("!");
		if (bang < 0) list.push({ module: glob(line, "i"), name: null });
		else list.push({ module: glob(line.slice(0, bang), "i"), name: glob(line.slice(bang + 1), "") });
	}
	return list;
}

const UNKNOWN = "<unknown>";

function skipped(f: RawFrame, skip: SkipList): boolean {
	return skip.some((p) => p.module.test(f.module) && (p.name === null || p.name.test(f.name ?? UNKNOWN)));
}

/**
 * The tokens the hash takes: the names of the first frames that survive
 * the skip list, an unnamed frame contributing its module instead. When
 * every frame is noise the stack is all runtime or driver, and the module
 * names of the first raw frames stand in.
 */
export function selectFrames(frames: RawFrame[], skip: SkipList): string[] {
	const kept: string[] = [];
	for (const f of frames) {
		if (kept.length === HASHED_FRAMES) break;
		if (skipped(f, skip)) continue;
		kept.push(f.name ?? `${f.module}!${UNKNOWN}`);
	}
	if (kept.length > 0 || frames.length === 0) return kept;
	return frames.slice(0, HASHED_FRAMES).map((f) => f.module);
}

/** The group hash: hex16 of the fault, the selected frames and the message. */
export async function fingerprint(fault: string, tokens: string[], message: string | null): Promise<string> {
	const input = `g${GROUPING}\n${fault}\n${tokens.join("\n")}\n${message ?? ""}`;
	const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(input));
	return hex(new Uint8Array(digest)).slice(0, 16);
}
