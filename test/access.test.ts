import { createLocalJWKSet, exportJWK, generateKeyPair, SignJWT, type JWTVerifyGetKey } from "jose";
import { beforeAll, describe, expect, it } from "vitest";
import { accessToken, verifyAccess, type AccessConfig } from "../src/access";

const cfg: AccessConfig = { teamDomain: "team.cloudflareaccess.com", aud: "aud-1" };

let keys: JWTVerifyGetKey;
let sign: (claims: Record<string, unknown>, opts?: { iss?: string; aud?: string; exp?: string }) => Promise<string>;
let signOther: () => Promise<string>;

beforeAll(async () => {
	const pair = await generateKeyPair("RS256", { extractable: true });
	const jwk = { ...(await exportJWK(pair.publicKey)), kid: "k1", alg: "RS256", use: "sig" };
	keys = createLocalJWKSet({ keys: [jwk] });
	sign = (claims, opts = {}) =>
		new SignJWT(claims)
			.setProtectedHeader({ alg: "RS256", kid: "k1" })
			.setIssuer(opts.iss ?? `https://${cfg.teamDomain}`)
			.setAudience(opts.aud ?? cfg.aud)
			.setIssuedAt()
			.setExpirationTime(opts.exp ?? "1h")
			.sign(pair.privateKey);
	const other = await generateKeyPair("RS256");
	signOther = () =>
		new SignJWT({ sub: "u1" })
			.setProtectedHeader({ alg: "RS256", kid: "k1" })
			.setIssuer(`https://${cfg.teamDomain}`)
			.setAudience(cfg.aud)
			.setIssuedAt()
			.setExpirationTime("1h")
			.sign(other.privateKey);
});

function withHeader(token: string): Request {
	return new Request("https://dash.example/", { headers: { "Cf-Access-Jwt-Assertion": token } });
}

function withCookie(token: string): Request {
	return new Request("https://dash.example/", { headers: { Cookie: `a=b; CF_Authorization=${token}; c=d` } });
}

describe("accessToken", () => {
	it("prefers the header", () => {
		const r = new Request("https://dash.example/", {
			headers: { "Cf-Access-Jwt-Assertion": "h", Cookie: "CF_Authorization=c" },
		});
		expect(accessToken(r)).toBe("h");
	});
	it("falls back to the cookie", () => {
		expect(accessToken(withCookie("c"))).toBe("c");
	});
	it("is null without either", () => {
		expect(accessToken(new Request("https://dash.example/"))).toBeNull();
	});
});

describe("verifyAccess", () => {
	it("accepts a valid token in the header", async () => {
		const token = await sign({ sub: "u1", email: "a@example.com" });
		expect(await verifyAccess(withHeader(token), cfg, keys)).toEqual({ sub: "u1", email: "a@example.com" });
	});
	it("accepts a valid token in the cookie", async () => {
		const token = await sign({ sub: "svc" });
		expect(await verifyAccess(withCookie(token), cfg, keys)).toEqual({ sub: "svc", email: undefined });
	});
	it("rejects a missing token", async () => {
		expect(await verifyAccess(new Request("https://dash.example/"), cfg, keys)).toBeNull();
	});
	it("rejects an expired token", async () => {
		const token = await sign({ sub: "u1" }, { exp: "-1h" });
		expect(await verifyAccess(withHeader(token), cfg, keys)).toBeNull();
	});
	it("rejects another application's audience", async () => {
		const token = await sign({ sub: "u1" }, { aud: "aud-2" });
		expect(await verifyAccess(withHeader(token), cfg, keys)).toBeNull();
	});
	it("rejects another team's issuer", async () => {
		const token = await sign({ sub: "u1" }, { iss: "https://other.cloudflareaccess.com" });
		expect(await verifyAccess(withHeader(token), cfg, keys)).toBeNull();
	});
	it("rejects a token signed by an unknown key", async () => {
		expect(await verifyAccess(withHeader(await signOther()), cfg, keys)).toBeNull();
	});
	it("rejects a token without a subject", async () => {
		const token = await sign({ email: "a@example.com" });
		expect(await verifyAccess(withHeader(token), cfg, keys)).toBeNull();
	});
	it("rejects garbage", async () => {
		expect(await verifyAccess(withHeader("not.a.jwt"), cfg, keys)).toBeNull();
	});
});
