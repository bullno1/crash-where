import { parseEnvelope } from "./envelope";
import { keptFrames, type SkipList, STORED_FRAMES } from "./grouping";
import { ENVELOPE_OBJECT } from "./samples";
import type { SampleSummary } from "./shard";
import { type RawFrame, symbolicate } from "./symbols";

/** A breadcrumb as the client wrote it: monotonic milliseconds, thread, category, message. */
export interface Crumb {
	t: number;
	th: number;
	c: string;
	m: string;
}

/** A loaded module as the client listed it. */
export interface Module {
	name: string | null;
	build_id: string | null;
	base: string;
	size: number;
}

/** An object stored beside the envelope under the sample's prefix. */
export interface Attachment {
	name: string;
	size: number;
}

/**
 * A sample as the group page shows it: the parts of the envelope a reader
 * acts on, with the frames named from the app's tables and the indices
 * of those that entered the group's fingerprint.
 */
export interface SampleView extends SampleSummary {
	/** Unix seconds the client wrote the envelope, when it said. */
	sent_at: number | null;
	/** Build id of the executable, when the client knew it. */
	build_id: string | null;
	type: string;
	message_raw: string;
	message_norm: string;
	/** Id of the crashing thread. */
	thread: number | null;
	frames: RawFrame[];
	/** Absolute address of each frame as the client printed it, aligned with `frames`. */
	raw: (string | null)[];
	/** Indices into `frames` of those the fingerprint took. */
	hashed: number[];
	env: Record<string, string>;
	state: Record<string, string>;
	breadcrumbs: Crumb[];
	modules: Module[];
	attachments: Attachment[];
	/** What the client said it would send, by kind, whether or not it arrived. */
	declared: Record<string, boolean>;
}

/** Why a sample the shard lists cannot be shown. */
export type SampleError = "missing" | "malformed";

export type SampleLoad = { ok: true; sample: SampleView } | { ok: false; reason: SampleError };

function record(v: unknown): v is Record<string, unknown> {
	return typeof v === "object" && v !== null && !Array.isArray(v);
}

/** The string-valued entries of an object; anything else in it is dropped. */
function strings(v: unknown): Record<string, string> {
	const out: Record<string, string> = {};
	if (record(v)) {
		for (const [k, x] of Object.entries(v)) if (typeof x === "string") out[k] = x;
	}
	return out;
}

/** The boolean entries of an object; anything else in it is dropped. */
function flags(v: unknown): Record<string, boolean> {
	const out: Record<string, boolean> = {};
	if (record(v)) {
		for (const [k, x] of Object.entries(v)) if (typeof x === "boolean") out[k] = x;
	}
	return out;
}

function crumbs(v: unknown): Crumb[] {
	if (!Array.isArray(v)) return [];
	const out: Crumb[] = [];
	for (const c of v) {
		if (record(c) && typeof c.t === "number" && typeof c.c === "string" && typeof c.m === "string") {
			out.push({ t: c.t, th: typeof c.th === "number" ? c.th : 0, c: c.c, m: c.m });
		}
	}
	return out;
}

function modules(v: unknown): Module[] {
	if (!Array.isArray(v)) return [];
	const out: Module[] = [];
	for (const m of v) {
		if (!record(m) || typeof m.base !== "string" || typeof m.size !== "number") continue;
		out.push({
			name: typeof m.name === "string" ? m.name : null,
			build_id: typeof m.build_id === "string" ? m.build_id : null,
			base: m.base, size: m.size,
		});
	}
	return out;
}

/** The key-value maps a sample's envelope carries, for comparing samples. */
export interface SampleContext {
	state: Record<string, string>;
	env: Record<string, string>;
}

/**
 * How many of a group's samples share each of the current sample's
 * values. `total` counts the samples whose envelope could be read, the
 * current one included; a key maps to how many of them carry the same
 * value as the current sample.
 */
export interface SharedValues {
	total: number;
	state: Record<string, number>;
	env: Record<string, number>;
}

/** Reads only the state and env of a sample's envelope; null when it is gone or not an envelope. */
export async function loadContext(bucket: R2Bucket, row: SampleSummary): Promise<SampleContext | null> {
	const object = await bucket.get(row.r2_key + ENVELOPE_OBJECT);
	if (object === null) return null;
	let json: unknown;
	try {
		json = await object.json();
	} catch {
		return null;
	}
	if (!record(json)) return null;
	return { state: strings(json.state), env: strings(json.env) };
}

/** Counts, over `others` and the current sample itself, the samples that carry each of the current sample's values. */
export function sharedValues(current: SampleContext, others: SampleContext[]): SharedValues {
	const count = (pairs: Record<string, string>, pick: (c: SampleContext) => Record<string, string>) =>
		Object.fromEntries(Object.entries(pairs).map(([k, v]) => [k, 1 + others.filter((c) => pick(c)[k] === v).length]));
	return { total: 1 + others.length, state: count(current.state, (c) => c.state), env: count(current.env, (c) => c.env) };
}

/**
 * Loads one sample from the bucket: its envelope, parsed as ingest did,
 * every frame named from the app's tables, and the objects beside it.
 * `missing` when the envelope object is gone, `malformed` when it is not
 * an envelope this collector reads.
 */
export async function loadSample(bucket: R2Bucket, app: string, row: SampleSummary, skip: SkipList): Promise<SampleLoad> {
	const object = await bucket.get(row.r2_key + ENVELOPE_OBJECT);
	if (object === null) return { ok: false, reason: "missing" };
	let json: unknown;
	try {
		json = await object.json();
	} catch {
		return { ok: false, reason: "malformed" };
	}
	const parsed = parseEnvelope(json);
	if (!parsed.ok || !record(json)) return { ok: false, reason: "malformed" };
	const { envelope } = parsed;
	const frames = await symbolicate(bucket, app, envelope.frames);
	const raw = Array.isArray(json.frames)
		? json.frames.map((f: unknown) => (record(f) && typeof f.raw === "string" ? f.raw : null))
		: [];
	const exception = record(json.exception) ? json.exception : {};
	const listed = await bucket.list({ prefix: row.r2_key });
	const attachments = listed.objects
		.filter((o) => o.key !== row.r2_key + ENVELOPE_OBJECT)
		.map((o) => ({ name: o.key.slice(row.r2_key.length), size: o.size }))
		.sort((a, b) => a.name.localeCompare(b.name));
	return {
		ok: true,
		sample: {
			...row,
			sent_at: typeof json.sent_at === "number" ? json.sent_at : null,
			build_id: record(json.app) && typeof json.app.build_id === "string" && json.app.build_id !== "" ? json.app.build_id : null,
			type: envelope.type,
			message_raw: typeof exception.message_raw === "string" ? exception.message_raw : "",
			message_norm: envelope.message,
			thread: typeof exception.thread === "number" ? exception.thread : null,
			frames,
			raw,
			hashed: keptFrames(frames.slice(0, STORED_FRAMES), skip),
			env: strings(json.env),
			state: strings(json.state),
			breadcrumbs: crumbs(json.breadcrumbs),
			modules: modules(json.modules),
			attachments,
			declared: flags(json.attachments),
		},
	};
}
