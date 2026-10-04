import { env } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { symbolKey } from "../src/releases";
import { forgetTable, forgetTables, NEGATIVE_LIFETIME, symbolicate } from "../src/symbols";
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
