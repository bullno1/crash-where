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

/** Parses the header of a complete table, or says why the bytes are not one. */
export function parseHeader(bytes: Uint8Array): CwsymParse {
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
	const inside = (off: number, len: number) => off % 4 === 0 && off + len <= bytes.length;
	const words = (at: Field, n: number) => inside(u32(at), n * 4);
	if (!words(Field.OFF_STARTS, count) || !words(Field.OFF_SIZES, count) || !words(Field.OFF_NAMES, count)
		|| !inside(u32(Field.OFF_STRINGS), u32(Field.LEN_STRINGS))) {
		return { ok: false, reason: "a section lies outside the file" };
	}
	const lenDstrings = u32(Field.LEN_DSTRINGS);
	if (lenDstrings === 0 || !inside(u32(Field.OFF_DSTRINGS), lenDstrings) || !words(Field.OFF_DISP, count)) {
		return { ok: false, reason: "the table is a prefix without its display sections; upload the complete file" };
	}
	return {
		ok: true,
		header: {
			rules: u16(Field.RULES),
			arch,
			buildId: hex(bytes.subarray(Field.BUILD_ID, Field.BUILD_ID + idLen)),
			count,
			flags: u32(Field.FLAGS),
		},
	};
}
