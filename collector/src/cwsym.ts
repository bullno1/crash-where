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
	LINE_COUNT = 68,
	OFF_LINE_STARTS = 72,
	OFF_LINE_LENS = 76,
	OFF_LINE_FILES = 80,
	OFF_LINE_LINES = 84,
	SITE_COUNT = 88,
	OFF_SITE_STARTS = 92,
	OFF_SITE_LENS = 96,
	OFF_SITE_CALLEES = 100,
	OFF_SITE_FILES = 104,
	OFF_SITE_LINES = 108,
	OFF_SITE_PARENTS = 112,
	OFF_DSTRINGS = 116,
	LEN_DSTRINGS = 120,
}

/** Header flag: the line and inline-site sections are present. */
const FLAG_HAS_LINES = 2;
/** A site whose parent is the function itself. */
const NO_PARENT = 0xffffffff;

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

/** One source location of an offset, innermost first in a list. */
export interface Location {
	/** Display name of the function at this level. */
	function: string;
	/** Source path, or null when the table has no line for it. */
	file: string | null;
	/** 1-based line, or 0 when unknown. */
	line: number;
}

/** The sections of a complete file past the hashed prefix. */
interface Rest {
	disp: Uint32Array;
	dstrings: Uint8Array;
	lines: {
		starts: Uint32Array;
		lens: Uint32Array;
		files: Uint32Array;
		lines: Uint32Array;
	} | null;
	sites: {
		starts: Uint32Array;
		lens: Uint32Array;
		callees: Uint32Array;
		files: Uint32Array;
		lines: Uint32Array;
		parents: Uint32Array;
	} | null;
}

/** The row of the last start at or below `offset`, or -1; starts are ascending. */
function lastAt(starts: Uint32Array, offset: number): number {
	let lo = 0;
	let hi = starts.length;
	while (lo < hi) {
		const mid = (lo + hi) >>> 1;
		if (starts[mid]! <= offset) lo = mid + 1;
		else hi = mid;
	}
	return lo - 1;
}

/** The NUL-terminated string at `at` in a string section, or null when `at` is past it. */
function stringAt(strings: Uint8Array, at: number): string | null {
	if (at >= strings.length) return null;
	let end = at;
	while (end < strings.length && strings[end] !== 0) ++end;
	return new TextDecoder().decode(strings.subarray(at, end));
}

/** Every start ascending with its range not reaching the next. */
function disjoint(starts: Uint32Array, lens: Uint32Array): boolean {
	for (let i = 1; i < starts.length; ++i) {
		if (starts[i]! < starts[i - 1]! + lens[i - 1]! || starts[i]! < starts[i - 1]!) return false;
	}
	return true;
}

/** Every reference inside the string section. */
function refsInside(refs: Uint32Array, len: number): boolean {
	return refs.every((r) => r < len);
}

/**
 * The symbol table of one build. From the prefix of a `cwsym` file it
 * names the function covering any module-relative offset; from the
 * complete file it also resolves an offset to its display name, file,
 * line and inline chain, as the tool's `symbolize` does.
 */
export class CwsymTable {
	private constructor(
		readonly header: CwsymHeader,
		private readonly starts: Uint32Array,
		private readonly sizes: Uint32Array,
		private readonly names: Uint32Array,
		private readonly strings: Uint8Array,
		private readonly rest: Rest | null,
	) {}

	/**
	 * Reads a prefix or a complete file; null when the bytes are not one.
	 * The sections past the prefix are taken when they are present and
	 * sound, and left out otherwise, so a table always names functions.
	 */
	static parse(bytes: Uint8Array): CwsymTable | null {
		const read = readLayout(bytes);
		if (!read.ok) return null;
		const { layout } = read;
		const words = (off: number, n: number) => new Uint32Array(bytes.buffer.slice(bytes.byteOffset + off, bytes.byteOffset + off + n * 4));
		const count = layout.header.count;
		return new CwsymTable(
			layout.header,
			words(layout.offStarts, count), words(layout.offSizes, count), words(layout.offNames, count),
			bytes.slice(layout.offStrings, layout.offStrings + layout.lenStrings),
			CwsymTable.readRest(bytes, layout, words),
		);
	}

	private static readRest(bytes: Uint8Array, layout: Layout, words: (off: number, n: number) => Uint32Array): Rest | null {
		const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		const u32 = (at: Field) => view.getUint32(at, true);
		const count = layout.header.count;
		const offDstrings = u32(Field.OFF_DSTRINGS);
		const lenDstrings = u32(Field.LEN_DSTRINGS);
		if (lenDstrings === 0 || !inside(bytes, offDstrings, lenDstrings) || !inside(bytes, u32(Field.OFF_DISP), count * 4)) return null;
		const disp = words(u32(Field.OFF_DISP), count);
		if (!refsInside(disp, lenDstrings)) return null;
		const rest: Rest = { disp, dstrings: bytes.slice(offDstrings, offDstrings + lenDstrings), lines: null, sites: null };
		if ((layout.header.flags & FLAG_HAS_LINES) === 0) return rest;
		const lineCount = u32(Field.LINE_COUNT);
		const siteCount = u32(Field.SITE_COUNT);
		const lineFields = [Field.OFF_LINE_STARTS, Field.OFF_LINE_LENS, Field.OFF_LINE_FILES, Field.OFF_LINE_LINES];
		const siteFields = [
			Field.OFF_SITE_STARTS, Field.OFF_SITE_LENS, Field.OFF_SITE_CALLEES, Field.OFF_SITE_FILES, Field.OFF_SITE_LINES, Field.OFF_SITE_PARENTS,
		];
		if (!lineFields.every((f) => inside(bytes, u32(f), lineCount * 4)) || !siteFields.every((f) => inside(bytes, u32(f), siteCount * 4))) {
			return rest;
		}
		const [lStarts, lLens, lFiles, lLines] = lineFields.map((f) => words(u32(f), lineCount)) as [Uint32Array, Uint32Array, Uint32Array, Uint32Array];
		const [sStarts, sLens, sCallees, sFiles, sLines, sParents] = siteFields.map((f) => words(u32(f), siteCount)) as [
			Uint32Array, Uint32Array, Uint32Array, Uint32Array, Uint32Array, Uint32Array,
		];
		if (!disjoint(lStarts, lLens) || !refsInside(lFiles, lenDstrings) || !refsInside(sCallees, lenDstrings) || !refsInside(sFiles, lenDstrings)) {
			return rest;
		}
		// A site lies inside a function and inside its parent, which comes before it; sites are in start order.
		const starts = words(layout.offStarts, count);
		const sizes = words(layout.offSizes, count);
		for (let i = 0; i < siteCount; ++i) {
			const start = sStarts[i]!;
			const end = start + sLens[i]!;
			const fn = lastAt(starts, start);
			const parent = sParents[i]!;
			let ok = end > start && fn >= 0 && end <= starts[fn]! + sizes[fn]! && (i === 0 || sStarts[i - 1]! <= start);
			if (ok && parent !== NO_PARENT) ok = parent < i && sStarts[parent]! <= start && end <= sStarts[parent]! + sLens[parent]!;
			if (!ok) return rest;
		}
		rest.lines = { starts: lStarts, lens: lLens, files: lFiles, lines: lLines };
		rest.sites = { starts: sStarts, lens: sLens, callees: sCallees, files: sFiles, lines: sLines, parents: sParents };
		return rest;
	}

	/** Whether the bytes held the display and line sections, so `symbolize` can do more than name. */
	get complete(): boolean {
		return this.rest !== null;
	}

	/** The row covering the offset, or -1 when none does. */
	private rowAt(offset: number): number {
		const row = lastAt(this.starts, offset);
		return row < 0 || offset >= this.starts[row]! + this.sizes[row]! ? -1 : row;
	}

	/** The normalized name of the function covering the offset, or null when none does. */
	lookup(offset: number): string | null {
		const row = this.rowAt(offset);
		return row < 0 ? null : stringAt(this.strings, this.names[row]!);
	}

	/** The display name of a function row, or its normalized name when the table has none. */
	private display(row: number): string {
		const at = this.rest?.disp[row] ?? 0;
		return (at === 0 ? null : stringAt(this.rest!.dstrings, at)) ?? stringAt(this.strings, this.names[row]!) ?? "";
	}

	/**
	 * The source locations of an offset, innermost first: the line row
	 * covering it gives the innermost file and line, the innermost inlined
	 * site covering it gives that location's function, and each parent
	 * site adds its call site as the next level, ending at the function
	 * itself. One location with the function and no file when the table
	 * has no lines; none when no function covers the offset.
	 */
	symbolize(offset: number): Location[] {
		const fn = this.rowAt(offset);
		if (fn < 0) return [];
		const rest = this.rest;
		const dstr = (at: number) => (rest === null ? null : stringAt(rest.dstrings, at)) ?? "";
		let file: string | null = null;
		let line = 0;
		if (rest?.lines) {
			const l = lastAt(rest.lines.starts, offset);
			if (l >= 0 && offset - rest.lines.starts[l]! < rest.lines.lens[l]!) {
				file = dstr(rest.lines.files[l]!);
				line = rest.lines.lines[l]!;
			}
		}
		// The innermost site covering the offset is the last covering one in
		// (start, depth) order; walk back, but not past the function's own start.
		let site = -1;
		const sites = rest?.sites ?? null;
		if (sites) {
			for (let s = lastAt(sites.starts, offset); s >= 0; --s) {
				if (sites.starts[s]! < this.starts[fn]!) break;
				if (offset - sites.starts[s]! < sites.lens[s]!) {
					site = s;
					break;
				}
			}
		}
		const out: Location[] = [{ function: site >= 0 ? dstr(sites!.callees[site]!) : this.display(fn), file, line }];
		while (site >= 0) {
			const parent = sites!.parents[site]!;
			out.push({
				function: parent !== NO_PARENT ? dstr(sites!.callees[parent]!) : this.display(fn),
				file: sites!.files[site] === 0 ? null : dstr(sites!.files[site]!),
				line: sites!.lines[site]!,
			});
			site = parent !== NO_PARENT ? parent : -1;
		}
		return out;
	}
}
