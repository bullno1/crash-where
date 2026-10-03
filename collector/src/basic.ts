/** Shortest dashboard password the Worker accepts; shorter ones disable login. */
export const MIN_PASSWORD_LENGTH = 16;

/** Identity behind a correct password: the name typed at the prompt. */
export interface BasicIdentity {
	sub: string;
}

async function sha256(text: string): Promise<Uint8Array> {
	const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
	return new Uint8Array(digest);
}

/** Compares two strings in time that does not depend on where they differ. */
async function equal(a: string, b: string): Promise<boolean> {
	const [x, y] = await Promise.all([sha256(a), sha256(b)]);
	let diff = 0;
	for (let i = 0; i < x.length; i++) diff |= x[i] ^ y[i];
	return diff === 0;
}

/**
 * Checks the request's Basic credentials against the dashboard password
 * and returns the identity, or null when they are missing or wrong.
 * Any user name is accepted; "admin" stands in for an empty one.
 */
export async function verifyBasic(
	request: Request, password: string
): Promise<BasicIdentity | null> {
	const header = request.headers.get("Authorization");
	if (!header || !header.startsWith("Basic ")) return null;
	let decoded: string;
	try {
		const bytes = Uint8Array.from(atob(header.slice(6).trim()), (c) => c.charCodeAt(0));
		decoded = new TextDecoder().decode(bytes);
	} catch {
		return null;
	}
	const colon = decoded.indexOf(":");
	if (colon < 0) return null;
	if (!(await equal(decoded.slice(colon + 1), password))) return null;
	return { sub: decoded.slice(0, colon) || "admin" };
}

/** The 401 that makes a browser show its login prompt. */
export function basicChallenge(): Response {
	return new Response("Unauthorized", {
		status: 401,
		headers: { "WWW-Authenticate": 'Basic realm="crash-where", charset="UTF-8"' },
	});
}
