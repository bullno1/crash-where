import { env as bindings, runInDurableObject } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { createDb } from "../src/db";
import worker from "../src/index";
import { symbolKey } from "../src/releases";
import { type AppShard, SUPPORT_WINDOW } from "../src/shard";
import { symbolicate } from "../src/symbols";
import { authenticateToken, createToken, revokeToken } from "../src/tokens";
import { BUILD_ID_HEX, makeTable } from "./table";

const env = { DB: bindings.DB, SHARD: bindings.SHARD, BUCKET: bindings.BUCKET };
const db = createDb(bindings.DB);
const who = { sub: "ci", email: "ci@example.com" };

async function addApp(name: string, disabled: number | null = null): Promise<number> {
	const row = await bindings.DB.prepare(
		"INSERT INTO apps (name, display_name, created_at, created_by, disabled_at) VALUES (?1, ?1, 1, 'bob', ?2) RETURNING id"
	)
		.bind(name, disabled)
		.first<{ id: number }>();
	return row!.id;
}

/** An app with a usable token; every test gets its own shard this way. */
async function appWithToken(name: string, disabled: number | null = null): Promise<string> {
	const id = await addApp(name, disabled);
	return (await createToken(db, id, "ci", who, 1)).token;
}

interface Upload {
	app: string;
	version?: string;
	channel?: string;
	token?: string;
	body?: BodyInit | null;
	commit?: string;
	source_root?: string;
}

async function put(u: Upload): Promise<Response> {
	const path = `/v1/${u.app}/releases/${encodeURIComponent(u.version ?? "1.0.0")}`;
	let query = u.channel === undefined ? "?channel=stable" : u.channel === "" ? "" : `?channel=${u.channel}`;
	for (const key of ["commit", "source_root"] as const) {
		if (u[key] !== undefined) query += `${query === "" ? "?" : "&"}${key}=${encodeURIComponent(u[key])}`;
	}
	const headers: Record<string, string> = { "Content-Type": "application/octet-stream" };
	if (u.token !== undefined) headers.Authorization = `Bearer ${u.token}`;
	return worker.fetch(
		new Request(`https://api.example${path}${query}`, { method: "PUT", headers, body: u.body === undefined ? makeTable() : u.body }),
		env
	);
}

function inShard<T>(name: string, fn: (obj: AppShard) => T | Promise<T>): Promise<T> {
	const ns = bindings.SHARD;
	return runInDurableObject(ns.get(ns.idFromName(name)), (obj) => fn(obj as AppShard));
}

async function rows(name: string) {
	return inShard(name, async (obj) => ({
		versions: await obj.db.selectFrom("versions").select("version").orderBy("version").execute(),
		builds: await obj.db.selectFrom("builds").select(["build_id", "version", "source_commit", "source_root"]).orderBy("build_id").execute(),
		releases: await obj.db
			.selectFrom("releases")
			.select(["channel", "version", "supported_until"])
			.orderBy("channel")
			.orderBy("version")
			.execute(),
	}));
}

beforeEach(async () => {
	await bindings.DB.exec("DELETE FROM upload_tokens");
	await bindings.DB.exec("DELETE FROM apps");
});

describe("upload authorization", () => {
	it("needs a bearer token", async () => {
		await appWithToken("auth-none");
		expect((await put({ app: "auth-none" })).status).toBe(401);
		expect((await put({ app: "auth-none", token: "" })).status).toBe(401);
	});
	it("refuses an unknown token", async () => {
		await appWithToken("auth-bad");
		expect((await put({ app: "auth-bad", token: "cwu_nope" })).status).toBe(401);
	});
	it("refuses a revoked token", async () => {
		const token = await appWithToken("auth-revoked");
		const { id, app_id } = (await authenticateToken(db, token, 2))!;
		expect(await revokeToken(db, app_id, id)).toBe(true);
		expect((await put({ app: "auth-revoked", token })).status).toBe(401);
	});
	it("refuses a token minted for another app", async () => {
		const token = await appWithToken("auth-other");
		await addApp("auth-mine");
		const r = await put({ app: "auth-mine", token });
		expect(r.status).toBe(403);
		expect(await r.text()).toMatch(/another app/);
	});
	it("refuses a disabled app", async () => {
		const token = await appWithToken("auth-disabled", 2);
		const r = await put({ app: "auth-disabled", token });
		expect(r.status).toBe(403);
		expect(await r.text()).toMatch(/disabled/);
		expect(await bindings.BUCKET.head(symbolKey("auth-disabled", BUILD_ID_HEX))).toBeNull();
	});
	it("is 404 for an unknown app", async () => {
		const token = await appWithToken("auth-known");
		expect((await put({ app: "auth-unknown", token })).status).toBe(404);
	});
	it("records when the token was last used", async () => {
		const token = await appWithToken("auth-used");
		expect((await put({ app: "auth-used", token })).status).toBe(201);
		const row = await bindings.DB.prepare("SELECT last_used_at FROM upload_tokens").first<{ last_used_at: number }>();
		expect(row!.last_used_at).toBeGreaterThan(1_700_000_000);
	});
});

describe("upload validation", () => {
	it("refuses a version with control characters or over 100 characters", async () => {
		const token = await appWithToken("val-version");
		for (const version of ["1.0\n0", "1.0.0\t", "9".repeat(101)]) {
			const r = await put({ app: "val-version", token, version });
			expect(r.status, JSON.stringify(version)).toBe(400);
			expect(await r.text()).toMatch(/version/);
		}
	});
	it("imposes no version scheme", async () => {
		const token = await appWithToken("val-scheme");
		const versions = ["1.5.0-nightly.20261004", "v1.0.0", "2026-10-04 beta", "deadbeef"];
		for (const [i, version] of versions.entries()) {
			const body = makeTable({ buildId: Uint8Array.from({ length: 20 }, () => i + 1) });
			expect((await put({ app: "val-scheme", token, version, body })).status, version).toBe(201);
		}
		expect((await rows("val-scheme")).versions.map((v) => v.version).sort()).toEqual([...versions].sort());
	});
	it("refuses a missing or odd channel", async () => {
		const token = await appWithToken("val-channel");
		expect((await put({ app: "val-channel", token, channel: "" })).status).toBe(400);
		expect((await put({ app: "val-channel", token, channel: "a%20b" })).status).toBe(400);
	});
	it("refuses an odd commit or source root", async () => {
		const token = await appWithToken("val-source");
		expect((await put({ app: "val-source", token, commit: "" })).status).toBe(400);
		expect((await put({ app: "val-source", token, commit: "a b" })).status).toBe(400);
		expect((await put({ app: "val-source", token, commit: "x".repeat(101) })).status).toBe(400);
		expect((await put({ app: "val-source", token, source_root: "" })).status).toBe(400);
		expect((await put({ app: "val-source", token, source_root: "/a\nb" })).status).toBe(400);
		expect((await put({ app: "val-source", token, source_root: "x".repeat(501) })).status).toBe(400);
		expect((await rows("val-source")).builds).toEqual([]);
	});
	it("refuses bytes that are not a complete table", async () => {
		const token = await appWithToken("val-table");
		const foreign = await put({ app: "val-table", token, body: new Uint8Array([1, 2, 3]) });
		expect(foreign.status).toBe(400);
		expect(await foreign.text()).toBe("not a cwsym table");
		const prefix = await put({ app: "val-table", token, body: makeTable({ complete: false }) });
		expect(prefix.status).toBe(400);
		expect(await prefix.text()).toMatch(/prefix/);
		expect(await bindings.BUCKET.head(symbolKey("val-table", BUILD_ID_HEX))).toBeNull();
		expect((await rows("val-table")).builds).toEqual([]);
	});
});

describe("upload", () => {
	it("stores the table and registers version, build and release", async () => {
		const token = await appWithToken("up-first");
		const table = makeTable();
		const r = await put({ app: "up-first", token, body: table });
		expect(r.status).toBe(201);
		expect(await r.text()).toBe(`build ${BUILD_ID_HEX}\ncreated table,version,build,release\n`);
		const object = await bindings.BUCKET.get(symbolKey("up-first", BUILD_ID_HEX));
		expect(new Uint8Array(await object!.arrayBuffer())).toEqual(table);
		expect(await rows("up-first")).toEqual({
			versions: [{ version: "1.0.0" }],
			builds: [{ build_id: BUILD_ID_HEX, version: "1.0.0", source_commit: null, source_root: null }],
			releases: [{ channel: "stable", version: "1.0.0", supported_until: null }],
		});
	});
	it("records the commit and source root when given", async () => {
		const token = await appWithToken("up-source");
		const r = await put({ app: "up-source", token, commit: "release/1.0", source_root: "D:\\a\\game\\game" });
		expect(r.status).toBe(201);
		expect((await rows("up-source")).builds).toEqual([
			{ build_id: BUILD_ID_HEX, version: "1.0.0", source_commit: "release/1.0", source_root: "D:\\a\\game\\game" },
		]);
	});
	it("fills in a commit or root a rerun adds, and refuses one that differs", async () => {
		const token = await appWithToken("up-source-again");
		expect((await put({ app: "up-source-again", token })).status).toBe(201);
		expect((await put({ app: "up-source-again", token, commit: "abc" })).status).toBe(200);
		expect((await put({ app: "up-source-again", token, source_root: "/home/ci/game" })).status).toBe(200);
		expect((await put({ app: "up-source-again", token })).status).toBe(200);
		expect((await put({ app: "up-source-again", token, commit: "abc", source_root: "/home/ci/game" })).status).toBe(200);
		const other = await put({ app: "up-source-again", token, commit: "def" });
		expect(other.status).toBe(409);
		expect(await other.text()).toMatch(/with commit abc$/);
		const elsewhere = await put({ app: "up-source-again", token, source_root: "/tmp/game" });
		expect(elsewhere.status).toBe(409);
		expect(await elsewhere.text()).toMatch(/with source root \/home\/ci\/game$/);
		expect((await rows("up-source-again")).builds).toEqual([
			{ build_id: BUILD_ID_HEX, version: "1.0.0", source_commit: "abc", source_root: "/home/ci/game" },
		]);
	});
	it("makes the table known at once where the upload ran", async () => {
		const token = await appWithToken("up-known");
		const frame = [{ module: "game.exe", buildId: BUILD_ID_HEX, offset: 0x1010 }];
		expect((await symbolicate(bindings.BUCKET, "up-known", frame))[0]!.name).toBeNull();
		const body = makeTable({ functions: [{ start: 0x1000, size: 0x100, name: "render_mesh" }] });
		expect((await put({ app: "up-known", token, body })).status).toBe(201);
		expect((await symbolicate(bindings.BUCKET, "up-known", frame))[0]!.name).toBe("render_mesh");
	});
	it("is a no-op when run again", async () => {
		const token = await appWithToken("up-again");
		expect((await put({ app: "up-again", token })).status).toBe(201);
		const r = await put({ app: "up-again", token });
		expect(r.status).toBe(200);
		expect(await r.text()).toBe(`build ${BUILD_ID_HEX}\ncreated none\n`);
		expect((await rows("up-again")).builds).toHaveLength(1);
	});
	it("adds a second platform's build to the same release", async () => {
		const token = await appWithToken("up-platform");
		expect((await put({ app: "up-platform", token })).status).toBe(201);
		const linux = Uint8Array.from({ length: 20 }, () => 0xcc);
		const r = await put({ app: "up-platform", token, body: makeTable({ buildId: linux }) });
		expect(r.status).toBe(201);
		expect(await r.text()).toBe(`build ${"cc".repeat(20)}\ncreated table,build\n`);
		const { builds, releases } = await rows("up-platform");
		expect(builds).toHaveLength(2);
		expect(releases).toHaveLength(1);
	});
	it("releases an uploaded version on a second channel", async () => {
		const token = await appWithToken("up-channel");
		expect((await put({ app: "up-channel", token, channel: "beta" })).status).toBe(201);
		const r = await put({ app: "up-channel", token, channel: "stable" });
		expect(r.status).toBe(201);
		expect(await r.text()).toBe(`build ${BUILD_ID_HEX}\ncreated release\n`);
		expect((await rows("up-channel")).releases.map((x) => x.channel)).toEqual(["beta", "stable"]);
	});
	it("starts the window of the channel's previous release", async () => {
		const token = await appWithToken("up-window");
		expect((await put({ app: "up-window", token, version: "1.0.0", channel: "stable" })).status).toBe(201);
		expect((await put({ app: "up-window", token, version: "1.0.0", channel: "beta" })).status).toBe(201);
		const next = makeTable({ buildId: Uint8Array.from({ length: 20 }, () => 0xdd) });
		const r = await put({ app: "up-window", token, version: "1.1.0", channel: "stable", body: next });
		expect(r.status).toBe(201);
		expect(await r.text()).toBe(`build ${"dd".repeat(20)}\ncreated table,version,build,release\n`);
		const { releases } = await rows("up-window");
		const old = releases.find((x) => x.channel === "stable" && x.version === "1.0.0")!;
		const now = Math.floor(Date.now() / 1000);
		expect(old.supported_until).toBeGreaterThanOrEqual(now + SUPPORT_WINDOW - 5);
		expect(old.supported_until).toBeLessThanOrEqual(now + SUPPORT_WINDOW);
		expect(releases.find((x) => x.channel === "beta")!.supported_until).toBeNull();
		expect(releases.find((x) => x.version === "1.1.0")!.supported_until).toBeNull();
	});
	it("refuses a build id already registered under another version", async () => {
		const token = await appWithToken("up-conflict");
		expect((await put({ app: "up-conflict", token, version: "1.0.0" })).status).toBe(201);
		const r = await put({ app: "up-conflict", token, version: "1.0.1" });
		expect(r.status).toBe(409);
		expect(await r.text()).toMatch(/under version 1\.0\.0/);
		expect((await rows("up-conflict")).versions).toEqual([{ version: "1.0.0" }]);
	});
	it("refuses different bytes for a stored build id", async () => {
		const token = await appWithToken("up-bytes");
		const first = makeTable();
		expect((await put({ app: "up-bytes", token, body: first })).status).toBe(201);
		const r = await put({ app: "up-bytes", token, body: makeTable({ display: "main(int, char**)" }) });
		expect(r.status).toBe(409);
		expect(await r.text()).toMatch(/different table/);
		const object = await bindings.BUCKET.get(symbolKey("up-bytes", BUILD_ID_HEX));
		expect(new Uint8Array(await object!.arrayBuffer())).toEqual(first);
	});
	it("completes a rerun after the rows were not written", async () => {
		const token = await appWithToken("up-orphan");
		await bindings.BUCKET.put(symbolKey("up-orphan", BUILD_ID_HEX), makeTable());
		const r = await put({ app: "up-orphan", token });
		expect(r.status).toBe(201);
		expect(await r.text()).toBe(`build ${BUILD_ID_HEX}\ncreated version,build,release\n`);
	});
});
