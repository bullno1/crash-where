import { accessKeys, verifyAccess } from "./access";
import type { JWTVerifyGetKey } from "jose";

interface Env {
	ACCESS_TEAM_DOMAIN: string;
	ACCESS_AUD: string;
}

let keys: JWTVerifyGetKey | undefined;

/**
 * Stores a token obtained with `cloudflared access token` as the cookie
 * Access itself would set, so a browser can use a local `wrangler dev`.
 * Answers only on localhost; the token is still verified on every request.
 */
function devLogin(url: URL): Response {
	if (url.hostname !== "localhost" && url.hostname !== "127.0.0.1") {
		return new Response("Not found", { status: 404 });
	}
	const token = url.searchParams.get("token");
	if (!token) {
		return new Response("Missing token", { status: 400 });
	}
	return new Response(null, {
		status: 303,
		headers: {
			Location: "/",
			"Set-Cookie": `CF_Authorization=${token}; Path=/; HttpOnly; SameSite=Lax`,
		},
	});
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url);
		if (url.pathname.startsWith("/v1/")) {
			return new Response("Not found", { status: 404 });
		}
		if (url.pathname === "/dev/login") {
			return devLogin(url);
		}
		if (!env.ACCESS_TEAM_DOMAIN || !env.ACCESS_AUD) {
			return new Response("Access is not configured", { status: 503 });
		}
		keys ??= accessKeys(env.ACCESS_TEAM_DOMAIN);
		const who = await verifyAccess(
			request, { teamDomain: env.ACCESS_TEAM_DOMAIN, aud: env.ACCESS_AUD }, keys
		);
		if (!who) {
			return new Response("Unauthorized", { status: 401 });
		}
		return new Response(`Hello ${who.email ?? who.sub}`, {
			headers: { "content-type": "text/plain; charset=utf-8" },
		});
	},
} satisfies ExportedHandler<Env>;
