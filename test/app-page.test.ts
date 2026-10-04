import { env as bindings, runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import type { AppShard } from "../src/shard";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, SHARD: bindings.SHARD, DASHBOARD_PASSWORD: password };
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
		for (const sub of ["", "/versions", "/tokens"]) expect((await page("nobody", sub)).status).toBe(404);
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
		expect(html).toContain("<td>abort in main: tex != NULL</td>\n<td>1</td>\n<td>2</td>\n<td>1</td>\n<td>0</td>");
		expect(html).toContain("<td>memory in copy_mesh…</td>\n<td>3</td>\n<td>3</td>\n<td>3</td>\n<td>7</td>");
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
		expect(html).toContain("beta <small>current</small>");
		expect(html).toContain("stable <small>until 2025-06-15</small>");
		expect(html).toContain("<code>lin2</code>");
		expect(html.indexOf("<code>lin2</code>")).toBeLessThan(html.indexOf("<code>win2</code>"));
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
		expect(await (await page("page-json")).text()).toContain("<td>memory in tick</td>");
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
			tokens: [{ id: expect.any(Number), label: "ci", created_at: 3, created_by: "bob", last_used_at: null, revoked_at: null }],
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
