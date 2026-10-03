import { hex } from "./bytes";

/**
 * The `cwsym` table header: what an upload is checked against before the
 * table is stored. The tool validated the whole table before sending it,
 * so this reads only the identity of the build and the bounds of the
 * sections, and rejects a prefix table, which lacks its display sections.
 */
export interface CwsymHeader {
	rules: number;
	arch: number;
	/** Lowercase hex of the build id, the string the client sends. */
	buildId: string;
	/** Function rows. */
	count: number;
	flags: number;
}

export type CwsymParse = { ok: true; header: CwsymHeader } | { ok: false; reason: string };

export const HEADER_SIZE = 128;
const MAGIC = [0x43, 0x57, 0x53, 0x59, 0x4d, 0];
const LAYOUT_VERSION = 1;
const BUILD_ID_CAP = 20;
const ARCH_MAX = 4;

const enum Field {
	VERSION = 6,
	RULES = 8,
	ARCH = 10,
	BUILD_ID = 12,
	BUILD_ID_LEN = 32,
	COUNT = 36,
	FLAGS = 40,
	OFF_STARTS = 44,
	OFF_SIZES = 48,
	OFF_NAMES = 52,
	OFF_STRINGS = 56,
	LEN_STRINGS = 60,
	OFF_DISP = 64,
	OFF_DSTRINGS = 116,
	LEN_DSTRINGS = 120,
}

/** The header plus where the hashed sections lie, as far as `bytes` holds them. */
interface Layout {
	header: CwsymHeader;
	offStarts: number;
	offSizes: number;
	offNames: number;
	offStrings: number;
	lenStrings: number;
}

type LayoutParse = { ok: true; layout: Layout } | { ok: false; reason: string };

/** Reads the header and checks that the hashed sections lie inside `bytes`. */
function readLayout(bytes: Uint8Array): LayoutParse {
	if (bytes.length < HEADER_SIZE || MAGIC.some((b, i) => bytes[i] !== b)) {
		return { ok: false, reason: "not a cwsym table" };
	}
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const u16 = (at: Field) => view.getUint16(at, true);
	const u32 = (at: Field) => view.getUint32(at, true);
	const version = u16(Field.VERSION);
	if (version !== LAYOUT_VERSION) {
		return { ok: false, reason: `cwsym layout version ${version}; this collector reads ${LAYOUT_VERSION}` };
	}
	const arch = u16(Field.ARCH);
	const idLen = bytes[Field.BUILD_ID_LEN]!;
	const count = u32(Field.COUNT);
	if (arch < 1 || arch > ARCH_MAX || idLen < 1 || idLen > BUILD_ID_CAP || count === 0) {
		return { ok: false, reason: `bad header: arch ${arch}, build id length ${idLen}, ${count} functions` };
	}
	const layout: Layout = {
		header: {
			rules: u16(Field.RULES),
			arch,
			buildId: hex(bytes.subarray(Field.BUILD_ID, Field.BUILD_ID + idLen)),
			count,
			flags: u32(Field.FLAGS),
		},
		offStarts: u32(Field.OFF_STARTS),
		offSizes: u32(Field.OFF_SIZES),
		offNames: u32(Field.OFF_NAMES),
		offStrings: u32(Field.OFF_STRINGS),
		lenStrings: u32(Field.LEN_STRINGS),
	};
	const words = count * 4;
	if (!inside(bytes, layout.offStarts, words) || !inside(bytes, layout.offSizes, words)
		|| !inside(bytes, layout.offNames, words) || !inside(bytes, layout.offStrings, layout.lenStrings)) {
		return { ok: false, reason: "a section lies outside the file" };
	}
	return { ok: true, layout };
}

function inside(bytes: Uint8Array, off: number, len: number): boolean {
	return off % 4 === 0 && off + len <= bytes.length;
}

/** Parses the header of a complete table, or says why the bytes are not one. */
export function parseHeader(bytes: Uint8Array): CwsymParse {
	const read = readLayout(bytes);
	if (!read.ok) return read;
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const lenDstrings = view.getUint32(Field.LEN_DSTRINGS, true);
	if (lenDstrings === 0 || !inside(bytes, view.getUint32(Field.OFF_DSTRINGS, true), lenDstrings)
		|| !inside(bytes, view.getUint32(Field.OFF_DISP, true), read.layout.header.count * 4)) {
		return { ok: false, reason: "the table is a prefix without its display sections; upload the complete file" };
	}
	return { ok: true, header: read.layout.header };
}

/**
 * How many bytes from the start of the file hold the hashed sections, read
 * from a header, or null when the bytes are not a header. The sections
 * are laid out in order and `strings` is the last of them.
 */
export function prefixLength(header: Uint8Array): number | null {
	if (header.length < HEADER_SIZE || MAGIC.some((b, i) => header[i] !== b)) return null;
	const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
	return view.getUint32(Field.OFF_STRINGS, true) + view.getUint32(Field.LEN_STRINGS, true);
}

/**
 * The function table of one build: the prefix of a `cwsym` file, which
 * names the function covering any module-relative offset.
 */
export class CwsymTable {
	private constructor(
		readonly header: CwsymHeader,
		private readonly starts: Uint32Array,
		private readonly sizes: Uint32Array,
		private readonly names: Uint32Array,
		private readonly strings: Uint8Array,
	) {}

	/** Reads a prefix or a complete file; null when the bytes are not one. */
	static parse(bytes: Uint8Array): CwsymTable | null {
		const read = readLayout(bytes);
		if (!read.ok) return null;
		const { layout } = read;
		const words = (off: number) => new Uint32Array(bytes.buffer.slice(bytes.byteOffset + off, bytes.byteOffset + off + layout.header.count * 4));
		return new CwsymTable(
			layout.header,
			words(layout.offStarts), words(layout.offSizes), words(layout.offNames),
			bytes.slice(layout.offStrings, layout.offStrings + layout.lenStrings),
		);
	}

	/** The normalized name of the function covering the offset, or null when none does. */
	lookup(offset: number): string | null {
		// The last start at or below the offset; starts are ascending and disjoint.
		let lo = 0;
		let hi = this.starts.length;
		while (lo < hi) {
			const mid = (lo + hi) >>> 1;
			if (this.starts[mid]! <= offset) lo = mid + 1;
			else hi = mid;
		}
		const row = lo - 1;
		if (row < 0 || offset >= this.starts[row]! + this.sizes[row]!) return null;
		const at = this.names[row]!;
		if (at >= this.strings.length) return null;
		let end = at;
		while (end < this.strings.length && this.strings[end] !== 0) ++end;
		return new TextDecoder().decode(this.strings.subarray(at, end));
	}
}
