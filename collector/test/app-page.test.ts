import { env as bindings, runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import { symbolKey } from "../src/releases";
import { ENVELOPE_OBJECT, sampleKey } from "../src/samples";
import type { AppShard } from "../src/shard";
import { BUILD_ID_HEX, makeTable } from "./table";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, SHARD: bindings.SHARD, BUCKET: bindings.BUCKET, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;

async function page(name: string, sub = "", accept?: string): Promise<Response> {
	const headers: Record<string, string> = { Authorization: auth };
	if (accept !== undefined) headers.Accept = accept;
	return worker.fetch(new Request(`https://dash.example/dashboard/apps/${name}${sub}`, { headers }), env);
}

async function addApp(name: string, display: string, disabled: number | null = null): Promise<void> {
	await bindings.DB.prepare(
		"INSERT INTO apps (name, display_name, created_at, created_by, disabled_at) VALUES (?1, ?2, 1_700_000_000, 'bob', ?3)"
	)
		.bind(name, display, disabled)
		.run();
}

function inShard<T>(name: string, fn: (obj: AppShard) => T | Promise<T>): Promise<T> {
	const ns = bindings.SHARD;
	return runInDurableObject(ns.get(ns.idFromName(name)), (obj) => fn(obj as AppShard));
}

describe("app page", () => {
	it("is 404 for an unknown app, on every page", async () => {
		for (const sub of ["", "/versions", "/tokens", "/settings"]) expect((await page("nobody", sub)).status).toBe(404);
	});
	it("shows the app's details and links its pages, marking the current one", async () => {
		await addApp("page-empty", "Page <Empty>", 1_750_000_000);
		const r = await page("page-empty");
		expect(r.status).toBe(200);
		const html = await r.text();
		expect(html).toContain("<h1>Page &lt;Empty&gt;</h1>");
		expect(html).toContain("<code>page-empty</code>");
		expect(html).toContain("disabled since 2025-06-15");
		expect(html).toContain("created at 2023-11-14 by bob");
		expect(html).toContain('<a href="/dashboard/apps/page-empty" aria-current="page">Crashes</a>');
		expect(html).toContain('<a href="/dashboard/apps/page-empty/versions">Versions</a>');
		expect(html).toContain('<a href="/dashboard/apps/page-empty/tokens">Upload tokens</a>');
		expect(html).toContain('<a href="/dashboard/apps/page-empty/settings">Settings</a>');
		expect(html).toContain("No crashes reported yet");
		expect(html).not.toContain("No versions yet");
		const versions = await (await page("page-empty", "/versions")).text();
		expect(versions).toContain("<h1>Page &lt;Empty&gt;</h1>");
		expect(versions).toContain('<a href="/dashboard/apps/page-empty/versions" aria-current="page">Versions</a>');
		expect(versions).toContain("No versions yet");
		expect(versions).not.toContain("No crashes reported yet");
	});
	it("lists crashes most recently seen first, named by fault, first frame and message", async () => {
		await addApp("page-crashes", "Page Crashes");
		await inShard("page-crashes", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
			await obj.db
				.insertInto("crash_groups")
				.values([
					{
						id: 1, fingerprint: "a".repeat(16), fault: "memory", message: null, first_seen: 1_700_000_000, last_seen: 1_700_000_000,
						frames: JSON.stringify([
							{ module: "libc.so.6", name: "memcpy" }, { module: "game", name: "copy_mesh" }, { module: "game", name: "load_level" },
						]),
					},
					{
						id: 2, fingerprint: "b".repeat(16), fault: "abort", message: "tex != NULL", first_seen: 1_700_000_000, last_seen: 1_750_000_000,
						frames: JSON.stringify([{ module: "game", name: "cw_abort" }, { module: "game", name: "main" }]),
					},
				])
				.execute();
			await obj.db
				.insertInto("crash_counts")
				.values([
					{ group_id: 1, version: "1.0.0", channel: "stable", trust: 0, day: 19_000, count: 3 },
					{ group_id: 1, version: "1.0.0", channel: "beta", trust: 0, day: 19_001, count: 4 },
				])
				.execute();
			// This week: the memory crash hit three users once each, the abort one user twice.
			const hour = Math.floor(Date.now() / 1000) - 3600;
			const row = (id: string, group_id: number, user_key: string) =>
				({ report_id: id, group_id, version: "1.0.0", channel: "stable", trust: 0, user_key, received_at: hour });
			await obj.db
				.insertInto("reports")
				.values([row("r1", 1, "u1"), row("r2", 1, "u2"), row("r3", 1, "u3"), row("r4", 2, "u1"), row("r5", 2, "u1")])
				.execute();
		});
		const html = await (await page("page-crashes")).text();
		expect(html).toContain('<td><a href="/dashboard/apps/page-crashes/crashes/2">abort in main: tex != NULL</a></td>\n<td>1</td>\n<td>2</td>\n<td>1</td>\n<td>0</td>');
		expect(html).toContain('<td><a href="/dashboard/apps/page-crashes/crashes/1">memory in copy_mesh…</a></td>\n<td>3</td>\n<td>3</td>\n<td>3</td>\n<td>7</td>');
		expect(html).not.toContain("from load_level");
		expect(html.indexOf("memory in copy_mesh"), "urgency outranks recency").toBeLessThan(html.indexOf("abort in main"));
		expect(html).toContain("<td>2025-06-15</td>");
	});
	it("lists versions newest first with their channels and builds", async () => {
		await addApp("page-full", "Page Full");
		await inShard("page-full", async (obj) => {
			await obj.db
				.insertInto("versions")
				.values([
					{ version: "1.0.0", created_at: 1_700_000_000 },
					{ version: "1.1.0", created_at: 1_700_100_000 },
				])
				.execute();
			await obj.db
				.insertInto("releases")
				.values([
					{ channel: "stable", version: "1.0.0", released_at: 1, supported_until: 1_750_000_000 },
					{ channel: "beta", version: "1.1.0", released_at: 2, supported_until: null },
					{ channel: "stable", version: "1.1.0", released_at: 3, supported_until: null },
				])
				.execute();
			await obj.db
				.insertInto("builds")
				.values([
					{ build_id: "win1", version: "1.0.0", uploaded_at: 1 },
					{ build_id: "lin2", version: "1.1.0", uploaded_at: 1 },
					{ build_id: "win2", version: "1.1.0", uploaded_at: 1 },
				])
				.execute();
		});
		const html = await (await page("page-full", "/versions")).text();
		expect(html.indexOf("<code>1.1.0</code>")).toBeLessThan(html.indexOf("<code>1.0.0</code>"));
		expect(html).toContain("<div>beta</div>");
		expect(html).toContain("<div>stable <small>until 2025-06-15</small></div>");
		expect(html).toContain("<code>lin2</code>");
		expect(html.indexOf("<code>lin2</code>")).toBeLessThan(html.indexOf("<code>win2</code>"));
		// A link from a crash names a version, which the page marks.
		const marked = await (await page("page-full", "/versions?version=1.0.0")).text();
		expect(marked).toContain("<td>→ <code>1.0.0</code></td>");
		expect(marked).toContain("<td><code>1.1.0</code></td>");
		expect(html).not.toContain("→ ");
	});
	it("serves each page's data as JSON when asked", async () => {
		await addApp("page-json", "Page JSON");
		await inShard("page-json", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "2.0.0", created_at: 1 }).execute();
			await obj.db.insertInto("releases").values({ channel: "stable", version: "2.0.0", released_at: 1 }).execute();
			await obj.db
				.insertInto("crash_groups")
				.values({
					id: 7, fingerprint: "c".repeat(16), fault: "memory", message: null, first_seen: 5, last_seen: 6,
					frames: JSON.stringify([{ module: "game", name: "tick" }]),
				})
				.execute();
			await obj.db
				.insertInto("crash_counts")
				.values({ group_id: 7, version: "2.0.0", channel: "stable", trust: 0, day: 1, count: 2 })
				.execute();
		});
		await bindings.DB.prepare(
			"INSERT INTO upload_tokens (app_id, hash, label, created_at, created_by) SELECT id, 'h', 'ci', 3, 'bob' FROM apps WHERE name = 'page-json'"
		).run();
		const json = async (sub: string) => {
			const r = await page("page-json", sub, "application/json");
			expect(r.status).toBe(200);
			expect(r.headers.get("Content-Type")).toMatch(/^application\/json/);
			expect(r.headers.get("Vary")).toBe("Accept");
			const data = await r.json() as Record<string, unknown>;
			expect(data.app).toMatchObject({ name: "page-json", display_name: "Page JSON", disabled_at: null });
			return data;
		};
		expect(await (await page("page-json")).text()).toContain('<td><a href="/dashboard/apps/page-json/crashes/7">memory in tick</a></td>');
		expect(await json("")).toEqual({
			app: expect.any(Object),
			crashes: [
				{
					id: 7, title: "memory in tick", fault: "memory", message: null, frames: [{ module: "game", name: "tick" }],
					count: 2, recent_count: 0, recent_users: 0, urgency: 0, first_seen: 5, last_seen: 6,
				},
			],
		});
		expect(await json("/versions")).toEqual({
			app: expect.any(Object),
			versions: [
				{ version: "2.0.0", created_at: 1, channels: [{ channel: "stable", released_at: 1, supported_until: null }], builds: [] },
			],
		});
		expect(await json("/tokens")).toEqual({
			app: expect.any(Object),
			tokens: [{ id: expect.any(Number), label: "ci", created_at: 3, created_by: "bob", last_used_at: null }],
		});
	});
	it("follows the Accept header's quality, then its order", async () => {
		await addApp("page-any", "Page Any");
		const type = async (accept: string) => {
			const r = await page("page-any", "", accept);
			expect(r.headers.get("Vary")).toBe("Accept");
			return r.headers.get("Content-Type")!.split(";")[0];
		};
		expect(await type("*/*")).toBe("text/html");
		expect(await type("text/html, application/json")).toBe("text/html");
		expect(await type("application/json, text/html")).toBe("application/json");
		expect(await type("text/html;q=0.5, application/json")).toBe("application/json");
		expect(await type("application/json;q=0.9, */*;q=0.8")).toBe("application/json");
		expect(await type("text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8")).toBe("text/html");
	});
	it("requires a login", async () => {
		const r = await worker.fetch(new Request("https://dash.example/dashboard/apps/page-full"), env);
		expect(r.status).toBe(401);
	});
});

/** An envelope as the client stores one, crashing in render_mesh under draw_scene with libc on top. */
function envelope(app: string, reportId: string, over: Record<string, unknown> = {}): Record<string, unknown> {
	return {
		schema: 2,
		report_id: reportId,
		install_id: "11111111-1111-4111-8111-111111111111",
		sent_at: 1_750_000_000,
		app: { name: app, version: "1.0.0", build_id: BUILD_ID_HEX, channel: "stable" },
		env: { os: "linux", gpu_vendor: "intel" },
		exception: { type: "SIGSEGV", message_norm: "read from <ADDR>", message_raw: "read from 0x10", thread: 7 },
		modules: [{ name: "game.exe", build_id: BUILD_ID_HEX, base: "0x400000", size: 4096 }],
		frames: [
			{ module: "libc.so.6", build_id: "ff", offset: 0x10 },
			{ module: "game.exe", build_id: BUILD_ID_HEX, offset: 0x1010 },
			{ module: "game.exe", build_id: BUILD_ID_HEX, offset: 0x2010 },
			{ module: null, build_id: null, offset: 0x99 },
			{ module: "javascript:tick", build_id: "0", offset: 0, raw: "    at tick (https://example.com/game.js:12:5)" },
		],
		breadcrumbs: [{ t: 1000, th: 1, c: "level", m: "load forest_02" }, { t: 2500, th: 7, c: "render", m: "frame" }],
		state: { level: "forest_02" },
		attachments: { log_tail: true, minidump: true, snapshot: false },
		...over,
	};
}

/** An app with a group of two samples, `s-new` and `s-old`, and a group without any; returns the path of the first. */
async function crashApp(app: string): Promise<string> {
	await addApp(app, "Page Crash");
	// render_mesh has copy_verts inlined over its first half; draw_scene has lines but no display name.
	await bindings.BUCKET.put(
		symbolKey(app, BUILD_ID_HEX),
		makeTable({
			functions: [{ start: 0x1000, size: 0x100, name: "render_mesh" }, { start: 0x2000, size: 0x100, name: "draw_scene" }],
			displays: ["render_mesh(mesh*)"],
			lines: [
				{ start: 0x1000, size: 0x80, file: "src/mesh.h", line: 12 },
				{ start: 0x1080, size: 0x80, file: "src/render.c", line: 44 },
				{ start: 0x2000, size: 0x100, file: "src/render.c", line: 90 },
			],
			sites: [{ start: 0x1000, size: 0x80, callee: "copy_verts", file: "src/render.c", line: 41, parent: null }],
		})
	);
	const frames = [
		{ module: "libc.so.6", name: null, buildId: "ff", offset: 0x10 },
		{ module: "game.exe", name: "render_mesh", buildId: BUILD_ID_HEX, offset: 0x1010 },
		{ module: "game.exe", name: "draw_scene", buildId: BUILD_ID_HEX, offset: 0x2010 },
	];
	await inShard(app, async (obj) => {
		await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
		await obj.db
			.insertInto("crash_groups")
			.values([
				{ id: 3, fingerprint: "d".repeat(16), fault: "memory", message: null, first_seen: 1_740_000_000, last_seen: 1_750_000_000, frames: JSON.stringify(frames) },
				{ id: 4, fingerprint: "e".repeat(16), fault: "abort", message: "oops", first_seen: 1, last_seen: 2, frames: JSON.stringify(frames.slice(0, 2)) },
			])
			.execute();
		await obj.db
			.insertInto("crash_counts")
			.values({ group_id: 3, version: "1.0.0", channel: "stable", trust: 0, day: 20_000, count: 5 })
			.execute();
		// Install u1 crashed twice, both sampled; u2 once, not sampled.
		const row = (id: string, channel: string, user_key: string, received_at: number) =>
			({ report_id: id, group_id: 3, version: "1.0.0", channel, trust: 0, user_key, received_at });
		await obj.db
			.insertInto("reports")
			.values([row("s-new", "stable", "u1", 1_750_000_000), row("s-old", "beta", "u1", 1_740_000_000), row("r3", "stable", "u2", 1_745_000_000)])
			.execute();
		await obj.db
			.insertInto("crash_samples")
			.values([
				{ report_id: "s-old", group_id: 3, version: "1.0.0", trust: 0, r2_key: sampleKey(app, "s-old"), received_at: 1_740_000_000 },
				{ report_id: "s-new", group_id: 3, version: "1.0.0", trust: 0, r2_key: sampleKey(app, "s-new"), received_at: 1_750_000_000 },
			])
			.execute();
	});
	await bindings.BUCKET.put(sampleKey(app, "s-new") + ENVELOPE_OBJECT, JSON.stringify(envelope(app, "s-new")));
	await bindings.BUCKET.put(sampleKey(app, "s-old") + ENVELOPE_OBJECT, JSON.stringify(envelope(app, "s-old", {
		state: { level: "cave_01" },
		exception: { type: "SIGSEGV", message_norm: "read <N> bytes", message_raw: "read 16 bytes at 0x10", thread: 7 },
	})));
	await bindings.BUCKET.put(sampleKey(app, "s-new") + "1_c_s-new.log", "log tail", {
		httpMetadata: { contentType: "text/plain", contentEncoding: "gzip" },
	});
	return `/dashboard/apps/${app}/crashes/3`;
}

describe("crash page", () => {
	it("is 404 for an unknown crash, a malformed id and an unknown sample", async () => {
		const app = "crash-404";
		const CRASH = await crashApp(app);
		expect((await page("nobody", "/crashes/3")).status).toBe(404);
		expect((await page(app, "/crashes/abc")).status).toBe(404);
		expect((await page(app, "/crashes/99")).status).toBe(404);
		expect((await page(app, "/crashes/3?sample=nope")).status).toBe(404);
	});
	it("shows the group's facts and its newest sample in full", async () => {
		const app = "crash-newest";
		const CRASH = await crashApp(app);
		const r = await page(app, "/crashes/3");
		expect(r.status).toBe(200);
		const html = await r.text();
		expect(html).toContain(`<a href="/dashboard/apps/${app}" aria-current="page">Crashes</a>`);
		expect(html).toContain(`<link rel="canonical" href="https://dash.example${CRASH}?sample=s-new">`);
		expect(html).toContain("<h2>memory in render_mesh, from draw_scene</h2>");
		expect(html).toContain("<code>memory</code> · first seen 2025-02-19 21:20:00 UTC · last seen 2025-06-15 15:06:40 UTC");
		expect(html).toContain("5 reports in total");
		expect(html).toContain(`<tr><td><a href="/dashboard/apps/${app}/versions?version=1.0.0">1.0.0</a></td><td>stable</td><td>5</td></tr>`);
		// One install repeats, and it has a sample to link to; the other does not repeat.
		expect(html).toContain("<p>3 reports from 2 installs.</p>");
		expect(html).toContain(`<td><code>u1</code></td>\n<td>2</td>\n<td>2025-06-15 15:06:40 UTC</td>\n<td><a href="${CRASH}?sample=s-new"><code>s-new</code></a></td>`);
		expect(html).not.toContain("<code>u2</code>");
		expect(html).toContain("<code>SIGSEGV</code> on thread 7");
		// The address the normalizer replaces is underlined in the raw message.
		expect(html).toContain('<pre>read from <u title="&lt;ADDR&gt;">0x10</u></pre>');
		expect(html).not.toContain("Normalized:");
		const at = (s: string) => { const i = html.indexOf(s); expect(i, s).toBeGreaterThan(-1); return i; };
		expect(at("<h2>memory in")).toBeLessThan(at("<h3>Versions</h3>"));
		expect(at("<h3>Versions</h3>")).toBeLessThan(at("<h3>Users</h3>"));
		expect(at("<h3>Users</h3>")).toBeLessThan(at("<h3>Exception</h3>"));
		expect(at("<h3>Files</h3>")).toBeLessThan(at("<h3>Samples</h3>"));
		// The stack is named from the table; libc is skipped and the two game frames are marked.
		expect(html).toContain("<th>#</th><th>Function</th><th>Location</th><th>Module</th><th>Offset</th></tr>");
		expect(html).toContain("<td>0</td>\n<td><small>unnamed</small></td>\n<td></td>\n<td><code>libc.so.6</code></td>\n<td><code>0x10</code></td>");
		// Frame 1 is inside copy_verts inlined into render_mesh: the innermost level first, the caller indented under it.
		expect(html).toContain("<td>1</td>\n<td><mark>copy_verts</mark></td>\n<td><code>src/mesh.h:12</code></td>\n<td><code>game.exe</code></td>\n<td><code>0x1010</code></td>");
		// A JavaScript frame has no table; its location is the browser's own trace line.
		expect(html).toContain("<td>4</td>\n<td><mark>tick</mark></td>\n<td><code>    at tick (https://example.com/game.js:12:5)</code></td>\n<td><code>javascript</code></td>");
		expect(html).toContain("<td></td>\n<td>&nbsp;&nbsp;&nbsp;&nbsp;↳ <mark>render_mesh(mesh*)</mark></td>\n<td><code>src/render.c:41</code></td>");
		expect(html).toContain("<td>2</td>\n<td><mark>draw_scene</mark></td>\n<td><code>src/render.c:90</code></td>");
		// A frame in no module is not noise, so it enters the fingerprint too.
		expect(html).toContain("<td>3</td>\n<td><mark><small>unnamed</small></mark></td>\n<td></td>\n<td><code>?</code></td>\n<td><code>0x99</code></td>");
		expect(html).toContain("<tr><td>-1.500 s</td><td>1</td><td>level</td><td>load forest_02</td></tr>");
		expect(html).toContain("<tr><td>0.000 s</td><td><mark>7</mark></td><td>render</td><td>frame</td></tr>");
		// The other sample is on another level but the same machine: shared values are bold with their share.
		expect(html).toContain('<tr><th scope="row">level</th><td>forest_02</td><td>1 of 2</td></tr>');
		expect(html).toContain('<tr><th scope="row"><strong>os</strong></th><td><strong>linux</strong></td><td>2 of 2</td></tr>');
		expect(html).toContain('<tr><th scope="row"><strong>gpu_vendor</strong></th><td><strong>intel</strong></td><td>2 of 2</td></tr>');
		expect(html).toContain("<tr><td><code>game.exe</code></td><td><code>" + BUILD_ID_HEX + "</code></td><td><code>0x400000</code></td><td>4096</td></tr>");
		expect(html).toContain(`<li><a href="${CRASH}/samples/s-new/envelope.json">envelope.json</a></li>`);
		expect(html).toContain(`<li><a href="${CRASH}/samples/s-new/1_c_s-new.log">1_c_s-new.log</a> <small>8 bytes</small></li>`);
		// The log arrived; the minidump the client declared did not.
		expect(html).toContain('<li><s title="Not received">minidump</s></li>');
		expect(html).not.toContain("<s title=\"Not received\">log_tail</s>");
		// Every sample is listed, newest first; an arrow marks the current one, whose link is its permanent URL.
		expect(html).toContain(`<td>→ <a href="${CRASH}?sample=s-new" aria-current="page"><code>s-new</code></a><br><small>install <code>u1</code> · 2 reports</small></td>\n<td><code>1.0.0</code></td>\n<td>stable</td>\n<td>2025-06-15 15:06:40 UTC</td>`);
		expect(html).toContain(`<td><a href="${CRASH}?sample=s-old"><code>s-old</code></a></td>`);
		expect(html.indexOf("<small>current</small>")).toBeLessThan(html.indexOf("?sample=s-old"));
	});
	it("shows the sample the query names", async () => {
		const app = "crash-query";
		const CRASH = await crashApp(app);
		const html = await (await page(app, "/crashes/3?sample=s-old")).text();
		expect(html).toContain(`<link rel="canonical" href="https://dash.example${CRASH}?sample=s-old">`);
		expect(html).toContain('<tr><th scope="row">level</th><td>cave_01</td><td>1 of 2</td></tr>');
		// A normalized message the page cannot overlay on the raw one is shown beside it.
		expect(html).toContain("<pre>read 16 bytes at 0x10</pre>");
		expect(html).toContain("<p>Normalized: <code>read &lt;N&gt; bytes</code></p>");
		expect(html).toContain(`<td>→ <a href="${CRASH}?sample=s-old" aria-current="page"><code>s-old</code></a><br><small>install <code>u1</code> · 2 reports</small></td>`);
		expect(html).toContain(`<td><a href="${CRASH}?sample=s-new"><code>s-new</code></a></td>`);
		expect(html).toContain(`<td><a href="${CRASH}?sample=s-new"><code>s-new</code></a></td>`);
	});
	it("shows the stored frames of a group without samples", async () => {
		const app = "crash-stored";
		const CRASH = await crashApp(app);
		const html = await (await page(app, "/crashes/4")).text();
		expect(html).toContain("<h2>abort in render_mesh: oops</h2>");
		expect(html).toContain(`<link rel="canonical" href="https://dash.example/dashboard/apps/${app}/crashes/4">`);
		expect(html).toContain("<small>no reports counted</small>");
		expect(html).toContain("<p>0 reports from 0 installs, none of them twice.</p>");
		expect(html).toContain("No sample is held for this crash");
		// Stored frames carry no locations, so there is no Location column.
		expect(html).not.toContain("<th>Location</th>");
		expect(html).toContain("<td>0</td>\n<td><small>unnamed</small></td>\n<td><code>libc.so.6</code></td>");
		expect(html).toContain("<td>1</td>\n<td><mark>render_mesh</mark></td>");
		expect(html).not.toContain("<h3>Samples</h3>");
	});
	it("says so when a sample's envelope is gone", async () => {
		const app = "crash-gone";
		const CRASH = await crashApp(app);
		await bindings.BUCKET.delete(sampleKey(app, "s-new") + ENVELOPE_OBJECT);
		const html = await (await page(app, "/crashes/3")).text();
		expect(html).toContain("The envelope of this sample is no longer stored.");
		expect(html).toContain(`<td><a href="${CRASH}?sample=s-old"><code>s-old</code></a></td>`);
		// The other sample alone is readable, so it has nothing to compare with.
		const other = await (await page(app, "/crashes/3?sample=s-old")).text();
		expect(other).toContain('<tr><th scope="row">level</th><td>cave_01</td></tr>');
		expect(other).not.toContain("<th>Samples</th>");
	});
	it("serves the sample's files as stored, attachments as downloads", async () => {
		const app = "crash-files";
		const CRASH = await crashApp(app);
		const envelopeFile = await page(app, "/crashes/3/samples/s-new/envelope.json");
		expect(envelopeFile.status).toBe(200);
		expect(envelopeFile.headers.get("Content-Disposition")).toBeNull();
		expect(await envelopeFile.json()).toMatchObject({ report_id: "s-new" });
		const log = await page(app, "/crashes/3/samples/s-new/1_c_s-new.log");
		expect(log.status).toBe(200);
		expect(log.headers.get("Content-Type")).toBe("text/plain");
		expect(log.headers.get("Content-Encoding")).toBe("gzip");
		expect(log.headers.get("Content-Disposition")).toBe('attachment; filename="1_c_s-new.log"');
		expect((await page(app, "/crashes/3/samples/s-new/1_c_s-old.log")).status).toBe(404);
		expect((await page(app, "/crashes/3/samples/s-new/evil.dmp")).status).toBe(404);
		expect((await page(app, "/crashes/3/samples/nobody/envelope.json")).status).toBe(404);
		expect((await page(app, "/crashes/4/samples/s-new/envelope.json")).status).toBe(404);
	});
	it("serves the crash as JSON when asked", async () => {
		const app = "crash-json";
		const CRASH = await crashApp(app);
		const r = await page(app, "/crashes/3", "application/json");
		expect(r.status).toBe(200);
		const data = await r.json() as Record<string, unknown>;
		expect(data.crash).toMatchObject({ id: 3, title: "memory in render_mesh, from draw_scene", count: 5 });
		expect(data.permalink).toBe(`https://dash.example${CRASH}?sample=s-new`);
		expect(data.releases).toEqual([{ version: "1.0.0", channel: "stable", count: 5 }]);
		expect(data.problem).toBeNull();
		expect(data.shared).toEqual({ total: 2, state: { level: 1 }, env: { os: 2, gpu_vendor: 2 } });
		expect(data.sample).toMatchObject({
			report_id: "s-new", version: "1.0.0", channel: "stable", type: "SIGSEGV", thread: 7, message_raw: "read from 0x10",
			user_key: "u1", sent_at: 1_750_000_000, message_norm: "read from <ADDR>",
			hashed: [1, 2, 3, 4], state: { level: "forest_02" }, attachments: [{ name: "1_c_s-new.log", size: 8 }],
			declared: { log_tail: true, minidump: true, snapshot: false },
			breadcrumbs: [{ t: 1000, th: 1, c: "level", m: "load forest_02" }, { t: 2500, th: 7, c: "render", m: "frame" }],
		});
		expect((data.sample as { frames: { name: string | null }[] }).frames.map((f) => f.name)).toEqual([null, "render_mesh", "draw_scene", null, "tick"]);
		expect((data.sample as { locations: unknown }).locations).toEqual([
			[],
			[{ function: "copy_verts", file: "src/mesh.h", line: 12 }, { function: "render_mesh(mesh*)", file: "src/render.c", line: 41 }],
			[{ function: "draw_scene", file: "src/render.c", line: 90 }],
			[],
			[],
		]);
		expect((data.sample as { trace_line: unknown }).trace_line).toEqual([null, null, null, null, "    at tick (https://example.com/game.js:12:5)"]);
		expect(data.users).toEqual({ reports: 3, users: 2, top: [{ trust: 0, user_key: "u1", count: 2, last_seen: 1_750_000_000 }] });
		expect(data.samples).toMatchObject([{ report_id: "s-new", user_key: "u1", user_reports: 2 }, { report_id: "s-old", user_key: "u1", user_reports: 2 }]);
	});
});
