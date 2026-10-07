import { env as bindings } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import { originAllowed, parseCorsOrigins } from "../src/cors";
import worker from "../src/index";

const env = { DB: bindings.DB, SHARD: bindings.SHARD, BUCKET: bindings.BUCKET };

async function addApp(name: string, origins: string | null): Promise<void> {
	await bindings.DB.prepare(
		"INSERT INTO apps (name, display_name, created_at, created_by, cors_origins) VALUES (?1, ?1, 1, 'bob', ?2)"
	)
		.bind(name, origins)
		.run();
}

async function preflight(path: string, origin: string): Promise<Response> {
	return worker.fetch(
		new Request(`https://api.example/v1/${path}`, {
			method: "OPTIONS",
			headers: {
				Origin: origin,
				"Access-Control-Request-Method": "POST",
				"Access-Control-Request-Headers": "content-type, authorization",
			},
		}),
		env
	);
}

async function post(path: string, origin?: string): Promise<Response> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (origin !== undefined) headers.Origin = origin;
	return worker.fetch(new Request(`https://api.example/v1/${path}`, { method: "POST", headers, body: "{}" }), env);
}

describe("the CORS setting", () => {
	it("parses to null, '*' or lower-case lines", () => {
		expect(parseCorsOrigins("")).toEqual({ origins: null });
		expect(parseCorsOrigins(" \n\r\n")).toEqual({ origins: null });
		expect(parseCorsOrigins("*")).toEqual({ origins: "*" });
		expect(parseCorsOrigins("https://a.example\n*\n")).toEqual({ origins: "*" });
		expect(parseCorsOrigins(" HTTPS://Game.Example \r\n\r\nhttp://localhost:*\nhttps://*.itch.io\n*://[::1]:8080")).toEqual({
			origins: "https://game.example\nhttp://localhost:*\nhttps://*.itch.io\n*://[::1]:8080",
		});
	});
	it("refuses what is not an origin", () => {
		for (const bad of ["game.example", "https://game.example/", "https://game.example/path", "https://a b", "https://", "*.example"]) {
			expect(parseCorsOrigins(bad)).toEqual({ error: expect.stringMatching(/is not an origin/) });
		}
		expect(parseCorsOrigins(`https://${"a".repeat(2000)}`)).toEqual({ error: expect.stringMatching(/at most 2000/) });
	});
	it("matches an origin against the setting", () => {
		expect(originAllowed(null, "https://game.example")).toBe(false);
		expect(originAllowed("*", "https://game.example")).toBe(true);
		expect(originAllowed("*", "")).toBe(false);
		const setting = "https://game.example\nhttp://localhost:*\nhttps://*.itch.io";
		expect(originAllowed(setting, "https://game.example")).toBe(true);
		expect(originAllowed(setting, "HTTPS://GAME.EXAMPLE")).toBe(true);
		expect(originAllowed(setting, "https://game.example.evil")).toBe(false);
		expect(originAllowed(setting, "http://game.example")).toBe(false);
		expect(originAllowed(setting, "http://localhost:8000")).toBe(true);
		expect(originAllowed(setting, "http://localhost")).toBe(false);
		expect(originAllowed(setting, "https://studio.itch.io")).toBe(true);
		expect(originAllowed(setting, "https://itch.io")).toBe(false);
		expect(originAllowed(setting, "https://studio.itch.io.evil")).toBe(false);
		// A dot in the pattern is a dot, not any character.
		expect(originAllowed("https://game.example", "https://gameXexample")).toBe(false);
	});
});

describe("ingest CORS", () => {
	it("answers a preflight for an allowed origin and marks the reply", async () => {
		await addApp("cors-list", "https://game.example\nhttps://*.itch.io");
		for (const path of ["cors-list/report", "cors-list/attach?report=x&name=y"]) {
			const r = await preflight(path, "https://studio.itch.io");
			expect(r.status).toBe(204);
			expect(r.headers.get("Access-Control-Allow-Origin")).toBe("https://studio.itch.io");
			expect(r.headers.get("Access-Control-Allow-Methods")).toBe("POST");
			expect(r.headers.get("Access-Control-Allow-Headers")).toBe("content-type,authorization");
			expect(r.headers.get("Access-Control-Max-Age")).toBe("86400");
			expect(r.headers.get("Vary")).toContain("Origin");
		}
		const sent = await post("cors-list/report", "https://game.example");
		expect(sent.status).toBe(400);
		expect(sent.headers.get("Access-Control-Allow-Origin")).toBe("https://game.example");
		expect(sent.headers.get("Vary")).toBe("Origin");
	});
	it("allows every origin with '*', echoing the origin", async () => {
		await addApp("cors-all", "*");
		const r = await preflight("cors-all/report", "https://anyone.example");
		expect(r.status).toBe(204);
		expect(r.headers.get("Access-Control-Allow-Origin")).toBe("https://anyone.example");
	});
	it("gives a disallowed origin, an unset app and an unknown app no allow header", async () => {
		await addApp("cors-list2", "https://game.example");
		await addApp("cors-none", null);
		for (const [path, origin] of [
			["cors-list2/report", "https://other.example"],
			["cors-none/report", "https://game.example"],
			["nobody/report", "https://game.example"],
		]) {
			const r = await preflight(path!, origin!);
			expect(r.status).toBe(204);
			expect(r.headers.get("Access-Control-Allow-Origin")).toBeNull();
			const sent = await post(path!, origin!);
			expect(sent.headers.get("Access-Control-Allow-Origin")).toBeNull();
		}
	});
	it("leaves a request without an origin alone", async () => {
		await addApp("cors-native", "*");
		const r = await post("cors-native/report");
		expect(r.status).toBe(400);
		expect(r.headers.get("Access-Control-Allow-Origin")).toBeNull();
	});
	it("does not cover the release upload", async () => {
		await addApp("cors-upload", "*");
		const r = await preflight("cors-upload/releases/1.0.0?channel=stable", "https://game.example");
		expect(r.status).toBe(404);
		expect(r.headers.get("Access-Control-Allow-Origin")).toBeNull();
	});
});
