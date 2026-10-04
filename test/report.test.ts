import { env as bindings, runInDurableObject } from "cloudflare:test";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createDb } from "../src/db";
import worker from "../src/index";
import { symbolKey } from "../src/releases";
import { ENVELOPE_OBJECT, sampleKey } from "../src/samples";
import type { AppShard } from "../src/shard";
import { forgetTables } from "../src/symbols";
import { createToken } from "../src/tokens";
import { BUILD_ID_HEX, makeTable, type SymbolRow } from "./table";

/** A second build the app has no table for until a test uploads one. */
const OTHER_BUILD = Uint8Array.from({ length: 20 }, (_, i) => 0x21 + i);
const OTHER_BUILD_HEX = Array.from(OTHER_BUILD, (b) => b.toString(16).padStart(2, "0")).join("");

const env = { DB: bindings.DB, SHARD: bindings.SHARD, BUCKET: bindings.BUCKET };

/** The game's functions; frames point into them by offset. */
const FUNCTIONS: SymbolRow[] = [
	{ start: 0x1000, size: 0x100, name: "render_mesh" },
	{ start: 0x2000, size: 0x100, name: "draw_scene" },
	{ start: 0x3000, size: 0x100, name: "tick" },
	{ start: 0x4000, size: 0x100, name: "run" },
	{ start: 0x5000, size: 0x100, name: "main" },
	{ start: 0x6000, size: 0x100, name: "std::sort" },
	{ start: 0x7000, size: 0x100, name: "cw_handle_signal" },
	{ start: 0x8000, size: 0x100, name: "load_level" },
];

async function addApp(name: string, disabled: number | null = null): Promise<number> {
	const result = await bindings.DB.prepare(
		"INSERT INTO apps (name, display_name, created_at, created_by, disabled_at) VALUES (?1, ?1, 1, 'bob', ?2)"
	)
		.bind(name, disabled)
		.run();
	return result.meta.last_row_id;
}

/** Uploads a table through the route, as the symbol tool does, with a token minted for the app. */
async function uploadTable(app: string, table: Uint8Array): Promise<Response> {
	const db = createDb(bindings.DB);
	const row = await db.selectFrom("apps").select("id").where("name", "=", app).executeTakeFirstOrThrow();
	const { token } = await createToken(db, row.id, "ci", { sub: "ci" }, 1);
	return worker.fetch(
		new Request(`https://api.example/v1/${app}/releases/1.0.0?channel=stable`, {
			method: "PUT",
			headers: { "Content-Type": "application/octet-stream", Authorization: `Bearer ${token}` },
			body: table,
		}),
		env
	);
}

function inShard<T>(name: string, fn: (obj: AppShard) => T | Promise<T>): Promise<T> {
	const ns = bindings.SHARD;
	return runInDurableObject(ns.get(ns.idFromName(name)), (obj) => fn(obj as AppShard));
}

/** An app with the game's table stored and a version released on `stable`. */
async function release(name: string, version = "1.0.0", channel = "stable"): Promise<void> {
	await addApp(name);
	await bindings.BUCKET.put(symbolKey(name, BUILD_ID_HEX), makeTable({ functions: FUNCTIONS }));
	await inShard(name, (obj) => obj.registerRelease({ version, channel, buildId: BUILD_ID_HEX, now: 1 }));
}

interface FrameSpec {
	module?: string | null;
	build_id?: string | null;
	offset: number;
}

const frame = (offset: number, module: string | null = "game.exe", build_id: string | null = BUILD_ID_HEX): FrameSpec =>
	({ module, build_id, offset });

/** The default stack: render_mesh called from draw_scene, tick and run. */
const STACK = [frame(0x1010), frame(0x2010), frame(0x3010), frame(0x4010)];

let nextId = 1;
const INSTALL = "11111111-1111-4111-8111-111111111111";

/** An envelope as the client writes one, with every field ingest reads overridable. */
function envelope(over: Record<string, unknown> = {}, frames: FrameSpec[] = STACK): Record<string, unknown> {
	return {
		schema: 2,
		report_id: `00000000-0000-4000-8000-${String(nextId++).padStart(12, "0")}`,
		install_id: INSTALL,
		sent_at: 1758100000,
		app: { name: "forest-quest", version: "1.0.0", build_id: BUILD_ID_HEX, channel: "stable" },
		env: { os: "linux" },
		exception: { type: "SIGSEGV", message_norm: "read from <ADDR>", message_raw: "read from 0x10", thread: 1 },
		modules: [{ name: "game.exe", build_id: BUILD_ID_HEX, base: "0x400000", size: 1 }],
		frames,
		breadcrumbs: [],
		state: {},
		attachments: { log_tail: false, minidump: false, snapshot: false },
		...over,
	};
}

interface PostOptions {
	gzip?: boolean;
	headers?: Record<string, string>;
	raw?: BodyInit;
}

async function gzip(bytes: Uint8Array): Promise<Uint8Array> {
	const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream("gzip"));
	return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function post(app: string, body: Record<string, unknown>, opts: PostOptions = {}): Promise<Response> {
	const headers: Record<string, string> = { "Content-Type": "application/json", ...opts.headers };
	let payload: BodyInit = opts.raw ?? JSON.stringify(body);
	if (opts.gzip) {
		payload = await gzip(new TextEncoder().encode(payload as string));
		headers["Content-Encoding"] = "gzip";
	}
	return worker.fetch(new Request(`https://api.example/v1/${app}/report`, { method: "POST", headers, body: payload }), env);
}

/** Posts an envelope for the app, naming the app in it too. */
function report(app: string, over: Record<string, unknown> = {}, frames: FrameSpec[] = STACK, opts: PostOptions = {}) {
	const body = envelope(over, frames);
	body.app = { ...(body.app as object), name: app };
	return post(app, body, opts);
}

async function state(name: string) {
	return inShard(name, async (obj) => ({
		groups: await obj.db.selectFrom("crash_groups").selectAll().orderBy("id").execute(),
		counts: await obj.db.selectFrom("crash_counts").selectAll().orderBy("group_id").orderBy("day").execute(),
		reports: await obj.db.selectFrom("reports").selectAll().orderBy("report_id").execute(),
		samples: await obj.db.selectFrom("crash_samples").selectAll().orderBy("id").execute(),
	}));
}

/** The stored envelope of a report, parsed, or null when none is stored. */
async function storedEnvelope(app: string, reportId: string): Promise<unknown> {
	const object = await bindings.BUCKET.get(sampleKey(app, reportId) + ENVELOPE_OBJECT);
	return object === null ? null : object.json();
}

/** Posts a sidecar for the report, with its length declared as the client's transports do. */
function attach(app: string, reportId: string, name: string, body: Uint8Array | ReadableStream, headers: Record<string, string> = {}) {
	const sent: Record<string, string> = { "Content-Type": "application/octet-stream" };
	if (body instanceof Uint8Array) sent["Content-Length"] = String(body.byteLength);
	return worker.fetch(
		new Request(`https://api.example/v1/${app}/attach?report=${reportId}&name=${name}`, {
			method: "POST", headers: { ...sent, ...headers }, body,
		}),
		env
	);
}

/** A sampled report for the app, returning its id. */
async function sampled(app: string): Promise<string> {
	const body = envelope();
	body.app = { ...(body.app as object), name: app };
	expect(await (await post(app, body)).text()).toBe("want_attachments 1\n");
	return body.report_id as string;
}

const bytes = (s: string) => new TextEncoder().encode(s);

async function setCap(app: string, cap: number): Promise<void> {
	await bindings.DB.prepare("UPDATE apps SET sample_cap_untrusted = ?1 WHERE name = ?2").bind(cap, app).run();
}

const today = () => Math.floor(Date.now() / 1000 / 86400);

beforeEach(async () => {
	await bindings.DB.exec("DELETE FROM upload_tokens");
	await bindings.DB.exec("DELETE FROM apps");
	forgetTables();
});
afterEach(() => vi.restoreAllMocks());

describe("report routing", () => {
	it("is 404 for an unknown app and 403 for a disabled one", async () => {
		expect((await report("rt-unknown")).status).toBe(404);
		await addApp("rt-disabled", 2);
		const r = await report("rt-disabled");
		expect(r.status).toBe(403);
		expect(await r.text()).toMatch(/disabled/);
	});
	it("refuses an envelope that is not what the client writes", async () => {
		await release("rt-shape");
		const bad = async (body: Record<string, unknown>, pattern: RegExp) => {
			const r = await report("rt-shape", body);
			expect(r.status, JSON.stringify(body)).toBe(400);
			expect(await r.text()).toMatch(pattern);
		};
		await bad({ schema: 1 }, /schema 1/);
		await bad({ report_id: "has spaces" }, /report_id/);
		await bad({ install_id: "has spaces" }, /install_id/);
		await bad({ install_id: undefined }, /install_id/);
		await bad({ exception: {} }, /exception\.type/);
		await bad({ frames: "none" }, /frames/);
		await bad({ frames: [{ module: "game.exe", build_id: BUILD_ID_HEX, offset: -1 }] }, /offset/);
		await bad({ app: { name: "rt-shape", version: "1.0.0", channel: "a b" } }, /channel/);
		const other = await post("rt-shape", envelope({ app: { name: "forest-quest", version: "1.0.0", channel: "stable" } }));
		expect(other.status).toBe(400);
		expect(await other.text()).toMatch(/another app/);
		const r = await post("rt-shape", {}, { raw: "{not json" });
		expect(r.status).toBe(400);
		expect(await r.text()).toMatch(/JSON/);
		expect((await state("rt-shape")).reports).toEqual([]);
	});
	it("refuses a crash without frames", async () => {
		await release("rt-noframes");
		const r = await report("rt-noframes", {}, []);
		expect(r.status).toBe(400);
		expect(await r.text()).toMatch(/no frames/);
	});
	it("is 410 for a version not released on the channel, writing nothing", async () => {
		await release("rt-unreleased");
		const r = await report("rt-unreleased", { app: { name: "rt-unreleased", version: "1.0.1", channel: "stable" } });
		expect(r.status).toBe(410);
		expect(await r.text()).toMatch(/not released/);
		const beta = await report("rt-unreleased", { app: { name: "rt-unreleased", version: "1.0.0", channel: "beta" } });
		expect(beta.status).toBe(410);
		expect((await state("rt-unreleased")).reports).toEqual([]);
	});
	it("is 410 for a version past its window", async () => {
		await release("rt-expired");
		await inShard("rt-expired", (obj) =>
			obj.db.updateTable("releases").set({ supported_until: 2 }).where("version", "=", "1.0.0").execute()
		);
		const r = await report("rt-expired");
		expect(r.status).toBe(410);
		expect(await r.text()).toMatch(/no longer supported/);
	});
});

describe("report body", () => {
	it("accepts a gzipped envelope", async () => {
		await release("body-gzip");
		const r = await report("body-gzip", {}, STACK, { gzip: true });
		expect(r.status).toBe(201);
		expect((await state("body-gzip")).counts).toHaveLength(1);
	});
	it("refuses bad gzip, another encoding and an oversize body", async () => {
		await release("body-bad");
		const notGzip = await post("body-bad", {}, { raw: "{}", headers: { "Content-Encoding": "gzip" } });
		expect(notGzip.status).toBe(400);
		expect(await notGzip.text()).toMatch(/gzip/);
		const br = await post("body-bad", {}, { raw: "{}", headers: { "Content-Encoding": "br" } });
		expect(br.status).toBe(415);
		const big = await post("body-bad", {}, { raw: "{}", headers: { "Content-Length": String(10 * 1024 * 1024) } });
		expect(big.status).toBe(413);
		const inflated = await post("body-bad", {}, { raw: "[" + "0,".repeat(600 * 1024) + "0]", gzip: true });
		expect(inflated.status).toBe(413);
	});
});

describe("report counting", () => {
	it("counts a first report in a new group and samples it", async () => {
		await release("count-first");
		const body = envelope();
		body.app = { ...(body.app as object), name: "count-first" };
		const r = await post("count-first", body);
		expect(r.status).toBe(201);
		expect(await r.text()).toBe("want_attachments 1\n");
		expect(await storedEnvelope("count-first", body.report_id as string)).toEqual(body);
		const { groups, counts, reports, samples } = await state("count-first");
		expect(samples).toMatchObject([{ report_id: body.report_id, version: "1.0.0", trust: 0, r2_key: `samples/count-first/${body.report_id}/` }]);
		expect(groups).toHaveLength(1);
		const g = groups[0]!;
		expect(reports).toEqual([
			{
				report_id: body.report_id, group_id: g.id, version: "1.0.0", channel: "stable",
				trust: 0, user_key: INSTALL, received_at: g.first_seen,
			},
		]);
		expect(g.fault).toBe("memory");
		expect(g.message).toBeNull();
		expect(g.fingerprint).toMatch(/^[0-9a-f]{16}$/);
		expect(JSON.parse(g.frames)).toEqual([
			{ module: "game.exe", name: "render_mesh", buildId: BUILD_ID_HEX, offset: 0x1010 },
			{ module: "game.exe", name: "draw_scene", buildId: BUILD_ID_HEX, offset: 0x2010 },
			{ module: "game.exe", name: "tick", buildId: BUILD_ID_HEX, offset: 0x3010 },
			{ module: "game.exe", name: "run", buildId: BUILD_ID_HEX, offset: 0x4010 },
		]);
		expect(g.first_seen).toBe(g.last_seen);
		expect(counts).toEqual([
			{ group_id: g.id, version: "1.0.0", channel: "stable", trust: 0, day: today(), count: 1 },
		]);
	});
	it("counts a retried report once and replies as before", async () => {
		await release("count-retry");
		const body = envelope();
		body.app = { ...(body.app as object), name: "count-retry" };
		expect((await post("count-retry", body)).status).toBe(201);
		const again = await post("count-retry", body);
		expect(again.status).toBe(200);
		expect(await again.text()).toBe("want_attachments 1\n");
		const { counts, reports, samples } = await state("count-retry");
		expect(reports).toHaveLength(1);
		expect(counts[0]!.count).toBe(1);
		expect(samples).toHaveLength(1);
	});
	it("adds a second report of the same crash to the group's count", async () => {
		await release("count-same");
		await report("count-same");
		await report("count-same");
		const { groups, counts } = await state("count-same");
		expect(groups).toHaveLength(1);
		expect(counts).toHaveLength(1);
		expect(counts[0]!.count).toBe(2);
	});
	it("records each report under its install, so one install crashing twice is one user", async () => {
		await release("count-users");
		expect((await report("count-users")).status).toBe(201);
		expect((await report("count-users")).status).toBe(201);
		expect((await report("count-users", { install_id: "22222222-2222-4222-8222-222222222222" })).status).toBe(201);
		const { groups, counts, reports } = await state("count-users");
		expect(groups).toHaveLength(1);
		expect(counts[0]!.count).toBe(3);
		expect(reports.map((r) => r.user_key).sort()).toEqual([INSTALL, INSTALL, "22222222-2222-4222-8222-222222222222"]);
		const users = await inShard("count-users", (obj) =>
			obj.db
				.selectFrom("reports")
				.select((eb) => eb.fn.count<number>("user_key").distinct().as("users"))
				.where("group_id", "=", groups[0]!.id)
				.where("trust", "=", 0)
				.executeTakeFirstOrThrow()
		);
		expect(Number(users.users)).toBe(2);
		const [listed] = await inShard("count-users", (obj) => obj.listGroups(Math.floor(Date.now() / 1000)));
		expect(listed).toMatchObject({ count: 3, recent_count: 3, recent_users: 2, urgency: 2 });
	});
	it("keeps channels and versions apart in the counts", async () => {
		await release("count-keys");
		await inShard("count-keys", (obj) => obj.registerRelease({ version: "1.0.0", channel: "beta", buildId: BUILD_ID_HEX, now: 1 }));
		await report("count-keys");
		await report("count-keys", { app: { name: "count-keys", version: "1.0.0", channel: "beta" } });
		const { groups, counts } = await state("count-keys");
		expect(groups).toHaveLength(1);
		expect(counts.map((c) => [c.channel, c.count])).toEqual([["beta", 1], ["stable", 1]]);
	});
});

describe("report sampling", () => {
	it("stores nothing for an app that keeps no samples, on the retry too", async () => {
		await release("sample-none");
		await setCap("sample-none", 0);
		const body = envelope();
		body.app = { ...(body.app as object), name: "sample-none" };
		const r = await post("sample-none", body);
		expect(r.status).toBe(201);
		expect(await r.text()).toBe("want_attachments 0\n");
		expect(await (await post("sample-none", body)).text()).toBe("want_attachments 0\n");
		expect(await storedEnvelope("sample-none", body.report_id as string)).toBeNull();
		expect((await state("sample-none")).samples).toEqual([]);
	});
	it("replaces the sample the draw points at and deletes its objects", async () => {
		await release("sample-evict");
		await setCap("sample-evict", 1);
		const first = envelope();
		const second = envelope();
		const third = envelope();
		for (const b of [first, second, third]) b.app = { ...(b.app as object), name: "sample-evict" };
		const id = (b: Record<string, unknown>) => b.report_id as string;
		await post("sample-evict", first);
		await bindings.BUCKET.put(sampleKey("sample-evict", id(first)) + "core.dmp", "dump");
		vi.spyOn(Math, "random").mockReturnValue(0);
		expect(await (await post("sample-evict", second)).text()).toBe("want_attachments 1\n");
		expect(await storedEnvelope("sample-evict", id(first))).toBeNull();
		expect(await bindings.BUCKET.get(sampleKey("sample-evict", id(first)) + "core.dmp")).toBeNull();
		expect(await storedEnvelope("sample-evict", id(second))).toEqual(second);
		vi.spyOn(Math, "random").mockReturnValue(0.9);
		expect(await (await post("sample-evict", third)).text()).toBe("want_attachments 0\n");
		expect(await storedEnvelope("sample-evict", id(second))).toEqual(second);
		expect((await state("sample-evict")).samples.map((s) => s.report_id)).toEqual([id(second)]);
	});
	it("stores a gzipped envelope inflated", async () => {
		await release("sample-gzip");
		const body = envelope();
		body.app = { ...(body.app as object), name: "sample-gzip" };
		expect((await post("sample-gzip", body, { gzip: true })).status).toBe(201);
		const object = await bindings.BUCKET.get(sampleKey("sample-gzip", body.report_id as string) + ENVELOPE_OBJECT);
		expect(object?.httpMetadata?.contentType).toBe("application/json");
		expect(await object?.json()).toEqual(body);
	});
});

describe("attachments", () => {
	it("is 404 for an unknown app and 403 for a disabled one", async () => {
		expect((await attach("at-unknown", "r", "1_c_r.dmp", bytes("x"))).status).toBe(404);
		await addApp("at-disabled", 2);
		expect((await attach("at-disabled", "r", "1_c_r.dmp", bytes("x"))).status).toBe(403);
	});
	it("refuses a name that is not the report's own sidecar", async () => {
		await release("at-name");
		const id = await sampled("at-name");
		for (const name of ["", "core.dmp", `1_c_${id}`, `1_c_${id}.exe`, `1_c_${id}.dmp.gz`, "1_c_other.dmp", `x_c_${id}.dmp`]) {
			const r = await attach("at-name", id, name, bytes("x"));
			expect(r.status, name).toBe(400);
		}
		expect((await attach("at-name", "", `1_c_${id}.dmp`, bytes("x"))).status).toBe(400);
		expect((await attach("at-name", "a b", "1_c_a b.dmp", bytes("x"))).status).toBe(400);
	});
	it("refuses a report that is not sampled, or whose sample was replaced", async () => {
		await release("at-unsampled");
		await setCap("at-unsampled", 1);
		const first = envelope();
		first.app = { ...(first.app as object), name: "at-unsampled" };
		await post("at-unsampled", first);
		const unknown = await attach("at-unsampled", "nobody", "1_c_nobody.dmp", bytes("x"));
		expect(unknown.status).toBe(404);
		expect(await unknown.text()).toMatch(/not sampled/);
		vi.spyOn(Math, "random").mockReturnValue(0);
		await sampled("at-unsampled");
		const id = first.report_id as string;
		expect((await attach("at-unsampled", id, `1_c_${id}.dmp`, bytes("x"))).status).toBe(404);
		expect(await bindings.BUCKET.head(sampleKey("at-unsampled", id) + `1_c_${id}.dmp`)).toBeNull();
	});
	it("stores a sidecar as sent, with its type and encoding, and overwrites on a retry", async () => {
		await release("at-store");
		const id = await sampled("at-store");
		const dump = await gzip(bytes("core"));
		const r = await attach("at-store", id, `1_c_${id}.dmp`, dump, { "Content-Encoding": "gzip" });
		expect(r.status).toBe(201);
		let object = await bindings.BUCKET.get(sampleKey("at-store", id) + `1_c_${id}.dmp`);
		expect(new Uint8Array(await object!.arrayBuffer())).toEqual(dump);
		expect(object!.httpMetadata).toEqual({ contentType: "application/octet-stream", contentEncoding: "gzip" });
		expect((await attach("at-store", id, `1_c_${id}.dmp`, bytes("again"))).status).toBe(201);
		object = await bindings.BUCKET.get(sampleKey("at-store", id) + `1_c_${id}.dmp`);
		expect(await object!.text()).toBe("again");
		expect(object!.httpMetadata).toEqual({ contentType: "application/octet-stream" });
		expect((await attach("at-store", id, `1_c_${id}.log`, bytes("tail"))).status).toBe(201);
		object = await bindings.BUCKET.get(sampleKey("at-store", id) + `1_c_${id}.log`);
		expect(object!.httpMetadata).toEqual({ contentType: "text/plain" });
	});
	it("refuses an undeclared, oversize or otherwise encoded body before reading it", async () => {
		await release("at-size");
		const id = await sampled("at-size");
		const name = `1_c_${id}.log`;
		const stream = new ReadableStream({ start: (c) => { c.enqueue(bytes("x")); c.close(); } });
		expect((await attach("at-size", id, name, stream)).status).toBe(411);
		expect((await attach("at-size", id, name, bytes("x"), { "Content-Length": String(1024 * 1024 + 1) })).status).toBe(413);
		expect((await attach("at-size", id, name, bytes("x"), { "Content-Encoding": "br" })).status).toBe(415);
		expect(await bindings.BUCKET.head(sampleKey("at-size", id) + name)).toBeNull();
	});
});

describe("report grouping", () => {
	it("merges the same fault under its platform names and ignores its message", async () => {
		await release("group-platform");
		await report("group-platform", { exception: { type: "SIGSEGV", message_norm: "read from <ADDR>" } });
		await report("group-platform", { exception: { type: "EXCEPTION_ACCESS_VIOLATION", message_norm: "write address <ADDR>" } });
		expect((await state("group-platform")).groups).toHaveLength(1);
	});
	it("splits aborts by message and stores it", async () => {
		await release("group-abort");
		await report("group-abort", { exception: { type: "SIGABRT", message_norm: "expected non-null texture" } });
		await report("group-abort", { exception: { type: "SIGABRT", message_norm: "expected non-null mesh" } });
		await report("group-abort", { exception: { type: "SIGABRT", message_norm: "expected non-null mesh" } });
		const { groups, counts } = await state("group-abort");
		expect(groups.map((g) => [g.fault, g.message])).toEqual([
			["abort", "expected non-null texture"], ["abort", "expected non-null mesh"],
		]);
		expect(counts.map((c) => c.count)).toEqual([1, 2]);
	});
	it("keeps a type the game named as its own fault", async () => {
		await release("group-named");
		await report("group-named", { exception: { type: "ASSERT", message_norm: "i < n" } });
		expect((await state("group-named")).groups[0]).toMatchObject({ fault: "ASSERT", message: "i < n" });
	});
	it("accepts an abnormal exit without frames", async () => {
		await release("group-exit");
		const r = await report("group-exit", { exception: { type: "KILLED", message_norm: "exit code <N>" } }, []);
		expect(r.status).toBe(201);
		expect((await state("group-exit")).groups[0]).toMatchObject({ fault: "exit", message: null, frames: "[]" });
	});
	it("skips noise frames and hashes four", async () => {
		await release("group-skip");
		// memcpy in libc, the handler and std::sort in the game: all noise.
		const noisy = [frame(0x10, "libc.so.6", "ab".repeat(20)), frame(0x7010), frame(0x1010), frame(0x6010), frame(0x2010), frame(0x3010), frame(0x4010), frame(0x5010)];
		const clean = [frame(0x1010), frame(0x2010), frame(0x3010), frame(0x4010), frame(0x8010)];
		await report("group-skip", {}, noisy);
		await report("group-skip", {}, clean);
		const { groups } = await state("group-skip");
		expect(groups).toHaveLength(1);
		expect(JSON.parse(groups[0]!.frames)).toHaveLength(8);
		// A different fourth frame is another group.
		await report("group-skip", {}, [frame(0x1010), frame(0x2010), frame(0x3010), frame(0x8010)]);
		expect((await state("group-skip")).groups).toHaveLength(2);
	});
	it("names a frame in a module without a table by its module", async () => {
		await release("group-unknown");
		await report("group-unknown", {}, [frame(0x10, "nvoglv64.dll", "cc".repeat(20)), frame(0x1010)]);
		await report("group-unknown", {}, [frame(0x20, "nvoglv64.dll", "cc".repeat(20)), frame(0x1010)]);
		await report("group-unknown", {}, [frame(0x20, null, null), frame(0x1010)]);
		const { groups } = await state("group-unknown");
		expect(groups).toHaveLength(2);
		expect(JSON.parse(groups[0]!.frames)[0]).toMatchObject({ module: "nvoglv64.dll", name: null });
		expect(JSON.parse(groups[1]!.frames)[0]).toEqual({ module: "?", name: null, buildId: null, offset: 0x20 });
	});
	it("looks a return address up inside the call", async () => {
		await release("group-return");
		// 0x1100 is the byte after render_mesh; as a return address it belongs to render_mesh's last call.
		await report("group-return", {}, [frame(0x1010), frame(0x1100)]);
		expect(JSON.parse((await state("group-return")).groups[0]!.frames)).toMatchObject([
			{ module: "game.exe", name: "render_mesh" }, { module: "game.exe", name: "render_mesh" },
		]);
	});
});

describe("late symbols", () => {
	const unnamed = (build: string) => STACK.map((f) => ({ ...f, build_id: build }));
	it("names a group's frames in place when its build's table arrives", async () => {
		await release("late-name");
		await report("late-name", {}, unnamed(OTHER_BUILD_HEX));
		const before = (await state("late-name")).groups[0]!;
		expect(JSON.parse(before.frames)).toMatchObject([{ name: null, buildId: OTHER_BUILD_HEX, offset: 0x1010 }, {}, {}, {}]);
		const table = makeTable({ buildId: OTHER_BUILD, functions: [{ start: 0x1000, size: 0x100, name: "boot" }] });
		expect((await uploadTable("late-name", table)).status).toBe(201);
		const { groups, counts } = await state("late-name");
		expect(groups).toHaveLength(1);
		expect(groups[0]!.id).toBe(before.id);
		expect(groups[0]!.fingerprint).not.toBe(before.fingerprint);
		expect(JSON.parse(groups[0]!.frames)).toMatchObject([{ name: "boot" }, { name: null }, { name: null }, { name: null }]);
		expect(counts[0]!.count).toBe(1);
		// The same crash now hashes with the names: it joins the group instead of opening another.
		await report("late-name", {}, unnamed(OTHER_BUILD_HEX));
		expect((await state("late-name")).groups).toHaveLength(1);
	});
	it("merges a group into the older one its names now match", async () => {
		await release("late-merge");
		const old = envelope({}, unnamed(OTHER_BUILD_HEX));
		old.app = { ...(old.app as object), name: "late-merge" };
		await post("late-merge", old);
		await report("late-merge");
		const before = await state("late-merge");
		expect(before.groups).toHaveLength(2);
		const [unknown, named] = before.groups;
		expect((await uploadTable("late-merge", makeTable({ buildId: OTHER_BUILD, functions: FUNCTIONS }))).status).toBe(201);
		const { groups, counts, reports, samples } = await state("late-merge");
		expect(groups).toHaveLength(1);
		expect(groups[0]!.id).toBe(unknown!.id);
		expect(groups[0]!.fingerprint).toBe(named!.fingerprint);
		expect(JSON.parse(groups[0]!.frames)).toMatchObject([{ name: "render_mesh", buildId: OTHER_BUILD_HEX }, {}, {}, {}]);
		expect(counts).toEqual([{ group_id: unknown!.id, version: "1.0.0", channel: "stable", trust: 0, day: today(), count: 2 }]);
		expect(reports.map((r) => r.group_id)).toEqual([unknown!.id, unknown!.id]);
		expect(samples.map((s) => s.group_id)).toEqual([unknown!.id, unknown!.id]);
	});
});
