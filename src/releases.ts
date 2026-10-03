/** Largest table accepted, in bytes; the body is held in memory while its header is checked. */
export const MAX_TABLE_BYTES = 64 * 1024 * 1024;

/** A version is any short string the client can be configured with; no scheme is imposed yet. */
const VERSION_PATTERN = /^[^\p{Cc}]{1,100}$/u;

export const VERSION_GRAMMAR = "1 to 100 characters without control characters";

/** A build stream name such as `stable` or `beta`. */
const CHANNEL_PATTERN = /^[A-Za-z0-9_-]{1,32}$/;

export const CHANNEL_GRAMMAR = "1 to 32 letters, digits, '-' or '_'";

export function validVersion(version: string): boolean {
	return VERSION_PATTERN.test(version);
}

export function validChannel(channel: string): boolean {
	return CHANNEL_PATTERN.test(channel);
}

/** The R2 key of a build's table. */
export function symbolKey(app: string, buildId: string): string {
	return `symbols/${app}/${buildId}.cwsym`;
}
