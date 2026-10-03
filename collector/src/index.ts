import { accessKeys, verifyAccess } from "./access";
import { basicChallenge, MIN_PASSWORD_LENGTH, verifyBasic } from "./basic";
import type { JWTVerifyGetKey } from "jose";

interface Env {
	ACCESS_TEAM_DOMAIN?: string;
	ACCESS_AUD?: string;
	DASHBOARD_PASSWORD?: string;
}

/** Who is using the dashboard, whichever login mode admitted them. */
interface Identity {
	sub: string;
	email?: string;
}

let keys: JWTVerifyGetKey | undefined;

function accessConfigured(env: Env): boolean {
	return Boolean(env.ACCESS_TEAM_DOMAIN && env.ACCESS_AUD);
}

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

/**
 * Identifies the caller, or returns the response that turns them away.
 * Cloudflare Access is used when its two secrets are set; otherwise a
 * dashboard password enables Basic authentication; otherwise nobody gets in.
 */
async function authenticate(request: Request, env: Env): Promise<Identity | Response> {
	if (env.ACCESS_TEAM_DOMAIN && env.ACCESS_AUD) {
		keys ??= accessKeys(env.ACCESS_TEAM_DOMAIN);
		const who = await verifyAccess(
			request, { teamDomain: env.ACCESS_TEAM_DOMAIN, aud: env.ACCESS_AUD }, keys
		);
		return who ?? new Response("Unauthorized", { status: 401 });
	}
	if (env.DASHBOARD_PASSWORD) {
		if (env.DASHBOARD_PASSWORD.length < MIN_PASSWORD_LENGTH) {
			return new Response("The dashboard password is too short", { status: 503 });
		}
		const who = await verifyBasic(request, env.DASHBOARD_PASSWORD);
		return who ?? basicChallenge();
	}
	return new Response("Dashboard login is not configured", { status: 503 });
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url);
		if (url.pathname.startsWith("/v1/")) {
			return new Response("Not found", { status: 404 });
		}
		if (url.pathname === "/dev/login" && accessConfigured(env)) {
			return devLogin(url);
		}
		const who = await authenticate(request, env);
		if (who instanceof Response) {
			return who;
		}
		return new Response(`Hello ${who.email ?? who.sub}`, {
			headers: { "content-type": "text/plain; charset=utf-8" },
		});
	},
} satisfies ExportedHandler<Env>;
