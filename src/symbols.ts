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

/** A frame with the function the symbol table names at its offset. */
export interface RawFrame {
	/** The module's file name, `?` when unknown. */
	module: string;
	/** Normalized function name, or null when no table covers the offset. */
	name: string | null;
}

/** Tables kept per isolate, most recently used last; a null marks a build without one. */
const cache = new Map<string, CwsymTable | null>();
const CACHE_SIZE = 32;

/** Prefix of a web frame's module, which names the JavaScript function itself. */
const JAVASCRIPT = "javascript:";

/**
 * Fetches the hashed prefix of a build's table, two range reads on a cold
 * build so the line sections never enter the isolate. Null when the build
 * has no table or the object is not one.
 */
async function table(bucket: R2Bucket, app: string, buildId: string): Promise<CwsymTable | null> {
	const key = symbolKey(app, buildId);
	if (cache.has(key)) {
		const hit = cache.get(key)!;
		cache.delete(key);
		cache.set(key, hit);
		return hit;
	}
	let parsed: CwsymTable | null = null;
	const head = await bucket.get(key, { range: { offset: 0, length: HEADER_SIZE } });
	if (head) {
		const length = prefixLength(new Uint8Array(await head.arrayBuffer()));
		const prefix = length === null ? null : await bucket.get(key, { range: { offset: 0, length } });
		if (prefix) parsed = CwsymTable.parse(new Uint8Array(await prefix.arrayBuffer()));
	}
	if (cache.size >= CACHE_SIZE) cache.delete(cache.keys().next().value!);
	cache.set(key, parsed);
	return parsed;
}

/**
 * Names every frame from the app's symbol tables. Frames past the first
 * hold return addresses, so they are looked up one byte back, inside the
 * call instruction, which keeps a call at the end of a function from
 * naming the function after it.
 */
export async function symbolicate(bucket: R2Bucket, app: string, frames: Frame[]): Promise<RawFrame[]> {
	const out: RawFrame[] = [];
	for (const [i, f] of frames.entries()) {
		if (f.module === null || f.buildId === null) {
			out.push({ module: "?", name: null });
		} else if (f.module.startsWith(JAVASCRIPT)) {
			out.push({ module: "javascript", name: f.module.slice(JAVASCRIPT.length) });
		} else {
			const t = await table(bucket, app, f.buildId);
			const offset = i === 0 ? f.offset : Math.max(0, f.offset - 1);
			out.push({ module: f.module, name: t?.lookup(offset) ?? null });
		}
	}
	return out;
}

/** Forgets every cached table; for tests that replace an object. */
export function forgetTables(): void {
	cache.clear();
}
