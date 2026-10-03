import { HEADER_SIZE } from "../src/cwsym";

/** Knobs for a synthetic table; every default makes a valid, complete one. */
export interface TableOptions {
	/** Raw build id bytes, at most 20. */
	buildId?: Uint8Array;
	/** Layout version field. */
	version?: number;
	rules?: number;
	arch?: number;
	/** Function rows. */
	count?: number;
	/** The one display string; changing it changes the bytes but not the identity. */
	display?: string;
	/** False cuts the file after the hashed sections, as the Worker's prefix read would. */
	complete?: boolean;
	magic?: string;
}

/** The build id most tests use: twenty bytes, as an ELF sha1 id. */
export const BUILD_ID = Uint8Array.from({ length: 20 }, (_, i) => i + 1);
export const BUILD_ID_HEX = "0102030405060708090a0b0c0d0e0f1011121314";

const align = (n: number) => (n + 7) & ~7;

/** Builds a `cwsym` v1 table in memory with `count` functions and no line sections. */
export function makeTable(opts: TableOptions = {}): Uint8Array {
	const count = opts.count ?? 1;
	const buildId = opts.buildId ?? BUILD_ID;
	const complete = opts.complete ?? true;
	const strings = new TextEncoder().encode("main\0");
	const dstrings = new TextEncoder().encode(`${opts.display ?? "main(int)"}\0`);
	const words = count * 4;
	let off = HEADER_SIZE;
	const offStarts = off; off = align(off + words);
	const offSizes = off; off = align(off + words);
	const offNames = off; off = align(off + words);
	const offStrings = off; off = align(off + strings.length);
	const prefixEnd = off;
	const offDisp = off; off = align(off + words);
	const offDstrings = off; off = align(off + dstrings.length);
	const total = complete ? off : prefixEnd;

	const bytes = new Uint8Array(total);
	const view = new DataView(bytes.buffer);
	bytes.set(new TextEncoder().encode(opts.magic ?? "CWSYM\0"), 0);
	view.setUint16(6, opts.version ?? 1, true);
	view.setUint16(8, opts.rules ?? 1, true);
	view.setUint16(10, opts.arch ?? 1, true);
	bytes.set(buildId, 12);
	bytes[32] = buildId.length;
	view.setUint32(36, count, true);
	view.setUint32(40, 0, true);
	view.setUint32(44, offStarts, true);
	view.setUint32(48, offSizes, true);
	view.setUint32(52, offNames, true);
	view.setUint32(56, offStrings, true);
	view.setUint32(60, strings.length, true);
	view.setUint32(64, offDisp, true);
	view.setUint32(116, complete ? offDstrings : 0, true);
	view.setUint32(120, complete ? dstrings.length : 0, true);
	for (let i = 0; i < count; ++i) {
		view.setUint32(offStarts + i * 4, 0x1000 * (i + 1), true);
		view.setUint32(offSizes + i * 4, 0x40, true);
		view.setUint32(offNames + i * 4, 0, true);
		if (complete) view.setUint32(offDisp + i * 4, 0, true);
	}
	bytes.set(strings, offStrings);
	if (complete) bytes.set(dstrings, offDstrings);
	return bytes;
}
