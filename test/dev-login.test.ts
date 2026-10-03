import { describe, expect, it } from "vitest";
import worker from "../src/index";

const env = { ACCESS_TEAM_DOMAIN: "team.cloudflareaccess.com", ACCESS_AUD: "aud-1" };

describe("/dev/login", () => {
	it("stores the token as the Access cookie and redirects", async () => {
		const r = await worker.fetch(new Request("http://localhost:8787/dev/login?token=abc"), env);
		expect(r.status).toBe(303);
		expect(r.headers.get("Location")).toBe("/");
		expect(r.headers.get("Set-Cookie")).toBe("CF_Authorization=abc; Path=/; HttpOnly; SameSite=Lax");
	});
	it("accepts 127.0.0.1", async () => {
		const r = await worker.fetch(new Request("http://127.0.0.1:8787/dev/login?token=abc"), env);
		expect(r.status).toBe(303);
	});
	it("does not exist on other hosts", async () => {
		const r = await worker.fetch(new Request("https://crash.example/dev/login?token=abc"), env);
		expect(r.status).toBe(404);
		expect(r.headers.get("Set-Cookie")).toBeNull();
	});
	it("rejects a missing token", async () => {
		const r = await worker.fetch(new Request("http://localhost:8787/dev/login"), env);
		expect(r.status).toBe(400);
	});
});
