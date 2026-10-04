import { CwsymTable, HEADER_SIZE, prefixLength } from "./cwsym";
import { symbolKey } from "./releases";

/** A frame as the envelope carries it, before symbolication. */
export interface Frame {
	/** File name of the module, or null when the address was in no known module. */
	module: string | null;
	/** Hex build id of the module, or null with the module. */
	buildId: string | null;
	/** Module-relative offset. */
	offset: number;
}

/**
 * A frame with the function the symbol table names at its offset, as a
 * group stores it. The build id and offset let a table that arrives
 * later name the frame; groups stored before they were kept lack them.
 */
export interface RawFrame {
	/** The module's file name, `?` when unknown. */
	module: string;
	/** Normalized function name, or null when no table covers the offset. */
	name: string | null;
	/** Hex build id of the module, or null with the module. */
	buildId?: string | null;
	/** Module-relative offset. */
	offset?: number;
}

/**
 * Tables kept per isolate, most recently used last. A null marks a build
 * without a table and is the one answer that can go stale, since a table
 * is never replaced; it is trusted for `NEGATIVE_LIFETIME` and then
 * fetched again, so every isolate learns of an upload within that time.
 */
const cache = new Map<string, { table: CwsymTable | null; at: number }>();
const CACHE_SIZE = 32;

/** Milliseconds a build without a table is remembered as such. */
export const NEGATIVE_LIFETIME = 60_000;

/** Prefix of a web frame's module, which names the JavaScript function itself. */
const JAVASCRIPT = "javascript:";

/**
 * Fetches the hashed prefix of a build's table, two range reads on a cold
 * build so the line sections never enter the isolate. Null when the build
 * has no table or the object is not one.
 */
async function table(bucket: R2Bucket, app: string, buildId: string): Promise<CwsymTable | null> {
	const key = symbolKey(app, buildId);
	const now = Date.now();
	const hit = cache.get(key);
	if (hit !== undefined && (hit.table !== null || now - hit.at < NEGATIVE_LIFETIME)) {
		cache.delete(key);
		cache.set(key, hit);
		return hit.table;
	}
	let parsed: CwsymTable | null = null;
	const head = await bucket.get(key, { range: { offset: 0, length: HEADER_SIZE } });
	if (head) {
		const length = prefixLength(new Uint8Array(await head.arrayBuffer()));
		const prefix = length === null ? null : await bucket.get(key, { range: { offset: 0, length } });
		if (prefix) parsed = CwsymTable.parse(new Uint8Array(await prefix.arrayBuffer()));
	}
	cache.delete(key);
	if (cache.size >= CACHE_SIZE) cache.delete(cache.keys().next().value!);
	cache.set(key, { table: parsed, at: now });
	return parsed;
}

/**
 * Forgets what this isolate knows of one build, for the upload route: the
 * isolate that stored the table answers from it at once instead of after
 * the lifetime of a null it may hold. Other isolates wait that long.
 */
export function forgetTable(app: string, buildId: string): void {
	cache.delete(symbolKey(app, buildId));
}

/**
 * The name at frame `i`'s offset. Frames past the first hold return
 * addresses, so they are looked up one byte back, inside the call
 * instruction, which keeps a call at the end of a function from naming
 * the function after it.
 */
function nameAt(t: CwsymTable | null, i: number, offset: number): string | null {
	return t?.lookup(i === 0 ? offset : Math.max(0, offset - 1)) ?? null;
}

/** Names every frame from the app's symbol tables. */
export async function symbolicate(bucket: R2Bucket, app: string, frames: Frame[]): Promise<RawFrame[]> {
	const out: RawFrame[] = [];
	for (const [i, f] of frames.entries()) {
		if (f.module === null || f.buildId === null) {
			out.push({ module: "?", name: null, buildId: null, offset: f.offset });
		} else if (f.module.startsWith(JAVASCRIPT)) {
			out.push({ module: "javascript", name: f.module.slice(JAVASCRIPT.length), buildId: null, offset: f.offset });
		} else {
			out.push({ module: f.module, name: nameAt(await table(bucket, app, f.buildId), i, f.offset), buildId: f.buildId, offset: f.offset });
		}
	}
	return out;
}

/**
 * Names again the frames of one build from its table, leaving the rest
 * as they are. Null when no frame is of that build, or none carries an
 * offset to look up.
 */
export async function resymbolicate(bucket: R2Bucket, app: string, buildId: string, frames: RawFrame[]): Promise<RawFrame[] | null> {
	if (!frames.some((f) => f.buildId === buildId && f.offset !== undefined)) return null;
	const t = await table(bucket, app, buildId);
	return frames.map((f, i) =>
		f.buildId === buildId && f.offset !== undefined ? { ...f, name: nameAt(t, i, f.offset) } : f
	);
}

/** Forgets every cached table; for tests that replace an object. */
export function forgetTables(): void {
	cache.clear();
}
