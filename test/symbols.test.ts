import { env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { symbolKey } from "../src/releases";
import { forgetTable, forgetTables, FULL_TABLE_BUDGET, locate, NEGATIVE_LIFETIME, symbolicate } from "../src/symbols";
import { BUILD_ID_HEX, makeTable } from "./table";

const FRAME = [{ module: "game.exe", buildId: BUILD_ID_HEX, offset: 0x1010 }];
const TABLE = makeTable({ functions: [{ start: 0x1000, size: 0x100, name: "render_mesh" }] });

const name = async (app: string) => (await symbolicate(env.BUCKET, app, FRAME))[0]!.name;

beforeEach(forgetTables);
afterEach(() => vi.restoreAllMocks());

describe("symbol cache", () => {
	it("remembers a build without a table, then looks again once the memory is old", async () => {
		const start = Date.now();
		const clock = vi.spyOn(Date, "now").mockReturnValue(start);
		expect(await name("cache-age")).toBeNull();
		await env.BUCKET.put(symbolKey("cache-age", BUILD_ID_HEX), TABLE);
		expect(await name("cache-age")).toBeNull();
		clock.mockReturnValue(start + NEGATIVE_LIFETIME - 1);
		expect(await name("cache-age")).toBeNull();
		clock.mockReturnValue(start + NEGATIVE_LIFETIME);
		expect(await name("cache-age")).toBe("render_mesh");
	});
	it("keeps a table it found, whatever the clock says", async () => {
		await env.BUCKET.put(symbolKey("cache-keep", BUILD_ID_HEX), TABLE);
		expect(await name("cache-keep")).toBe("render_mesh");
		await env.BUCKET.delete(symbolKey("cache-keep", BUILD_ID_HEX));
		vi.spyOn(Date, "now").mockReturnValue(Date.now() + 10 * NEGATIVE_LIFETIME);
		expect(await name("cache-keep")).toBe("render_mesh");
	});
	it("forgets one build on request and leaves the others", async () => {
		expect(await name("cache-forget")).toBeNull();
		expect(await name("cache-other")).toBeNull();
		await env.BUCKET.put(symbolKey("cache-forget", BUILD_ID_HEX), TABLE);
		await env.BUCKET.put(symbolKey("cache-other", BUILD_ID_HEX), TABLE);
		forgetTable("cache-forget", BUILD_ID_HEX);
		expect(await name("cache-forget")).toBe("render_mesh");
		expect(await name("cache-other")).toBeNull();
	});
});

const LINED = makeTable({
	functions: [{ start: 0x1000, size: 0x100, name: "render_mesh" }],
	displays: ["render_mesh(mesh*)"],
	lines: [{ start: 0x1000, size: 0x100, file: "src/render.c", line: 40 }],
});

describe("locate", () => {
	it("resolves frames from the complete table, a return address one byte back", async () => {
		await env.BUCKET.put(symbolKey("locate-lines", BUILD_ID_HEX), makeTable({
			functions: [{ start: 0x1000, size: 0x10, name: "a" }, { start: 0x1010, size: 0x10, name: "b" }],
			lines: [{ start: 0x1000, size: 0x10, file: "a.c", line: 1 }, { start: 0x1010, size: 0x10, file: "b.c", line: 2 }],
		}));
		const frames = [
			{ module: "game.exe", buildId: BUILD_ID_HEX, offset: 0x1010 },
			{ module: "game.exe", buildId: BUILD_ID_HEX, offset: 0x1010 },
			{ module: null, buildId: null, offset: 0x1010 },
			{ module: "game.exe", buildId: "ff", offset: 0x1010 },
		];
		expect(await locate(env.BUCKET, "locate-lines", frames)).toEqual([
			[{ function: "b", file: "b.c", line: 2 }],
			[{ function: "a", file: "a.c", line: 1 }],
			[],
			[],
		]);
	});
	it("reads the complete table once and answers from memory after", async () => {
		await env.BUCKET.put(symbolKey("locate-cache", BUILD_ID_HEX), LINED);
		expect(await locate(env.BUCKET, "locate-cache", FRAME)).toEqual([[{ function: "render_mesh(mesh*)", file: "src/render.c", line: 40 }]]);
		await env.BUCKET.delete(symbolKey("locate-cache", BUILD_ID_HEX));
		expect(await locate(env.BUCKET, "locate-cache", FRAME)).toEqual([[{ function: "render_mesh(mesh*)", file: "src/render.c", line: 40 }]]);
	});
	it("leaves a table over the budget unread", async () => {
		await env.BUCKET.put(symbolKey("locate-big", BUILD_ID_HEX), LINED);
		const head = env.BUCKET.head.bind(env.BUCKET);
		vi.spyOn(env.BUCKET, "head").mockImplementation(async (key: string) => {
			const o = await head(key);
			return o === null ? null : Object.assign(Object.create(Object.getPrototypeOf(o) as object) as R2Object, o, { size: FULL_TABLE_BUDGET + 1 });
		});
		expect(await locate(env.BUCKET, "locate-big", FRAME)).toEqual([[]]);
		expect(await name("locate-big")).toBe("render_mesh");
	});
});
