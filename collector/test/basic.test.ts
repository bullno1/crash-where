import { describe, expect, it } from "vitest";
import worker from "../src/index";

const password = "correct horse battery staple";
const env = { DASHBOARD_PASSWORD: password };

function withBasic(user: string, pass: string): Request {
	const token = btoa(String.fromCharCode(...new TextEncoder().encode(`${user}:${pass}`)));
	return new Request("https://dash.example/dashboard", { headers: { Authorization: `Basic ${token}` } });
}

describe("password login", () => {
	it("challenges a request without credentials", async () => {
		const r = await worker.fetch(new Request("https://dash.example/dashboard"), env);
		expect(r.status).toBe(401);
		expect(r.headers.get("WWW-Authenticate")).toMatch(/^Basic /);
	});
	it("rejects a wrong password", async () => {
		const r = await worker.fetch(withBasic("alice", "wrong"), env);
		expect(r.status).toBe(401);
		expect(r.headers.get("WWW-Authenticate")).toMatch(/^Basic /);
	});
	it("rejects a prefix of the password", async () => {
		expect((await worker.fetch(withBasic("alice", password.slice(0, -1)), env)).status).toBe(401);
	});
	it("admits the right password and names the user", async () => {
		const r = await worker.fetch(withBasic("alice", password), env);
		expect(r.status).toBe(200);
		expect(await r.text()).toBe("Hello alice");
	});
	it("calls an empty user admin", async () => {
		expect(await (await worker.fetch(withBasic("", password), env)).text()).toBe("Hello admin");
	});
	it("handles a non-ASCII password", async () => {
		const pw = "pässwörd mit Umlauten!";
		expect((await worker.fetch(withBasic("a", pw), { DASHBOARD_PASSWORD: pw })).status).toBe(200);
		expect((await worker.fetch(withBasic("a", pw.toUpperCase()), { DASHBOARD_PASSWORD: pw })).status).toBe(401);
	});
	it("rejects malformed credentials", async () => {
		const r = await worker.fetch(
			new Request("https://dash.example/dashboard", { headers: { Authorization: "Basic not*base64" } }), env
		);
		expect(r.status).toBe(401);
	});
	it("refuses to run with a short password", async () => {
		const r = await worker.fetch(withBasic("a", "short"), { DASHBOARD_PASSWORD: "short" });
		expect(r.status).toBe(503);
		expect(r.headers.get("WWW-Authenticate")).toBeNull();
	});
	it("has no dev login route", async () => {
		expect((await worker.fetch(new Request("http://localhost:8787/dev/login?token=x"), env)).status).toBe(404);
	});
	it("guards every path under the prefix", async () => {
		const r = await worker.fetch(new Request("https://dash.example/dashboard/groups/1"), env);
		expect(r.status).toBe(401);
	});
	it("leaves the root public and sends it to the dashboard", async () => {
		const r = await worker.fetch(new Request("https://dash.example/"), env);
		expect(r.status).toBe(302);
		expect(r.headers.get("Location")).toBe("/dashboard");
	});
	it("leaves the API prefix outside the login", async () => {
		const r = await worker.fetch(new Request("https://dash.example/v1/report"), env);
		expect(r.status).toBe(404);
		expect(r.headers.get("WWW-Authenticate")).toBeNull();
	});
});

describe("mode selection", () => {
	it("refuses everything when nothing is configured", async () => {
		const r = await worker.fetch(withBasic("alice", password), {});
		expect(r.status).toBe(503);
		expect(r.headers.get("WWW-Authenticate")).toBeNull();
	});
	it("prefers Access over the password when both are set", async () => {
		const both = { ...env, ACCESS_TEAM_DOMAIN: "team.cloudflareaccess.com", ACCESS_AUD: "aud-1" };
		const r = await worker.fetch(withBasic("alice", password), both);
		expect(r.status).toBe(401);
		expect(r.headers.get("WWW-Authenticate")).toBeNull();
	});
});
