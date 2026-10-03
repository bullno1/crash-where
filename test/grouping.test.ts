import { describe, expect, it } from "vitest";
import { classify, compileSkipList, DEFAULT_SKIP_LIST, fingerprint, selectFrames } from "../src/grouping";
import type { RawFrame } from "../src/symbols";

const frame = (module: string, name: string | null): RawFrame => ({ module, name });
const skip = compileSkipList(DEFAULT_SKIP_LIST);

describe("fault classes", () => {
	it("merge the platform names of one kind of fault", () => {
		expect(classify("SIGSEGV")).toEqual({ fault: "memory", withMessage: false });
		expect(classify("EXCEPTION_ACCESS_VIOLATION")).toEqual({ fault: "memory", withMessage: false });
		expect(classify("SIGFPE").fault).toBe("arithmetic");
		expect(classify("EXCEPTION_FLT_DIVIDE_BY_ZERO").fault).toBe("arithmetic");
		expect(classify("EXCEPTION_INT_OVERFLOW").fault).toBe("arithmetic");
		expect(classify("EXCEPTION_STACK_OVERFLOW").fault).toBe("stack_overflow");
	});
	it("hash the message for aborts and reported errors only", () => {
		expect(classify("SIGABRT")).toEqual({ fault: "abort", withMessage: true });
		expect(classify("ERROR")).toEqual({ fault: "error", withMessage: true });
		expect(classify("HANG")).toEqual({ fault: "hang", withMessage: false });
		expect(classify("KILLED")).toEqual({ fault: "exit", withMessage: false });
	});
	it("keep a type the game named, with its message", () => {
		expect(classify("ASSERT")).toEqual({ fault: "ASSERT", withMessage: true });
		expect(classify("x".repeat(100)).fault).toHaveLength(64);
	});
});

describe("skip list", () => {
	it("matches a module without case and a function exactly", () => {
		const list = compileSkipList("NTDLL.dll\n*!std::*\n# a comment\n\ngame.exe!lua_*\n");
		expect(selectFrames([frame("ntdll.dll", "RtlUserThreadStart")], list)).toEqual(["ntdll.dll"]);
		expect(selectFrames([frame("game.exe", "std::sort"), frame("game.exe", "tick")], list)).toEqual(["tick"]);
		expect(selectFrames([frame("game.exe", "Std::sort")], list)).toEqual(["Std::sort"]);
		expect(selectFrames([frame("game.exe", "lua_pcall"), frame("other.exe", "lua_pcall")], list)).toEqual(["lua_pcall"]);
	});
	it("treats a glob's regex characters as text", () => {
		const list = compileSkipList("libc++.so*\n*!operator new*");
		expect(selectFrames([frame("libc++.so.1", "x"), frame("libcxx.so.1", "y")], list)).toEqual(["y"]);
		expect(selectFrames([frame("game.exe", "operator new[]"), frame("game.exe", "z")], list)).toEqual(["z"]);
	});
});

describe("frame selection", () => {
	it("drops noise wherever it sits and keeps four frames", () => {
		const frames = [
			frame("libc.so.6", "memcpy"),
			frame("game.exe", "copy_mesh"),
			frame("game.exe", "std::sort"),
			frame("game.exe", "load_level"),
			frame("game.exe", "cw_handler"),
			frame("game.exe", "tick"),
			frame("game.exe", "run"),
			frame("game.exe", "main"),
		];
		expect(selectFrames(frames, skip)).toEqual(["copy_mesh", "load_level", "tick", "run"]);
	});
	it("names an unknown frame by its module", () => {
		expect(selectFrames([frame("nvoglv64.dll", null), frame("game.exe", "render")], skip))
			.toEqual(["nvoglv64.dll!<unknown>", "render"]);
	});
	it("falls back to module names when every frame is noise", () => {
		const frames = [frame("ntdll.dll", "a"), frame("kernelbase.dll", "b"), frame("ntdll.dll", null)];
		expect(selectFrames(frames, skip)).toEqual(["ntdll.dll", "kernelbase.dll", "ntdll.dll"]);
		expect(selectFrames([], skip)).toEqual([]);
	});
});

describe("fingerprint", () => {
	it("is sixteen hex digits that depend on every input", async () => {
		const base = await fingerprint("memory", ["a", "b"], null);
		expect(base).toMatch(/^[0-9a-f]{16}$/);
		expect(await fingerprint("memory", ["a", "b"], null)).toBe(base);
		expect(await fingerprint("abort", ["a", "b"], null)).not.toBe(base);
		expect(await fingerprint("memory", ["a"], null)).not.toBe(base);
		expect(await fingerprint("memory", ["a", "b"], "m")).not.toBe(base);
	});
});
