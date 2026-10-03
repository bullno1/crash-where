import { describe, expect, it } from "vitest";
import { parseHeader } from "../src/cwsym";
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
