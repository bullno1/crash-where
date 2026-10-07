import { describe, expect, it } from "vitest";
import { CwsymTable, parseHeader } from "../src/cwsym";
import { BUILD_ID_HEX, makeTable } from "./table";

function reason(bytes: Uint8Array): string {
	const r = parseHeader(bytes);
	expect(r.ok).toBe(false);
	return r.ok ? "" : r.reason;
}

describe("cwsym header", () => {
	it("reads the identity of a complete table", () => {
		const r = parseHeader(makeTable({ rules: 3, arch: 2, count: 2 }));
		expect(r).toEqual({ ok: true, header: { rules: 3, arch: 2, buildId: BUILD_ID_HEX, count: 2, flags: 0 } });
	});
	it("prints a shorter build id with its own length", () => {
		const r = parseHeader(makeTable({ buildId: Uint8Array.from({ length: 16 }, () => 0xab) }));
		expect(r.ok && r.header.buildId).toBe("ab".repeat(16));
	});
	it("rejects foreign bytes and short buffers", () => {
		expect(reason(makeTable({ magic: "ELF\0\0\0" }))).toBe("not a cwsym table");
		expect(reason(makeTable().subarray(0, 100))).toBe("not a cwsym table");
		expect(reason(new Uint8Array(0))).toBe("not a cwsym table");
	});
	it("rejects another layout version", () => {
		expect(reason(makeTable({ version: 2 }))).toMatch(/layout version 2/);
	});
	it("rejects a bad architecture, build id length or count", () => {
		expect(reason(makeTable({ arch: 9 }))).toMatch(/^bad header/);
		expect(reason(makeTable({ buildId: new Uint8Array(0) }))).toMatch(/^bad header/);
		expect(reason(makeTable({ count: 0 }))).toMatch(/^bad header/);
	});
	it("rejects a section outside the file", () => {
		const bytes = makeTable();
		new DataView(bytes.buffer).setUint32(44, bytes.length, true);
		expect(reason(bytes)).toMatch(/outside the file/);
	});
	it("rejects a prefix table", () => {
		expect(reason(makeTable({ complete: false }))).toMatch(/prefix/);
	});
});

/**
 * `f` at 0x1000, with `g` inlined over [0x1020, 0x1050) and `h` inlined
 * into `g` over [0x1030, 0x1040); line rows cover the function in three
 * pieces.
 */
const LINED = makeTable({
	functions: [{ start: 0x1000, size: 0x100, name: "f" }, { start: 0x2000, size: 0x10, name: "k" }],
	displays: ["f(int)"],
	lines: [
		{ start: 0x1000, size: 0x20, file: "src/a.c", line: 10 },
		{ start: 0x1020, size: 0x10, file: "src/a.c", line: 12 },
		{ start: 0x1030, size: 0x10, file: "inc/b.h", line: 5 },
	],
	sites: [
		{ start: 0x1020, size: 0x30, callee: "g", file: "src/a.c", line: 11, parent: null },
		{ start: 0x1030, size: 0x10, callee: "h", file: "inc/b.h", line: 7, parent: 0 },
	],
});

describe("cwsym symbolize", () => {
	it("resolves a plain offset to its display name, file and line", () => {
		const t = CwsymTable.parse(LINED)!;
		expect(t.complete).toBe(true);
		expect(t.symbolize(0x1005)).toEqual([{ function: "f(int)", file: "src/a.c", line: 10 }]);
		expect(t.lookup(0x1005)).toBe("f");
	});
	it("walks the inline chain innermost first, each parent adding its call site", () => {
		const t = CwsymTable.parse(LINED)!;
		expect(t.symbolize(0x1035)).toEqual([
			{ function: "h", file: "inc/b.h", line: 5 },
			{ function: "g", file: "inc/b.h", line: 7 },
			{ function: "f(int)", file: "src/a.c", line: 11 },
		]);
		expect(t.symbolize(0x1025)).toEqual([
			{ function: "g", file: "src/a.c", line: 12 },
			{ function: "f(int)", file: "src/a.c", line: 11 },
		]);
	});
	it("gives the function without a file past the line rows, the normalized name without a display one", () => {
		const t = CwsymTable.parse(LINED)!;
		expect(t.symbolize(0x1080)).toEqual([{ function: "f(int)", file: null, line: 0 }]);
		expect(t.symbolize(0x2004)).toEqual([{ function: "k", file: null, line: 0 }]);
	});
	it("gives nothing for an offset no function covers", () => {
		expect(CwsymTable.parse(LINED)!.symbolize(0x1100)).toEqual([]);
		expect(CwsymTable.parse(LINED)!.symbolize(0x0)).toEqual([]);
	});
	it("names only from a table without lines, or from a prefix", () => {
		const plain = CwsymTable.parse(makeTable({ functions: [{ start: 0x1000, size: 0x100, name: "f" }], displays: ["f(int)"] }))!;
		expect(plain.symbolize(0x1005)).toEqual([{ function: "f(int)", file: null, line: 0 }]);
		const prefix = CwsymTable.parse(makeTable({ functions: [{ start: 0x1000, size: 0x100, name: "f" }], complete: false }))!;
		expect(prefix.complete).toBe(false);
		expect(prefix.symbolize(0x1005)).toEqual([{ function: "f", file: null, line: 0 }]);
	});
	it("drops line sections that are unsound and keeps the names", () => {
		const bad = makeTable({
			functions: [{ start: 0x1000, size: 0x100, name: "f" }],
			lines: [{ start: 0x1000, size: 0x20, file: "a.c", line: 1 }],
			// A site outside its function.
			sites: [{ start: 0x2000, size: 0x10, callee: "g", file: null, line: 0, parent: null }],
		});
		const t = CwsymTable.parse(bad)!;
		expect(t.symbolize(0x1005)).toEqual([{ function: "f", file: null, line: 0 }]);
	});
});
