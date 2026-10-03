import { env as bindings } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { createApp } from "../src/apps";
import { createDb } from "../src/db";
import worker from "../src/index";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;
const origin = "https://dash.example";

async function get(): Promise<Response> {
	return worker.fetch(new Request("https://dash.example/dashboard", { headers: { Authorization: auth } }), env);
}

/** Posts the form the way a browser does, with this origin's `Origin` header unless `headers` says otherwise. */
async function post(fields: Record<string, string>, headers: Record<string, string>): Promise<Response> {
	return worker.fetch(
		new Request("https://dash.example/dashboard/apps", {
			method: "POST",
			headers: { "Content-Type": "application/x-www-form-urlencoded", ...headers },
			body: new URLSearchParams(fields),
		}),
		env
	);
}

async function create(fields: Record<string, string>): Promise<Response> {
	return post(fields, { Authorization: auth, Origin: origin });
}

/** Submits the form and follows the redirect, as a browser would; returns the page it lands on. */
async function submit(fields: Record<string, string>): Promise<{ location: string; page: string }> {
	const r = await create(fields);
	expect(r.status).toBe(303);
	const location = r.headers.get("Location")!;
	const landed = await worker.fetch(new Request(`https://dash.example${location}`, { headers: { Authorization: auth } }), env);
	expect(landed.status).toBe(200);
	return { location, page: await landed.text() };
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
		const { location, page } = await submit({ name: "forest-quest", display_name: "Forest Quest" });
		expect(location).toBe("/dashboard");
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
		expect(css.headers.get("Vary")).toBeNull();
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
	it("marks the field at fault and keeps the typed values", async () => {
		const bad = (await submit({ name: "Bad", display_name: "X" })).page;
		expect(bad).toMatch(/name="name"[^>]*value="Bad"[^>]*aria-invalid="true"/);
		expect(bad).toMatch(/name="display_name"[^>]*value="X"/);
		expect(bad).not.toMatch(/name="display_name"[^>]*aria-invalid/);
		const missing = (await submit({ name: "ok" })).page;
		expect(missing).toMatch(/name="display_name"[^>]*aria-invalid="true"/);
	});
	it("shows a plain form on a visit without a submission", async () => {
		const page = await (await get()).text();
		expect(page).not.toContain("aria-invalid");
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
			const { location, page } = await submit({ name, display_name: "X" });
			expect(location, name).toMatch(/^\/dashboard\?/);
			expect(page, name).toContain("lowercase letters");
		}
		expect(await bindings.DB.prepare("SELECT count(*) AS n FROM apps").first("n")).toBe(0);
	});
	it("rejects a missing display name", async () => {
		const { page } = await submit({ name: "ok" });
		expect(page).toContain("display name is required");
	});
	it("refuses a duplicate name", async () => {
		await create({ name: "forest-quest", display_name: "One" });
		const { page } = await submit({ name: "forest-quest", display_name: "Two" });
		expect(page).toContain("already exists");
		expect(page).toMatch(/name="name"[^>]*aria-invalid="true"/);
		expect(await bindings.DB.prepare("SELECT count(*) AS n FROM apps").first("n")).toBe(1);
	});
	it("requires a login", async () => {
		const r = await post({ name: "x", display_name: "X" }, { Origin: origin });
		expect(r.status).toBe(401);
	});
});

describe("cross-site protection", () => {
	const fields = { name: "forged", display_name: "Forged" };
	async function count(): Promise<unknown> {
		return bindings.DB.prepare("SELECT count(*) AS n FROM apps").first("n");
	}
	it("refuses a form posted from another origin, even with credentials", async () => {
		const r = await post(fields, { Authorization: auth, Origin: "https://evil.example" });
		expect(r.status).toBe(403);
		expect(await count()).toBe(0);
	});
	it("refuses a form that names neither origin nor fetch site", async () => {
		const r = await post(fields, { Authorization: auth });
		expect(r.status).toBe(403);
		expect(await count()).toBe(0);
	});
	it("accepts a same-origin fetch site without an Origin header", async () => {
		const r = await post(fields, { Authorization: auth, "Sec-Fetch-Site": "same-origin" });
		expect(r.status).toBe(303);
		expect(await count()).toBe(1);
	});
	it("does not get in the way of reading the dashboard", async () => {
		expect((await get()).status).toBe(200);
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

describe("apps as JSON", () => {
	const json = { Authorization: auth, Accept: "application/json" };
	it("lists the rows", async () => {
		await createApp(createDb(bindings.DB), { name: "json-list", display_name: "JSON List" }, { sub: "bob" });
		const r = await worker.fetch(new Request("https://dash.example/dashboard", { headers: json }), env);
		expect(r.status).toBe(200);
		const data = await r.json() as { apps: { name: string }[] };
		expect(data.apps.map((a) => a.name)).toEqual(["json-list"]);
	});
	it("marks every negotiated reply as varying by Accept", async () => {
		const page = await get();
		expect(page.headers.get("Vary")).toBe("Accept");
		const created = await post({ name: "json-vary", display_name: "Vary" }, { ...json, Origin: origin });
		expect(created.status).toBe(201);
		expect(created.headers.get("Vary")).toBe("Accept");
		const redirected = await create({ name: "form-vary", display_name: "Vary" });
		expect(redirected.status).toBe(303);
		expect(redirected.headers.get("Vary")).toBe("Accept");
	});
	it("creates from a JSON body without an origin and answers with the row", async () => {
		const r = await worker.fetch(
			new Request("https://dash.example/dashboard/apps", {
				method: "POST",
				headers: { ...json, "Content-Type": "application/json" },
				body: JSON.stringify({ name: "json-made", display_name: " JSON Made " }),
			}),
			env
		);
		expect(r.status).toBe(201);
		expect(await r.json()).toMatchObject({ app: { name: "json-made", display_name: "JSON Made", created_by: "alice" } });
	});
	it("names the field at fault, and a taken name", async () => {
		const bad = await post({ name: "Bad", display_name: "X" }, { ...json, Origin: origin });
		expect(bad.status).toBe(400);
		expect(await bad.json()).toEqual({ error: { field: "name", message: expect.stringMatching(/^The name must be/) } });
		await createApp(createDb(bindings.DB), { name: "json-taken", display_name: "Taken" }, { sub: "bob" });
		const taken = await post({ name: "json-taken", display_name: "Again" }, { ...json, Origin: origin });
		expect(taken.status).toBe(409);
		expect(await taken.json()).toEqual({ error: { field: "name", message: "An app named 'json-taken' already exists." } });
	});
});
