import { type Context, Hono } from "hono";
import { getApp } from "./apps";
import { hex } from "./bytes";
import { parseHeader } from "./cwsym";
import type { App } from "./env";
import { MAX_ENVELOPE_BYTES, parseEnvelope } from "./envelope";
import {
	classify, compileSkipList, DEFAULT_SKIP_LIST, fingerprint, MAX_MESSAGE, selectFrames, STORED_FRAMES,
} from "./grouping";
import {
	CHANNEL_GRAMMAR, MAX_TABLE_BYTES, symbolKey, validChannel, validVersion, VERSION_GRAMMAR,
} from "./releases";
import { symbolicate } from "./symbols";
import { authenticateToken } from "./tokens";

/** The client API, outside the dashboard login. Replies are `key value` text lines. */
export const api = new Hono<App>();

/**
 * Registers a release and stores its symbol table, in one request from the
 * symbol tool. The table is written first and the rows second, so a failure
 * between the two leaves an object the rerun finds and skips. A rerun of an
 * upload already recorded changes nothing and still succeeds; a build id
 * that already belongs to another version, or to a table with other bytes,
 * is refused.
 */
api.put("/:app/releases/:version", async (c) => {
	const auth = c.req.header("Authorization") ?? "";
	const bearer = auth.startsWith("Bearer ") ? auth.slice("Bearer ".length).trim() : "";
	if (bearer === "") return c.text("An upload token is required", 401);
	const now = Math.floor(Date.now() / 1000);
	const db = c.get("db");
	const token = await authenticateToken(db, bearer, now);
	if (!token) return c.text("The upload token is not valid", 401);
	const app = await getApp(db, c.req.param("app"));
	if (!app) return c.text("No such app", 404);
	if (token.app_id !== app.id) return c.text("The upload token belongs to another app", 403);
	if (app.disabled_at !== null) return c.text("The app is disabled", 403);

	const version = c.req.param("version");
	if (!validVersion(version)) return c.text(`The version must be ${VERSION_GRAMMAR}`, 400);
	const channel = c.req.query("channel") ?? "";
	if (!validChannel(channel)) return c.text(`The channel must be ${CHANNEL_GRAMMAR}`, 400);

	const tooLarge = () => c.text(`The table must be at most ${MAX_TABLE_BYTES} bytes`, 413);
	if (Number(c.req.header("Content-Length") ?? 0) > MAX_TABLE_BYTES) return tooLarge();
	const body = await c.req.arrayBuffer();
	if (body.byteLength > MAX_TABLE_BYTES) return tooLarge();
	const parsed = parseHeader(new Uint8Array(body));
	if (!parsed.ok) return c.text(parsed.reason, 400);
	const { buildId } = parsed.header;

	// The etag of an object written in one put is the MD5 of its bytes.
	const key = symbolKey(app.name, buildId);
	const stored = await c.env.SYMBOLS.head(key);
	if (stored) {
		const digest = hex(new Uint8Array(await crypto.subtle.digest("MD5", body)));
		if (stored.etag !== digest) return c.text(`Build ${buildId} is already stored with a different table`, 409);
	} else {
		await c.env.SYMBOLS.put(key, body, { httpMetadata: { contentType: "application/octet-stream" } });
	}

	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const result = await shard.registerRelease({ version, channel, buildId, now });
	if (result.conflict !== undefined) {
		return c.text(`Build ${buildId} is already registered under version ${result.conflict}`, 409);
	}
	const created = [
		...(stored ? [] : ["table"]),
		...Object.entries(result.created).filter(([, yes]) => yes).map(([what]) => what),
	];
	return c.text(`build ${buildId}\ncreated ${created.length === 0 ? "none" : created.join(",")}\n`, created.length === 0 ? 200 : 201);
});

/** The skip list every app gets until the per-app one exists. */
const skipList = compileSkipList(DEFAULT_SKIP_LIST);

/**
 * The body as sent, or inflated when it came gzipped, capped either way;
 * a response says why it was refused.
 */
async function readBody(c: Context<App>): Promise<Uint8Array | Response> {
	const tooLarge = () => c.text(`The envelope must be at most ${MAX_ENVELOPE_BYTES} bytes`, 413);
	if (Number(c.req.header("Content-Length") ?? 0) > MAX_ENVELOPE_BYTES) return tooLarge();
	const raw = new Uint8Array(await c.req.arrayBuffer());
	if (raw.byteLength > MAX_ENVELOPE_BYTES) return tooLarge();
	const encoding = c.req.header("Content-Encoding")?.trim().toLowerCase();
	if (encoding === undefined || encoding === "" || encoding === "identity") return raw;
	if (encoding !== "gzip") return c.text(`Content-Encoding ${encoding} is not supported; send gzip or nothing`, 415);
	const chunks: Uint8Array[] = [];
	let total = 0;
	try {
		const reader = new Blob([raw]).stream().pipeThrough(new DecompressionStream("gzip")).getReader();
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			total += value.byteLength;
			if (total > MAX_ENVELOPE_BYTES) {
				await reader.cancel();
				return tooLarge();
			}
			chunks.push(value);
		}
	} catch {
		return c.text("The body is not valid gzip", 400);
	}
	const out = new Uint8Array(total);
	let at = 0;
	for (const chunk of chunks) {
		out.set(chunk, at);
		at += chunk.byteLength;
	}
	return out;
}

/**
 * Receives one crash envelope. The report is grouped from its frames and
 * the app's symbol tables, then counted in the app's shard against its
 * release. A token is not read yet: every report counts as unauthorized,
 * with the envelope's install id as its user. The reply is `want_attachments 0`, since samples are not stored yet;
 * a retried report id gets the same reply and is not counted again.
 */
api.post("/:app/report", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("app"));
	if (!app) return c.text("No such app", 404);
	if (app.disabled_at !== null) return c.text("The app is disabled", 403);

	const body = await readBody(c);
	if (body instanceof Response) return body;
	let json: unknown;
	try {
		json = JSON.parse(new TextDecoder().decode(body));
	} catch {
		return c.text("The envelope is not valid JSON", 400);
	}
	const parsed = parseEnvelope(json);
	if (!parsed.ok) return c.text(parsed.reason, 400);
	const { envelope } = parsed;
	if (envelope.app !== app.name) return c.text("The envelope names another app", 400);
	const { fault, withMessage } = classify(envelope.type);
	if (envelope.frames.length === 0 && fault !== "exit") return c.text("The envelope has no frames", 400);

	const raw = await symbolicate(c.env.SYMBOLS, app.name, envelope.frames.slice(0, STORED_FRAMES));
	const message = withMessage ? envelope.message.slice(0, MAX_MESSAGE) : null;
	const tokens = selectFrames(raw, skipList);
	const now = Math.floor(Date.now() / 1000);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const result = await shard.ingest({
		reportId: envelope.reportId,
		version: envelope.version,
		channel: envelope.channel,
		trust: 0,
		userKey: envelope.installId,
		now,
		group: { fingerprint: await fingerprint(fault, tokens, message), fault, frames: JSON.stringify(raw), message },
	});
	switch (result.outcome) {
		case "unknown":
			return c.text(`Version ${envelope.version} is not released on channel ${envelope.channel}`, 410);
		case "expired":
			return c.text(`Version ${envelope.version} is no longer supported on channel ${envelope.channel}`, 410);
		case "duplicate":
			return c.text("want_attachments 0\n", 200);
		case "counted":
			return c.text("want_attachments 0\n", 201);
	}
});

api.all("*", (c) => c.text("Not found", 404));
