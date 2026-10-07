import type { Context } from "hono";
import { accepts } from "hono/accepts";
import type { App } from "../env";

export function day(unix: number): string {
	return new Date(unix * 1000).toISOString().slice(0, 10);
}

/** A moment to the second, in UTC. */
export function when(unix: number): string {
	return new Date(unix * 1000).toISOString().slice(0, 19).replace("T", " ") + " UTC";
}

/**
 * Whether the client prefers JSON to a page. Every dashboard route answers
 * both from the same data: a browser, or anything that accepts either,
 * gets HTML; a script asking for `application/json` gets the object the
 * page would have been rendered from. The reply is marked as varying by
 * the header it was chosen on.
 */
export function wantsJson(c: Context<App>): boolean {
	c.header("Vary", "Accept");
	const type = accepts(c, { header: "Accept", supports: ["text/html", "application/json"], default: "text/html" });
	return type === "application/json";
}

/** The submitted fields, from a form or a JSON object, trimmed; anything that is not a string is absent. */
export async function fields(c: Context<App>): Promise<Record<string, string>> {
	const type = c.req.header("Content-Type") ?? "";
	const body: unknown = type.startsWith("application/json") ? await c.req.json() : await c.req.parseBody();
	const out: Record<string, string> = {};
	if (typeof body === "object" && body !== null) {
		for (const [key, value] of Object.entries(body)) {
			if (typeof value === "string") out[key] = value.trim();
		}
	}
	return out;
}
