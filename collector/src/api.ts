import { type Context, Hono } from "hono";
import { getApp } from "./apps";
import { hex } from "./bytes";
import { parseHeader } from "./cwsym";
import type { App } from "./env";
import { CLIENT_ID, MAX_ENVELOPE_BYTES, parseEnvelope } from "./envelope";
import {
	classify, compileSkipList, DEFAULT_SKIP_LIST, fingerprint, MAX_MESSAGE, selectFrames, STORED_FRAMES,
} from "./grouping";
import {
	CHANNEL_GRAMMAR, MAX_TABLE_BYTES, symbolKey, validChannel, validVersion, VERSION_GRAMMAR,
} from "./releases";
import { remapForBuild } from "./remap";
import { attachmentKind, deleteSample, ENVELOPE_OBJECT, sampleKey } from "./samples";
import { forgetTable, symbolicate } from "./symbols";
import { authenticateToken } from "./tokens";

/** The client API, outside the dashboard login. Replies are `key value` text lines. */
export const api = new Hono<App>();

/**
 * Registers a release and stores its symbol table, in one request from the
 * symbol tool. The table is written first and the rows second, so a failure
 * between the two leaves an object the rerun finds and skips. A rerun of an
 * upload already recorded changes nothing and still succeeds; a build id
 * that already belongs to another version, or to a table with other bytes,
 * is refused. A new table names the frames of its build in the groups that
 * stored them unnamed, which may rename or merge those groups.
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
	const stored = await c.env.BUCKET.head(key);
	if (stored) {
		const digest = hex(new Uint8Array(await crypto.subtle.digest("MD5", body)));
		if (stored.etag !== digest) return c.text(`Build ${buildId} is already stored with a different table`, 409);
	} else {
		await c.env.BUCKET.put(key, body, { httpMetadata: { contentType: "application/octet-stream" } });
		forgetTable(app.name, buildId);
	}

	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const result = await shard.registerRelease({ version, channel, buildId, now });
	if (result.conflict !== undefined) {
		return c.text(`Build ${buildId} is already registered under version ${result.conflict}`, 409);
	}
	if (!stored) await remapForBuild(c.env.BUCKET, shard, app.name, buildId, skipList);
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
 * with the envelope's install id as its user. The shard decides whether
 * the report is sampled; a sampled envelope is stored as parsed, plain
 * JSON, under its prefix, and the reply asks for the attachments to join
 * it. A retried report id is not counted again and gets the answer its
 * first delivery got, so the client sends what it still holds.
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

	const raw = await symbolicate(c.env.BUCKET, app.name, envelope.frames.slice(0, STORED_FRAMES));
	const message = withMessage ? envelope.message.slice(0, MAX_MESSAGE) : null;
	const tokens = selectFrames(raw, skipList);
	const now = Math.floor(Date.now() / 1000);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const trust: number = 0;
	const prefix = sampleKey(app.name, envelope.reportId);
	const result = await shard.ingest({
		reportId: envelope.reportId,
		version: envelope.version,
		channel: envelope.channel,
		trust,
		userKey: envelope.installId,
		now,
		group: { fingerprint: await fingerprint(fault, tokens, message), fault, frames: JSON.stringify(raw), message },
		sampleCap: trust === 1 ? app.sample_cap_trusted : app.sample_cap_untrusted,
		sampleKey: prefix,
		draw: Math.random(),
	});
	const reply = (sampled: boolean, status: 200 | 201) => c.text(`want_attachments ${sampled ? 1 : 0}\n`, status);
	switch (result.outcome) {
		case "unknown":
			return c.text(`Version ${envelope.version} is not released on channel ${envelope.channel}`, 410);
		case "expired":
			return c.text(`Version ${envelope.version} is no longer supported on channel ${envelope.channel}`, 410);
		case "duplicate":
			return reply(result.sampled, 200);
		case "counted":
			if (result.evicted !== null) await deleteSample(c.env.BUCKET, result.evicted);
			if (result.sampled) {
				await c.env.BUCKET.put(prefix + ENVELOPE_OBJECT, body, { httpMetadata: { contentType: "application/json" } });
			}
			return reply(result.sampled, 201);
	}
});

/**
 * Receives one attachment of a sampled report, under the name of the
 * client's sidecar. The body is stored as sent, with its type and its
 * encoding, so a download inflates in the browser. The sample's envelope
 * object is the record that the report is sampled now: without it, or
 * with a name that is not the report's own sidecar, the reply is final
 * and the client deletes the file. A repeated name overwrites, so a
 * retry after a partial failure is harmless.
 */
api.post("/:app/attach", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("app"));
	if (!app) return c.text("No such app", 404);
	if (app.disabled_at !== null) return c.text("The app is disabled", 403);
	const reportId = c.req.query("report") ?? "";
	if (!CLIENT_ID.test(reportId)) return c.text("report is missing or malformed", 400);
	const name = c.req.query("name") ?? "";
	const kind = attachmentKind(name, reportId);
	if (kind === null) return c.text("name is not an attachment of the report", 400);
	const encoding = c.req.header("Content-Encoding")?.trim().toLowerCase() ?? "";
	const gzipped = encoding === "gzip";
	if (!gzipped && encoding !== "" && encoding !== "identity") return c.text("Only gzip is accepted as a content encoding", 415);
	const length = c.req.header("Content-Length");
	if (length === undefined) return c.text("Content-Length is required", 411);
	if (Number(length) > kind.maxBytes) return c.text(`${name} must be at most ${kind.maxBytes} bytes`, 413);
	const prefix = sampleKey(app.name, reportId);
	if ((await c.env.BUCKET.head(prefix + ENVELOPE_OBJECT)) === null) return c.text("The report is not sampled", 404);
	await c.env.BUCKET.put(prefix + name, c.req.raw.body, {
		httpMetadata: { contentType: kind.contentType, contentEncoding: gzipped ? "gzip" : undefined },
	});
	return c.body(null, 201);
});

api.all("*", (c) => c.text("Not found", 404));
