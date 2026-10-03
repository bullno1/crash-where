import { createRemoteJWKSet, jwtVerify, type JWTVerifyGetKey } from "jose";

/** Who Cloudflare Access let through. */
export interface AccessIdentity {
	/** Subject claim, stable per user or service token. */
	sub: string;
	/** Email of a logged-in user. Service tokens have none. */
	email?: string;
}

export interface AccessConfig {
	/** Team domain, for example `example.cloudflareaccess.com`. */
	teamDomain: string;
	/** AUD tag of the Access application. */
	aud: string;
}

/** Key set that Access signs with. Fetched once per isolate and cached by `jose`. */
export function accessKeys(teamDomain: string): JWTVerifyGetKey {
	return createRemoteJWKSet(new URL(`https://${teamDomain}/cdn-cgi/access/certs`));
}

/**
 * The Access JWT carried by a request, or null when there is none.
 * The edge adds the header; the cookie fallback lets a token obtained
 * with `cloudflared access token` be pasted into a browser against a
 * local `wrangler dev`.
 */
export function accessToken(request: Request): string | null {
	const header = request.headers.get("Cf-Access-Jwt-Assertion");
	if (header) return header;
	const cookie = request.headers.get("Cookie");
	if (!cookie) return null;
	for (const part of cookie.split(";")) {
		const [name, ...rest] = part.trim().split("=");
		if (name === "CF_Authorization") return rest.join("=");
	}
	return null;
}

/**
 * Verifies the request's Access JWT and returns the identity it carries,
 * or null when the token is missing, expired, signed by an unknown key,
 * or issued for another team or application.
 */
export async function verifyAccess(
	request: Request, cfg: AccessConfig, keys: JWTVerifyGetKey
): Promise<AccessIdentity | null> {
	const token = accessToken(request);
	if (!token) return null;
	try {
		const { payload } = await jwtVerify(token, keys, {
			issuer: `https://${cfg.teamDomain}`,
			audience: cfg.aud,
			algorithms: ["RS256"],
		});
		if (typeof payload.sub !== "string") return null;
		const email = typeof payload.email === "string" ? payload.email : undefined;
		return { sub: payload.sub, email };
	} catch {
		return null;
	}
}
