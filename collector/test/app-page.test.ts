import { env as bindings, runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import type { AppShard } from "../src/shard";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, SHARD: bindings.SHARD, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;

async function page(name: string): Promise<Response> {
	return worker.fetch(new Request(`https://dash.example/dashboard/apps/${name}`, { headers: { Authorization: auth } }), env);
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
	it("is 404 for an unknown app", async () => {
		expect((await page("nobody")).status).toBe(404);
	});
	it("shows the app's details and an empty versions list", async () => {
		await addApp("page-empty", "Page <Empty>", 1_750_000_000);
		const r = await page("page-empty");
		expect(r.status).toBe(200);
		const html = await r.text();
		expect(html).toContain("<h1>Page &lt;Empty&gt;</h1>");
		expect(html).toContain("<code>page-empty</code>");
		expect(html).toContain("disabled since 2025-06-15");
		expect(html).toContain("created at 2023-11-14 by bob");
		expect(html).toContain("No versions yet");
		expect(html).toContain("No crashes reported yet");
	});
	it("lists crashes most recently seen first, named by fault, frames and message", async () => {
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
		});
		const html = await (await page("page-crashes")).text();
		expect(html).toContain("<td>abort in main: tex != NULL</td>");
		expect(html).toContain("<td>memory in copy_mesh, from load_level</td>\n<td>7</td>");
		expect(html.indexOf("abort in main")).toBeLessThan(html.indexOf("memory in copy_mesh"));
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
		const html = await (await page("page-full")).text();
		expect(html.indexOf("<code>1.1.0</code>")).toBeLessThan(html.indexOf("<code>1.0.0</code>"));
		expect(html).toContain("beta <small>current</small>");
		expect(html).toContain("stable <small>until 2025-06-15</small>");
		expect(html).toContain("<code>lin2</code>");
		expect(html.indexOf("<code>lin2</code>")).toBeLessThan(html.indexOf("<code>win2</code>"));
	});
	it("requires a login", async () => {
		const r = await worker.fetch(new Request("https://dash.example/dashboard/apps/page-full"), env);
		expect(r.status).toBe(401);
	});
});
