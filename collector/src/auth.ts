import { createMiddleware } from "hono/factory";
import type { JWTVerifyGetKey } from "jose";
import { accessKeys, verifyAccess } from "./access";
import { basicChallenge, MIN_PASSWORD_LENGTH, verifyBasic } from "./basic";
import type { App, Env, Identity } from "./env";

let keys: JWTVerifyGetKey | undefined;

/**
 * Identifies the caller, or returns the response that turns them away.
 * Cloudflare Access is used when its two secrets are set; otherwise a
 * dashboard password enables Basic authentication; otherwise nobody gets in.
 */
export async function authenticate(request: Request, env: Env): Promise<Identity | Response> {
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

/** Middleware that admits only an identified caller and stores the identity. */
export const requireLogin = createMiddleware<App>(async (c, next) => {
	const who = await authenticate(c.req.raw, c.env);
	if (who instanceof Response) return who;
	c.set("identity", who);
	await next();
});
