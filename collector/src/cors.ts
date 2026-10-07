/**
 * Which pages may send cross-origin requests to an app's ingest routes.
 * The setting is `*` for every origin, or one origin pattern per line, a
 * scheme, a host and an optional port in which `*` matches any run of
 * characters; null allows none. A browser serializes an origin in lower
 * case, so patterns are stored that way and matched case-insensitively.
 */

/** The setting that allows every origin. */
export const ALL_ORIGINS = "*";

/** Longest setting accepted, as the lines joined by newlines. */
export const MAX_CORS_ORIGINS = 2000;

/** What a line must look like, with `*` allowed anywhere in the scheme, host or port; an IPv6 host sits in brackets. */
const PATTERN = /^[a-z0-9+.*-]+:\/\/(?:[a-z0-9.*-]+|\[[0-9a-f:.*]+\])(?::[0-9*]+)?$/;

/** The lines of a typed setting, trimmed, blank ones dropped, in lower case. */
export function corsLines(text: string): string[] {
	return text
		.split(/\r?\n/)
		.map((line) => line.trim().toLowerCase())
		.filter((line) => line !== "");
}

/**
 * The stored form of a typed setting, or the reason it is unusable. Empty
 * text stores null; a line that is just `*` means every origin.
 */
export function parseCorsOrigins(text: string): { origins: string | null } | { error: string } {
	if (text.trim() === ALL_ORIGINS) return { origins: ALL_ORIGINS };
	const lines = corsLines(text);
	if (lines.length === 0) return { origins: null };
	if (lines.includes(ALL_ORIGINS)) return { origins: ALL_ORIGINS };
	const joined = lines.join("\n");
	if (joined.length > MAX_CORS_ORIGINS) return { error: `The origins must be at most ${MAX_CORS_ORIGINS} characters.` };
	const bad = lines.find((line) => !PATTERN.test(line));
	if (bad !== undefined) return { error: `'${bad}' is not an origin: write scheme://host[:port], with '*' as a wildcard.` };
	return { origins: joined };
}

function escapeRegExp(s: string): string {
	return s.replace(/[.+?^${}()|[\]\\]/g, "\\$&");
}

/** Whether the setting allows a request from this origin. */
export function originAllowed(setting: string | null, origin: string): boolean {
	if (setting === null || origin === "") return false;
	if (setting === ALL_ORIGINS) return true;
	const wanted = origin.toLowerCase();
	return setting.split("\n").some((line) => {
		const re = new RegExp(`^${line.split("*").map(escapeRegExp).join(".*")}$`);
		return re.test(wanted);
	});
}
