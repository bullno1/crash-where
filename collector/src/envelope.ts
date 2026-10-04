import { validChannel, validVersion } from "./releases";
import type { Frame } from "./symbols";

/** What ingest reads from an envelope; the rest is carried to a sample later. */
export interface Envelope {
	reportId: string;
	/** The install's random id; the user key of an unauthorized report. */
	installId: string;
	app: string;
	version: string;
	channel: string;
	type: string;
	/** The normalized message, as the client produced it. */
	message: string;
	frames: Frame[];
}

export type EnvelopeParse = { ok: true; envelope: Envelope } | { ok: false; reason: string };

/** Largest envelope accepted, before or after inflation. */
export const MAX_ENVELOPE_BYTES = 512 * 1024;

const SCHEMA = 2;
/** Shape of the ids the client mints: report and install. */
export const CLIENT_ID = /^[A-Za-z0-9-]{1,64}$/;
const MAX_TYPE = 256;
const MAX_FRAMES = 256;
const MAX_MODULE = 256;
const MAX_BUILD_ID = 64;

function record(v: unknown): v is Record<string, unknown> {
	return typeof v === "object" && v !== null && !Array.isArray(v);
}

function text(v: unknown, max: number): string | null {
	return typeof v === "string" && v.length >= 1 && v.length <= max ? v : null;
}

/** Reads the fields ingest needs and checks their shape; frames may be empty. */
export function parseEnvelope(json: unknown): EnvelopeParse {
	const bad = (reason: string): EnvelopeParse => ({ ok: false, reason });
	if (!record(json)) return bad("the envelope is not an object");
	if (json.schema !== SCHEMA) return bad(`envelope schema ${String(json.schema)}; this collector reads ${SCHEMA}`);
	const reportId = typeof json.report_id === "string" && CLIENT_ID.test(json.report_id) ? json.report_id : null;
	if (reportId === null) return bad("report_id is missing or malformed");
	const installId = typeof json.install_id === "string" && CLIENT_ID.test(json.install_id) ? json.install_id : null;
	if (installId === null) return bad("install_id is missing or malformed");
	if (!record(json.app)) return bad("app is missing");
	const app = text(json.app.name, 63);
	const version = typeof json.app.version === "string" && validVersion(json.app.version) ? json.app.version : null;
	const channel = typeof json.app.channel === "string" && validChannel(json.app.channel) ? json.app.channel : null;
	if (app === null || version === null || channel === null) return bad("app.name, app.version or app.channel is missing or malformed");
	if (!record(json.exception)) return bad("exception is missing");
	const type = text(json.exception.type, MAX_TYPE);
	if (type === null) return bad("exception.type is missing or malformed");
	const message = typeof json.exception.message_norm === "string" ? json.exception.message_norm : "";
	if (!Array.isArray(json.frames)) return bad("frames is missing");
	if (json.frames.length > MAX_FRAMES) return bad(`more than ${MAX_FRAMES} frames`);
	const frames: Frame[] = [];
	for (const f of json.frames) {
		if (!record(f)) return bad("a frame is not an object");
		const module = f.module === null || f.module === undefined ? null : text(f.module, MAX_MODULE);
		const buildId = f.build_id === null || f.build_id === undefined ? null : text(f.build_id, MAX_BUILD_ID);
		const offset = typeof f.offset === "number" && Number.isSafeInteger(f.offset) && f.offset >= 0 ? f.offset : null;
		if (offset === null) return bad("a frame has no usable offset");
		// A module without a build id, or the reverse, cannot be symbolicated; it is an unknown frame.
		if (module === null || buildId === null) frames.push({ module: null, buildId: null, offset });
		else frames.push({ module, buildId: buildId.toLowerCase(), offset });
	}
	return { ok: true, envelope: { reportId, installId, app, version, channel, type, message, frames } };
}
