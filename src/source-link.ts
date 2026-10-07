/**
 * Links from a source location to the forge that holds the file. An app's
 * template is an RFC 6570 URI template at level 2: `{name}` percent-encodes
 * the value, `{+name}` keeps the characters a URL path uses, and the names
 * are the four of `SourceVars`. A location is linked only when every
 * variable the template names is known for it, so a template naming the
 * commit links nothing of a build uploaded without one, rather than
 * leaving a hole in the URL.
 */

/** What a template may name. */
export interface SourceVars {
	/** As the upload recorded it; undefined when it did not. */
	commit?: string;
	/** The build's version string. */
	version: string;
	/** Path relative to the checkout root, with `/` between segments; undefined when the file is not under it. */
	file?: string;
	/** 1-based; undefined when the table has no line. */
	line?: string;
}

/** Longest template accepted; a forge URL with the four variables is well under it. */
export const MAX_SOURCE_LINK_TEMPLATE = 500;

/** The one form the commit may take: a hash, a tag or a branch name. */
const COMMIT_PATTERN = /^[A-Za-z0-9._/-]{1,100}$/;

export const COMMIT_GRAMMAR = "1 to 100 letters, digits, '.', '_', '-' or '/'";

/** The checkout root as the upload names it: any short string without control characters. */
const SOURCE_ROOT_PATTERN = /^[^\p{Cc}]{1,500}$/u;

export const SOURCE_ROOT_GRAMMAR = "1 to 500 characters without control characters";

export function validCommit(commit: string): boolean {
	return COMMIT_PATTERN.test(commit);
}

export function validSourceRoot(root: string): boolean {
	return SOURCE_ROOT_PATTERN.test(root);
}

const NAMES = new Set<keyof SourceVars>(["commit", "version", "file", "line"]);

/** One expression of a template: the variable and whether it is a reserved expansion. */
interface Expression {
	name: keyof SourceVars;
	reserved: boolean;
}

/** A parsed template: literal text and expressions, alternating from a literal. */
type Template = (string | Expression)[];

/**
 * Splits a template into its parts, or returns why it is not one. Only
 * level 2 of RFC 6570 with the known names is accepted, and the literal
 * text must make an http or https URL once the expressions are filled.
 */
function parseTemplate(template: string): { parts: Template } | { error: string } {
	if (template.length > MAX_SOURCE_LINK_TEMPLATE) {
		return { error: `The template must be at most ${MAX_SOURCE_LINK_TEMPLATE} characters.` };
	}
	const parts: Template = [];
	let at = 0;
	for (const m of template.matchAll(/\{([^{}]*)\}/g)) {
		parts.push(template.slice(at, m.index));
		const body = m[1]!;
		const reserved = body.startsWith("+");
		const name = reserved ? body.slice(1) : body;
		if (!NAMES.has(name as keyof SourceVars)) {
			return { error: `The template may only name {commit}, {version}, {file} and {line}, not {${body}}.` };
		}
		parts.push({ name: name as keyof SourceVars, reserved });
		at = m.index + m[0].length;
	}
	parts.push(template.slice(at));
	if (parts.some((p) => typeof p === "string" && /[{}]/.test(p))) {
		return { error: "The template has an unmatched brace." };
	}
	const sample = expand(parts, { commit: "0", version: "0", file: "0", line: "0" });
	let url: URL;
	try {
		url = new URL(sample);
	} catch {
		return { error: "The template must be an http or https URL." };
	}
	if (url.protocol !== "http:" && url.protocol !== "https:") return { error: "The template must be an http or https URL." };
	return { parts };
}

/** Why a template is unusable, or null when it is fine. */
export function templateError(template: string): string | null {
	const parsed = parseTemplate(template);
	return "error" in parsed ? parsed.error : null;
}

/** The characters a reserved expansion keeps on top of the unreserved set. */
const RESERVED = ":/?#[]@!$&'()*+,;=";

/** Percent-encodes `value` as level 2 does, keeping `RESERVED` and `%` when `reserved`. */
function encode(value: string, reserved: boolean): string {
	let out = "";
	for (const ch of value) {
		if (/[A-Za-z0-9\-._~]/.test(ch) || (reserved && (RESERVED.includes(ch) || ch === "%"))) {
			out += ch;
		} else {
			out += encodeURIComponent(ch).replace(/[!'()*]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase()}`);
		}
	}
	return out;
}

/** Fills every expression; the caller has checked that every variable is defined. */
function expand(parts: Template, vars: Required<SourceVars>): string {
	return parts.map((p) => (typeof p === "string" ? p : encode(vars[p.name], p.reserved))).join("");
}

/**
 * The URL the template gives for these variables, or null when the
 * template is not one or names a variable that is undefined.
 */
export function sourceLink(template: string, vars: SourceVars): string | null {
	const parsed = parseTemplate(template);
	if ("error" in parsed) return null;
	const used = parsed.parts.filter((p): p is Expression => typeof p !== "string");
	if (used.some((p) => vars[p.name] === undefined)) return null;
	return expand(parsed.parts, vars as Required<SourceVars>);
}

/** Whether a path is absolute: starts at the root or with a Windows drive. */
function absolute(path: string): boolean {
	return path.startsWith("/") || /^[A-Za-z]:/.test(path);
}

/**
 * A file's path relative to the checkout root, with `/` between segments,
 * or undefined when the file is not under it. A path that is already
 * relative counts as under the root, since a build that maps its paths
 * away records them that way. A Windows root, one starting with a drive
 * letter, is matched without regard to case or to the direction of the
 * separators, as the file system that produced the paths would. A result
 * with a `..` segment is refused, since the forge would not resolve it.
 */
export function relativeSource(file: string, root: string | null): string | undefined {
	let rest: string;
	if (!absolute(file)) {
		rest = file;
	} else if (root === null) {
		return undefined;
	} else {
		const base = root.replace(/[\\/]+$/, "");
		const windows = /^[A-Za-z]:/.test(base);
		const fold = (s: string) => (windows ? s.replace(/\\/g, "/").toLowerCase() : s);
		const sep = file[base.length];
		if (fold(file.slice(0, base.length)) !== fold(base) || (sep !== "/" && sep !== "\\")) return undefined;
		rest = file.slice(base.length + 1);
	}
	const segments = rest.replace(/\\/g, "/").split("/").filter((s) => s !== "" && s !== ".");
	if (segments.length === 0 || segments.includes("..")) return undefined;
	return segments.join("/");
}
