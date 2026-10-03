import { Hono } from "hono";
import { getApp } from "./apps";
import { hex } from "./bytes";
import { parseHeader } from "./cwsym";
import type { App } from "./env";
import {
	CHANNEL_GRAMMAR, MAX_TABLE_BYTES, symbolKey, validChannel, validVersion, VERSION_GRAMMAR,
} from "./releases";
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

api.all("*", (c) => c.text("Not found", 404));
