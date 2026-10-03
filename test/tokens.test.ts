import { env as bindings } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { createDb } from "../src/db";
import worker from "../src/index";
import { authenticateToken, hashToken } from "../src/tokens";

const password = "correct horse battery staple";
const env = { DB: bindings.DB, SHARD: bindings.SHARD, SYMBOLS: bindings.SYMBOLS, DASHBOARD_PASSWORD: password };
const auth = `Basic ${btoa(`alice:${password}`)}`;
const origin = "https://dash.example";
const db = createDb(bindings.DB);

async function get(path: string, cookie?: string): Promise<Response> {
	const headers: Record<string, string> = { Authorization: auth };
	if (cookie !== undefined) headers.Cookie = cookie;
	return worker.fetch(new Request(`${origin}${path}`, { headers }), env);
}

/** Creates a token through the form, follows the redirect as a browser would, and returns the page and the token. */
async function mint(app: string): Promise<{ page: string; token: string; cookie: string }> {
	const r = await post(`/dashboard/apps/${app}/tokens`);
	expect(r.status).toBe(303);
	expect(r.headers.get("Location")).toBe(`/dashboard/apps/${app}`);
	const set = r.headers.get("Set-Cookie")!;
	expect(set).toMatch(new RegExp(`^cw_new_token=cwu_[A-Za-z0-9_-]{43}; Max-Age=60; Path=/dashboard/apps/${app}; HttpOnly; Secure; SameSite=Strict$`));
	const cookie = set.split(";")[0]!;
	const landed = await get(`/dashboard/apps/${app}`, cookie);
	expect(landed.status).toBe(200);
	return { page: await landed.text(), token: cookie.slice("cw_new_token=".length), cookie };
}

async function post(
	path: string, headers: Record<string, string> = { Authorization: auth, Origin: origin },
	fields: Record<string, string> = { label: "GitHub Actions" }
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

async function addApp(name: string): Promise<void> {
	await bindings.DB.prepare("INSERT INTO apps (name, display_name, created_at, created_by) VALUES (?1, ?1, 1, 'bob')")
		.bind(name)
		.run();
}

beforeEach(async () => {
	await bindings.DB.exec("DELETE FROM upload_tokens");
	await bindings.DB.exec("DELETE FROM apps");
});

describe("upload tokens", () => {
	it("are absent on a new app", async () => {
		await addApp("tok-empty");
		expect(await (await get("/dashboard/apps/tok-empty")).text()).toContain("No upload tokens yet");
	});
	it("are shown once after the redirect, then listed by their label", async () => {
		await addApp("tok-new");
		const { page, token } = await mint("tok-new");
		expect(page).toContain(`<code>${token}</code>`);
		expect(page).toContain("shown only this once");
		expect(page).toContain("<td>GitHub Actions</td>");
		expect(page).toContain("<td>alice</td>");
		const again = await (await get("/dashboard/apps/tok-new")).text();
		expect(again).not.toContain(token);
		expect(again).toContain("<td>GitHub Actions</td>");
		const row = await bindings.DB.prepare("SELECT hash FROM upload_tokens").first<{ hash: string }>();
		expect(row!.hash).toBe(await hashToken(token));
		expect(await authenticateToken(db, token, 5)).not.toBeNull();
	});
	it("need a label", async () => {
		await addApp("tok-label");
		const headers = { Authorization: auth, Origin: origin };
		expect((await post("/dashboard/apps/tok-label/tokens", headers, { label: "  " })).status).toBe(400);
		expect((await post("/dashboard/apps/tok-label/tokens", headers, { label: "x".repeat(101) })).status).toBe(400);
		expect((await post("/dashboard/apps/tok-label/tokens", headers, {})).status).toBe(400);
		expect(await bindings.DB.prepare("SELECT count(*) AS n FROM upload_tokens").first<{ n: number }>()).toEqual({ n: 0 });
		expect((await post("/dashboard/apps/tok-label/tokens", headers, { label: " <nightly> " })).status).toBe(303);
		expect(await (await get("/dashboard/apps/tok-label")).text()).toContain("<td>&lt;nightly&gt;</td>");
	});
	it("clear the cookie that carried them on the page that shows them", async () => {
		await addApp("tok-cookie");
		const r = await post("/dashboard/apps/tok-cookie/tokens");
		const cookie = r.headers.get("Set-Cookie")!.split(";")[0]!;
		const landed = await get("/dashboard/apps/tok-cookie", cookie);
		expect(landed.headers.get("Set-Cookie")).toMatch(/^cw_new_token=; Max-Age=0; Path=\/dashboard\/apps\/tok-cookie; Secure/);
		expect((await get("/dashboard/apps/tok-cookie")).headers.get("Set-Cookie")).toBeNull();
	});
	it("can be revoked, after which they no longer authenticate", async () => {
		await addApp("tok-revoke");
		const { page, token } = await mint("tok-revoke");
		const id = /tokens\/(\d+)\/revoke/.exec(page)![1];
		const r = await post(`/dashboard/apps/tok-revoke/tokens/${id}/revoke`);
		expect(r.status).toBe(303);
		expect(r.headers.get("Location")).toBe("/dashboard/apps/tok-revoke");
		const after = await (await get("/dashboard/apps/tok-revoke")).text();
		expect(after).toContain("revoked");
		expect(after).not.toContain("Revoke</button>");
		expect(await authenticateToken(db, token, 5)).toBeNull();
	});
	it("are 404 for an unknown app and refused without a login or origin", async () => {
		expect((await post("/dashboard/apps/nobody/tokens")).status).toBe(404);
		await addApp("tok-guard");
		expect((await post("/dashboard/apps/tok-guard/tokens", { Origin: origin })).status).toBe(401);
		expect((await post("/dashboard/apps/tok-guard/tokens", { Authorization: auth })).status).toBe(403);
	});
});

describe("upload tokens as JSON", () => {
	const json = { Authorization: auth, Origin: origin, Accept: "application/json" };
	it("are returned in the reply and listed without their hash", async () => {
		await addApp("json-mint");
		const r = await post("/dashboard/apps/json-mint/tokens", json, { label: "agent" });
		expect(r.status).toBe(201);
		const made = await r.json() as { token: string; id: number; label: string };
		expect(made.token).toMatch(/^cwu_/);
		expect(made.label).toBe("agent");
		expect(await authenticateToken(db, made.token, 2)).toMatchObject({ id: made.id, label: "agent" });
		const listed = await worker.fetch(new Request(`${origin}/dashboard/apps/json-mint`, { headers: json }), env);
		const data = await listed.json() as { tokens: Record<string, unknown>[] };
		expect(data.tokens).toEqual([
			{ id: made.id, label: "agent", created_at: expect.any(Number), created_by: "alice", last_used_at: 2, revoked_at: null },
		]);
	});
	it("revoke with a status instead of a redirect", async () => {
		await addApp("json-revoke");
		const made = await (await post("/dashboard/apps/json-revoke/tokens", json)).json() as { id: number };
		const r = await post(`/dashboard/apps/json-revoke/tokens/${made.id}/revoke`, json, {});
		expect(r.status).toBe(200);
		expect(await r.json()).toEqual({ revoked: true });
		expect((await post(`/dashboard/apps/json-revoke/tokens/${made.id}/revoke`, json, {})).status).toBe(404);
	});
});
