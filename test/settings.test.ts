import { env as bindings } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import { getApp } from "../src/apps";
import { createDb } from "../src/db";
import worker from "../src/index";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, SHARD: bindings.SHARD, BUCKET: bindings.BUCKET, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;
const origin = "https://dash.example";
const db = createDb(bindings.DB);

const good = { display_name: "Forest Quest", sample_cap_trusted: "7", sample_cap_untrusted: "0" };

async function get(path: string, accept?: string): Promise<Response> {
	const headers: Record<string, string> = { Authorization: auth };
	if (accept !== undefined) headers.Accept = accept;
	return worker.fetch(new Request(`${origin}${path}`, { headers }), env);
}

async function post(
	path: string, headers: Record<string, string> = { Authorization: auth, Origin: origin },
	fields: Record<string, string> = good
): Promise<Response> {
	return worker.fetch(
		new Request(`${origin}${path}`, {
			method: "POST",
			headers: { "Content-Type": "application/x-www-form-urlencoded", ...headers },
			body: new URLSearchParams(fields),
		}),
		env
	);
}

/** Submits the form and follows the redirect as a browser would. */
async function submit(app: string, fields: Record<string, string>): Promise<{ location: string; page: string }> {
	const r = await post(`/dashboard/apps/${app}/settings`, undefined, fields);
	expect(r.status).toBe(303);
	const location = r.headers.get("Location")!;
	const landed = await get(location);
	expect(landed.status).toBe(200);
	return { location, page: await landed.text() };
}

async function addApp(name: string, disabled: number | null = null): Promise<void> {
	await bindings.DB.prepare(
		"INSERT INTO apps (name, display_name, created_at, created_by, disabled_at) VALUES (?1, 'Old Name', 1_700_000_000, 'bob', ?2)"
	)
		.bind(name, disabled)
		.run();
}

function input(page: string, name: string): string {
	const tag = new RegExp(`<input name="${name}"[^>]*>`).exec(page)?.[0];
	expect(tag).toBeDefined();
	return tag!;
}

describe("app settings", () => {
	it("show the stored values and the kill switch", async () => {
		await addApp("set-show");
		const page = await (await get("/dashboard/apps/set-show/settings")).text();
		expect(page).toContain('<a href="/dashboard/apps/set-show/settings" aria-current="page">Settings</a>');
		expect(page).toContain("<h3 id=\"display-name\">Display name</h3>");
		expect(page).toContain("<h3>Sample caps</h3>");
		expect(page).toContain("<h3>Kill switch</h3>");
		expect(input(page, "display_name")).toMatch(/value="Old Name"[^>]*aria-labelledby="display-name"/);
		expect(input(page, "sample_cap_trusted")).toMatch(/value="5"[^>]*type="number"/);
		expect(input(page, "sample_cap_untrusted")).toContain('value="2"');
		expect(page).not.toContain("aria-invalid");
		expect(page).toContain('action="/dashboard/apps/set-show/settings/disable"');
		expect(page).not.toContain("/settings/enable");
	});
	it("save the display name and caps, which every page then shows", async () => {
		await addApp("set-save");
		const { location, page } = await submit("set-save", { ...good, display_name: " Forest <Quest> " });
		expect(location).toBe("/dashboard/apps/set-save/settings");
		expect(page).toContain("<h1>Forest &lt;Quest&gt;</h1>");
		expect(input(page, "display_name")).toContain('value="Forest &lt;Quest&gt;"');
		expect(input(page, "sample_cap_trusted")).toContain('value="7"');
		expect(input(page, "sample_cap_untrusted")).toContain('value="0"');
		expect(await getApp(db, "set-save")).toMatchObject({
			display_name: "Forest <Quest>", sample_cap_trusted: 7, sample_cap_untrusted: 0,
		});
		expect(await (await get("/dashboard")).text()).toContain("Forest &lt;Quest&gt;");
	});
	it("mark the field at fault, keep the typed values and store nothing", async () => {
		await addApp("set-bad");
		for (const [field, value] of [
			["display_name", ""], ["display_name", "x".repeat(101)],
			["sample_cap_trusted", "abc"], ["sample_cap_trusted", "-1"], ["sample_cap_trusted", "1.5"],
			["sample_cap_untrusted", "101"], ["sample_cap_untrusted", ""],
		] as const) {
			const { location, page } = await submit("set-bad", { ...good, [field]: value });
			expect(location).toMatch(/^\/dashboard\/apps\/set-bad\/settings\?/);
			expect(input(page, field)).toContain('aria-invalid="true"');
			expect(input(page, field)).toContain(`value="${value}"`);
			expect(page.match(/aria-invalid/g)).toHaveLength(1);
			expect(page).toContain("<h1>Old Name</h1>");
		}
		expect(input((await submit("set-bad", { ...good, sample_cap_untrusted: "" })).page, "display_name")).toContain('value="Forest Quest"');
		expect(await getApp(db, "set-bad")).toMatchObject({ display_name: "Old Name", sample_cap_trusted: 5, sample_cap_untrusted: 2 });
	});
	it("disable and enable the app, which ingest honours", async () => {
		await addApp("set-kill");
		const off = await post("/dashboard/apps/set-kill/settings/disable", undefined, {});
		expect(off.status).toBe(303);
		expect(off.headers.get("Location")).toBe("/dashboard/apps/set-kill/settings");
		const disabled = await (await get("/dashboard/apps/set-kill/settings")).text();
		expect(disabled).toMatch(/disabled since \d{4}-\d{2}-\d{2}/);
		expect(disabled).toContain('action="/dashboard/apps/set-kill/settings/enable"');
		expect(disabled).not.toContain("/settings/disable");
		const refused = await worker.fetch(new Request(`${origin}/v1/set-kill/report`, { method: "POST", body: "{}" }), env);
		expect(refused.status).toBe(403);
		const at = (await getApp(db, "set-kill"))!.disabled_at;
		expect(at).not.toBeNull();
		// Disabling again keeps the original moment.
		await post("/dashboard/apps/set-kill/settings/disable", undefined, {});
		expect((await getApp(db, "set-kill"))!.disabled_at).toBe(at);
		const on = await post("/dashboard/apps/set-kill/settings/enable", undefined, {});
		expect(on.status).toBe(303);
		expect((await getApp(db, "set-kill"))!.disabled_at).toBeNull();
		const enabled = await (await get("/dashboard/apps/set-kill/settings")).text();
		expect(enabled).toContain("active");
		expect(enabled).toContain('action="/dashboard/apps/set-kill/settings/disable"');
	});
	it("are 404 for an unknown app and refused without a login or origin", async () => {
		expect((await get("/dashboard/apps/nobody/settings")).status).toBe(404);
		expect((await post("/dashboard/apps/nobody/settings")).status).toBe(404);
		expect((await post("/dashboard/apps/nobody/settings/disable", undefined, {})).status).toBe(404);
		await addApp("set-guard");
		expect((await post("/dashboard/apps/set-guard/settings", { Origin: origin })).status).toBe(401);
		expect((await post("/dashboard/apps/set-guard/settings", { Authorization: auth })).status).toBe(403);
		expect((await post("/dashboard/apps/set-guard/settings/enable", { Authorization: auth }, {})).status).toBe(403);
		expect(await getApp(db, "set-guard")).toMatchObject({ display_name: "Old Name" });
	});
});

describe("app settings as JSON", () => {
	const json = { Authorization: auth, Origin: origin, Accept: "application/json" };
	it("read and update the row", async () => {
		await addApp("json-set");
		const shown = await (await get("/dashboard/apps/json-set/settings", "application/json")).json();
		expect(shown).toMatchObject({ app: { name: "json-set", display_name: "Old Name", sample_cap_trusted: 5, sample_cap_untrusted: 2 } });
		const r = await worker.fetch(
			new Request(`${origin}/dashboard/apps/json-set/settings`, {
				method: "POST",
				headers: { Authorization: auth, Accept: "application/json", "Content-Type": "application/json" },
				body: JSON.stringify({ display_name: "New Name", sample_cap_trusted: "9", sample_cap_untrusted: "3" }),
			}),
			env
		);
		expect(r.status).toBe(200);
		expect(r.headers.get("Vary")).toBe("Accept");
		expect(await r.json()).toMatchObject({ app: { display_name: "New Name", sample_cap_trusted: 9, sample_cap_untrusted: 3 } });
	});
	it("name the field at fault", async () => {
		await addApp("json-bad");
		const r = await post("/dashboard/apps/json-bad/settings", json, { ...good, sample_cap_untrusted: "x" });
		expect(r.status).toBe(400);
		expect(await r.json()).toEqual({
			error: { field: "sample_cap_untrusted", message: expect.stringMatching(/^The sample cap must be/) },
		});
	});
	it("flip the kill switch with the row instead of a redirect", async () => {
		await addApp("json-kill");
		const off = await post("/dashboard/apps/json-kill/settings/disable", json, {});
		expect(off.status).toBe(200);
		expect(await off.json()).toMatchObject({ app: { disabled_at: expect.any(Number) } });
		const on = await post("/dashboard/apps/json-kill/settings/enable", json, {});
		expect(on.status).toBe(200);
		expect(await on.json()).toMatchObject({ app: { disabled_at: null } });
	});
});
