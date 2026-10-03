import { env as bindings } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { createApp } from "../src/apps";
import { createDb } from "../src/db";
import worker from "../src/index";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;

async function get(): Promise<Response> {
	return worker.fetch(new Request("https://dash.example/dashboard", { headers: { Authorization: auth } }), env);
}

async function create(fields: Record<string, string>): Promise<Response> {
	return worker.fetch(
		new Request("https://dash.example/dashboard/apps", {
			method: "POST",
			headers: { Authorization: auth, "Content-Type": "application/x-www-form-urlencoded" },
			body: new URLSearchParams(fields),
		}),
		env
	);
}

beforeEach(async () => {
	await bindings.DB.exec("DELETE FROM apps");
});

describe("app listing", () => {
	it("shows an empty state", async () => {
		const r = await get();
		expect(r.status).toBe(200);
		expect(r.headers.get("Content-Type")).toMatch(/^text\/html/);
		const page = await r.text();
		expect(page).toContain("No apps yet");
		expect(page).toContain("Signed in as alice");
	});
	it("lists every app with its name, creator and date", async () => {
		await bindings.DB.prepare(
			"INSERT INTO apps (name, display_name, created_at, created_by, disabled_at) VALUES (?1, ?2, ?3, ?4, ?5)"
		)
			.bind("old-game", "Old Game", 1_700_000_000, "bob@example.com", 1_750_000_000)
			.run();
		expect((await create({ name: "forest-quest", display_name: "Forest Quest" })).status).toBe(303);
		const page = await (await get()).text();
		expect(page).toContain("<code>forest-quest</code>");
		expect(page).toContain("Forest Quest");
		expect(page).toContain('href="/dashboard/apps/forest-quest"');
		expect(page).toContain("alice");
		expect(page).toContain("2023-11-14");
		expect(page).toContain("disabled");
		expect(page.indexOf("Forest Quest")).toBeLessThan(page.indexOf("Old Game"));
		await create({ name: "aaa", display_name: "aardvark" });
		const again = await (await get()).text();
		expect(again.indexOf("aardvark")).toBeLessThan(again.indexOf("Forest Quest"));
	});
	it("links the stylesheet and serves it behind the login", async () => {
		const page = await (await get()).text();
		expect(page).toContain('<link rel="stylesheet" href="/dashboard/pico.css">');
		const css = await worker.fetch(
			new Request("https://dash.example/dashboard/pico.css", { headers: { Authorization: auth } }), env
		);
		expect(css.status).toBe(200);
		expect(css.headers.get("Content-Type")).toMatch(/^text\/css/);
		expect(await css.text()).toContain("--pico-");
		const anonymous = await worker.fetch(new Request("https://dash.example/dashboard/pico.css"), env);
		expect(anonymous.status).toBe(401);
	});
	it("gives the name input a pattern browsers can compile", async () => {
		const page = await (await get()).text();
		const pattern = /name="name"[^>]*pattern="([^"]*)"/.exec(page)?.[1];
		expect(pattern).toBeDefined();
		// Browsers compile the attribute with the v flag, which is stricter inside classes.
		const re = new RegExp(`^(?:${pattern})$`, "v");
		expect(re.test("forest-quest_2")).toBe(true);
		expect(re.test("Forest")).toBe(false);
	});
	it("marks the field at fault", async () => {
		const bad = await (await create({ name: "Bad", display_name: "X" })).text();
		expect(bad).toMatch(/name="name"[^>]*aria-invalid="true"/);
		expect(bad).not.toMatch(/name="display_name"[^>]*aria-invalid/);
		const missing = await (await create({ name: "ok" })).text();
		expect(missing).toMatch(/name="display_name"[^>]*aria-invalid="true"/);
	});
	it("escapes what it prints", async () => {
		await create({ name: "x", display_name: "<script>alert(1)</script>" });
		const page = await (await get()).text();
		expect(page).not.toContain("<script>alert");
		expect(page).toContain("&lt;script&gt;");
	});
});

describe("app creation", () => {
	it("stores the row and redirects to the list", async () => {
		const r = await create({ name: "forest-quest", display_name: " Forest Quest " });
		expect(r.status).toBe(303);
		expect(r.headers.get("Location")).toBe("/dashboard");
		const row = await bindings.DB.prepare("SELECT * FROM apps").first();
		expect(row).toMatchObject({
			name: "forest-quest", display_name: "Forest Quest", created_by: "alice", disabled_at: null,
		});
		expect(row?.created_at).toBeGreaterThan(1_700_000_000);
	});
	it("records the email when the login has one", async () => {
		const row = await createApp(createDb(bindings.DB), { name: "a", display_name: "A" }, { sub: "s1", email: "carol@example.com" });
		expect(row?.created_by).toBe("carol@example.com");
	});
	it("rejects a name the client could not send", async () => {
		for (const name of ["", "Forest", "forest quest", "a".repeat(64), "ünïcode"]) {
			const r = await create({ name, display_name: "X" });
			expect(r.status, name).toBe(400);
			expect(await r.text()).toContain("lowercase letters");
		}
		expect(await bindings.DB.prepare("SELECT count(*) AS n FROM apps").first("n")).toBe(0);
	});
	it("rejects a missing display name", async () => {
		const r = await create({ name: "ok" });
		expect(r.status).toBe(400);
		expect(await r.text()).toContain("display name is required");
	});
	it("refuses a duplicate name", async () => {
		await create({ name: "forest-quest", display_name: "One" });
		const r = await create({ name: "forest-quest", display_name: "Two" });
		expect(r.status).toBe(409);
		expect(await r.text()).toContain("already exists");
		expect(await bindings.DB.prepare("SELECT count(*) AS n FROM apps").first("n")).toBe(1);
	});
	it("requires a login", async () => {
		const r = await worker.fetch(
			new Request("https://dash.example/dashboard/apps", { method: "POST", body: new URLSearchParams({ name: "x", display_name: "X" }) }),
			env
		);
		expect(r.status).toBe(401);
	});
});

describe("schema", () => {
	it("refuses a bad name even when written directly", async () => {
		const insert = bindings.DB.prepare(
			"INSERT INTO apps (name, display_name, created_at, created_by) VALUES (?1, 'X', 0, 'sql')"
		);
		await expect(insert.bind("Bad Name").run()).rejects.toThrow(/CHECK/);
		await expect(insert.bind("fine_name-1").run()).resolves.toBeTruthy();
	});
});
