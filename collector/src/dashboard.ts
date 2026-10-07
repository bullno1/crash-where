import pico from "@picocss/pico/css/pico.classless.min.css";
import { type Context, Hono } from "hono";
import { accepts } from "hono/accepts";
import { deleteCookie, getCookie, setCookie } from "hono/cookie";
import { csrf } from "hono/csrf";
import { html } from "hono/html";
import { type AppError, type AppInput, type AppRow, createApp, getApp, listApps, validateApp } from "./apps";
import { requireLogin } from "./auth";
import type { App } from "./env";
import { layout } from "./layout";
import { type Page, render } from "./page";
import type { Location } from "./cwsym";
import { compileSkipList, DEFAULT_SKIP_LIST, groupTitle, keptFrames } from "./grouping";
import { loadContext, loadSample, type SampleError, type SampleView, sharedValues, type SharedValues } from "./sample";
import { attachmentKind, ENVELOPE_OBJECT, sampleKey } from "./samples";
import type { GroupRelease, GroupSummary, GroupUsers, SampleSummary, VersionSummary } from "./shard";
import type { RawFrame } from "./symbols";
import { createToken, listTokens, MAX_LABEL, revokeToken, type TokenRow, validLabel } from "./tokens";

/**
 * The dashboard: a login is required before any of its routes runs, and a
 * form submission must come from this origin, which refuses forged posts
 * before the ambient credentials of either login mode can authorize them.
 */
export const dashboard = new Hono<App>();

dashboard.use("*", csrf());
dashboard.use("*", requireLogin);

dashboard.get("/pico.css", (c) => {
	c.header("Content-Type", "text/css; charset=utf-8");
	c.header("Cache-Control", "public, max-age=86400");
	return c.body(pico);
});

function day(unix: number): string {
	return new Date(unix * 1000).toISOString().slice(0, 10);
}

/** A moment to the second, in UTC. */
function when(unix: number): string {
	return new Date(unix * 1000).toISOString().slice(0, 19).replace("T", " ") + " UTC";
}

/**
 * Whether the client prefers JSON to a page. Every dashboard route answers
 * both from the same data: a browser, or anything that accepts either,
 * gets HTML; a script asking for `application/json` gets the object the
 * page would have been rendered from. The reply is marked as varying by
 * the header it was chosen on.
 */
function wantsJson(c: Context<App>): boolean {
	c.header("Vary", "Accept");
	const type = accepts(c, { header: "Accept", supports: ["text/html", "application/json"], default: "text/html" });
	return type === "application/json";
}

/** The submitted fields, from a form or a JSON object, trimmed; anything that is not a string is absent. */
async function fields(c: Context<App>): Promise<Record<string, string>> {
	const type = c.req.header("Content-Type") ?? "";
	const body: unknown = type.startsWith("application/json") ? await c.req.json() : await c.req.parseBody();
	const out: Record<string, string> = {};
	if (typeof body === "object" && body !== null) {
		for (const [key, value] of Object.entries(body)) {
			if (typeof value === "string") out[key] = value.trim();
		}
	}
	return out;
}

/** The message for a name already in use, on the page and in JSON alike. */
function duplicateName(name: string): AppError {
	return { field: "name", message: `An app named '${name}' already exists.` };
}

/** A labelled input, marked invalid with its message when the error is its own. */
function field(
	label: string, name: keyof AppInput, value: string, attrs: Page, error: AppError | null
): Page {
	const mine = error?.field === name;
	return html`<label>${label}
<input name="${name}" value="${value}" ${attrs} ${mine ? html`aria-invalid="true" aria-describedby="${name}-error"` : ""}>
${mine ? html`<small id="${name}-error">${error.message}</small>` : ""}
</label>`;
}

function appsPage(who: string, apps: AppRow[], form: AppInput, error: AppError | null): Page {
	const rows = apps.map(
		(app) => html`<tr>
<td><a href="/dashboard/apps/${app.name}">${app.display_name}</a></td>
<td><code>${app.name}</code></td>
<td>${app.disabled_at === null ? "active" : html`<small>disabled</small>`}</td>
<td>${day(app.created_at)}</td>
<td>${app.created_by}</td>
</tr>`
	);
	const table =
		apps.length === 0
			? html`<p>No apps yet.</p>`
			: html`<table>
<thead><tr><th>App</th><th>Name</th><th>Status</th><th>Created</th><th>By</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
	return layout(
		"Apps", who,
		html`<h1>Apps</h1>
${table}
<h2>New app</h2>
<form method="post" action="/dashboard/apps">
${field("Name", "name", form.name, html`required pattern="[a-z0-9_\\-]{1,63}" placeholder="forest-quest"`, error)}
${field("Display name", "display_name", form.display_name, html`required maxlength="100" placeholder="Forest Quest"`, error)}
<button>Create app</button>
</form>`
	);
}

/**
 * Rebuilds the create form from the query string, where a failed submission
 * left its values, and works out what was wrong with them against the
 * current apps. A query without form fields is a plain visit.
 */
function formState(query: Record<string, string>, apps: AppRow[]): { form: AppInput; error: AppError | null } {
	const form: AppInput = { name: query.name ?? "", display_name: query.display_name ?? "" };
	if (!("name" in query) && !("display_name" in query)) return { form, error: null };
	const error = validateApp(form) ?? (apps.some((app) => app.name === form.name) ? duplicateName(form.name) : null);
	return { form, error };
}

dashboard.get("/", async (c) => {
	const who = c.get("identity");
	const apps = await listApps(c.get("db"));
	if (wantsJson(c)) return c.json({ apps });
	const { form, error } = formState(c.req.query(), apps);
	return render(c, appsPage(who.email ?? who.sub, apps, form, error));
});

/**
 * Every outcome of the form redirects, so a refresh of the result never
 * resubmits it. A JSON client gets the row, or the error on the field at
 * fault.
 */
dashboard.post("/apps", async (c) => {
	const body = await fields(c);
	const input: AppInput = { name: body.name ?? "", display_name: body.display_name ?? "" };
	const invalid = validateApp(input);
	const created = invalid === null ? await createApp(c.get("db"), input, c.get("identity")) : null;
	if (wantsJson(c)) {
		if (created) return c.json({ app: created }, 201);
		return c.json({ error: invalid ?? duplicateName(input.name) }, invalid ? 400 : 409);
	}
	if (!created) return c.redirect(`/dashboard?${new URLSearchParams({ ...input })}`, 303);
	return c.redirect("/dashboard", 303);
});

/** A token as the page and JSON show it: everything but the hash. */
interface TokenSummary {
	id: number;
	label: string;
	created_at: number;
	created_by: string;
	last_used_at: number | null;
	revoked_at: number | null;
}

function summarizeToken({ id, label, created_at, created_by, last_used_at, revoked_at }: TokenRow): TokenSummary {
	return { id, label, created_at, created_by, last_used_at, revoked_at };
}

/** The upload tokens of an app, with the one just minted shown in clear. */
function tokensSection(app: AppRow, tokens: TokenSummary[], fresh: string | null): Page {
	const rows = tokens.map(
		(t) => html`<tr>
<td>${t.label}</td>
<td>${day(t.created_at)}</td>
<td>${t.created_by}</td>
<td>${t.last_used_at === null ? html`<small>never</small>` : day(t.last_used_at)}</td>
<td>${t.revoked_at === null
	? html`<form method="post" action="${tokensPath(app.name)}/${t.id}/revoke"><button class="secondary">Revoke</button></form>`
	: html`<small>revoked ${day(t.revoked_at)}</small>`}</td>
</tr>`
	);
	const table =
		tokens.length === 0
			? html`<p>No upload tokens yet.</p>`
			: html`<table>
<thead><tr><th>Label</th><th>Created</th><th>By</th><th>Last used</th><th></th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
	return html`<h2>Upload tokens</h2>
<p>CI uploads symbol tables with <code>cwsym upload</code>, which reads its token from <code>CWSYM_TOKEN</code>.</p>
${fresh === null
	? ""
	: html`<article><p>New token, shown only this once:</p><pre><code>${fresh}</code></pre></article>`}
${table}
<form method="post" action="${tokensPath(app.name)}">
<label>Label
<input name="label" required maxlength="${MAX_LABEL}" placeholder="GitHub Actions">
</label>
<button>Create upload token</button>
</form>`;
}

const skipList = compileSkipList(DEFAULT_SKIP_LIST);

/** A group as the page and JSON show it, with the title the rule gives it. */
interface CrashSummary {
	id: number;
	title: string;
	fault: string;
	message: string | null;
	/** The raw frames the rule saw, before the skip list. */
	frames: RawFrame[];
	count: number;
	recent_count: number;
	recent_users: number;
	urgency: number;
	first_seen: number;
	last_seen: number;
}

function summarizeCrash(g: GroupSummary): CrashSummary {
	const frames = JSON.parse(g.frames) as RawFrame[];
	return { ...g, frames, title: groupTitle(g.fault, frames, g.message, skipList) };
}

/**
 * The crashes of an app, most urgent first. The overview names each by its
 * fault and first frame, with an ellipsis standing for the rest; the JSON
 * title carries the caller too.
 */
function crashesSection(app: AppRow, crashes: CrashSummary[]): Page {
	if (crashes.length === 0) return html`<h2>Crashes</h2>
<p>No crashes reported yet.</p>`;
	const rows = crashes.map(
		(g) => html`<tr>
<td><a href="${crashPath(app.name, g.id)}">${groupTitle(g.fault, g.frames, g.message, skipList, true)}</a></td>
<td>${g.urgency}</td>
<td>${g.recent_count}</td>
<td>${g.recent_users}</td>
<td>${g.count}</td>
<td>${day(g.first_seen)}</td>
<td>${day(g.last_seen)}</td>
</tr>`
	);
	return html`<h2>Crashes</h2>
<p>Urgency is the square root of reports times users over the last seven days.</p>
<table>
<thead><tr><th>Crash</th><th>Urgency</th><th>Reports (7 days)</th><th>Users (7 days)</th><th>Total</th><th>First seen</th><th>Last seen</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

/**
 * The versions of an app, newest first, with the channels they are
 * released on and their builds. A channel alone is the version's live
 * release there; one superseded says until when it is supported. An
 * arrow marks `shown`, the version a link to this page named.
 */
function versionsSection(versions: VersionSummary[], shown: string | null): Page {
	if (versions.length === 0) return html`<h2>Versions</h2>
<p>No versions yet. The first symbol upload for this app registers one.</p>`;
	const rows = versions.map(
		(v) => html`<tr>
<td>${v.version === shown ? "→ " : ""}<code>${v.version}</code></td>
<td>${v.channels.length === 0
	? html`<small>none</small>`
	: v.channels.map(
			(r) => html`<div>${r.channel}${r.supported_until === null ? "" : html` <small>until ${day(r.supported_until)}</small>`}</div>`
		)}</td>
<td>${v.builds.length === 0 ? html`<small>none</small>` : v.builds.map((b) => html`<div><code>${b}</code></div>`)}</td>
<td>${day(v.created_at)}</td>
</tr>`
	);
	return html`<h2>Versions</h2>
<table>
<thead><tr><th>Version</th><th>Channels</th><th>Builds</th><th>Created</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

function appPath(name: string): string {
	return `/dashboard/apps/${name}`;
}

function tokensPath(name: string): string {
	return `${appPath(name)}/tokens`;
}

function versionsPath(name: string): string {
	return `${appPath(name)}/versions`;
}

function crashPath(name: string, id: number): string {
	return `${appPath(name)}/crashes/${id}`;
}

/** The pages of an app, in the order the sub-navigation lists them. */
const APP_PAGES = [
	{ key: "crashes", label: "Crashes", path: appPath },
	{ key: "versions", label: "Versions", path: versionsPath },
	{ key: "tokens", label: "Upload tokens", path: tokensPath },
] as const;

type AppPageKey = (typeof APP_PAGES)[number]["key"];

/**
 * One page of an app: its heading and details, the links to its other
 * pages with this one marked, then the section. `canonical` is the page's
 * permanent URL when the request's is not it.
 */
function appPage(who: string, app: AppRow, current: AppPageKey, section: Page, canonical: string | null = null): Page {
	const links = APP_PAGES.map(
		(p) => html`<li><a href="${p.path(app.name)}"${p.key === current ? html` aria-current="page"` : ""}>${p.label}</a></li>`
	);
	const label = APP_PAGES.find((p) => p.key === current)!.label;
	return layout(
		current === "crashes" ? app.display_name : `${label} · ${app.display_name}`, who,
		html`<h1>${app.display_name}</h1>
<p><code>${app.name}</code> · ${app.disabled_at === null ? "active" : `disabled since ${day(app.disabled_at)}`} · created at ${day(app.created_at)} by ${app.created_by}</p>
<nav><ul>${links}</ul></nav>
${section}`,
		canonical
	);
}

/**
 * A token just minted reaches the page that shows it through this cookie,
 * scoped to the app's tokens page and cleared on the first read, so that it
 * enters neither a URL nor the browsing history and the form post can
 * redirect like every other.
 */
const FRESH_TOKEN_COOKIE = "cw_new_token";
const FRESH_TOKEN_SECONDS = 60;

dashboard.get("/apps/:name", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const crashes = (await shard.listGroups(Math.floor(Date.now() / 1000))).map(summarizeCrash);
	if (wantsJson(c)) return c.json({ app, crashes });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "crashes", crashesSection(app, crashes)));
});

dashboard.get("/apps/:name/versions", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const versions = await shard.listVersions();
	if (wantsJson(c)) return c.json({ app, versions });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "versions", versionsSection(versions, c.req.query("version") ?? null)));
});

dashboard.get("/apps/:name/tokens", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const tokens = (await listTokens(c.get("db"), app.id)).map(summarizeToken);
	if (wantsJson(c)) return c.json({ app, tokens });
	const fresh = getCookie(c, FRESH_TOKEN_COOKIE) ?? null;
	if (fresh !== null) deleteCookie(c, FRESH_TOKEN_COOKIE, { path: tokensPath(app.name), secure: true });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "tokens", tokensSection(app, tokens, fresh)));
});

/** A browser sees the new token once on the tokens page; a JSON client gets it in the reply. */
dashboard.post("/apps/:name/tokens", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const body = await fields(c);
	const label = validLabel(body.label ?? "");
	if (label === null) return c.text(`A label of 1 to ${MAX_LABEL} characters is required`, 400);
	const { token, row } = await createToken(c.get("db"), app.id, label, c.get("identity"), Math.floor(Date.now() / 1000));
	if (wantsJson(c)) return c.json({ token, ...summarizeToken(row) }, 201);
	setCookie(c, FRESH_TOKEN_COOKIE, token, {
		path: tokensPath(app.name), httpOnly: true, secure: true, sameSite: "Strict", maxAge: FRESH_TOKEN_SECONDS,
	});
	return c.redirect(tokensPath(app.name), 303);
});

dashboard.post("/apps/:name/tokens/:id/revoke", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const revoked = await revokeToken(c.get("db"), app.id, Number(c.req.param("id")), Math.floor(Date.now() / 1000));
	if (wantsJson(c)) return c.json({ revoked }, revoked ? 200 : 404);
	return c.redirect(tokensPath(app.name), 303);
});

/** Hex as a debugger prints it. */
function hex(n: number): string {
	return `0x${n.toString(16)}`;
}

/** A source location as `file:line`, the file alone when the line is unknown, or nothing. */
function where(loc: Location): Page {
	if (loc.file === null) return html``;
	return html`<code>${loc.file}${loc.line === 0 ? "" : `:${loc.line}`}</code>`;
}

/**
 * A stack as a table, the frames the fingerprint took marked. With
 * `locations`, a Location column gives each frame's file and line, the
 * function is its display name, and a frame inside inlined code takes
 * one row per inline level, innermost first, the outer levels indented
 * under it. A frame no table locates shows its `traceLine` there when it
 * has one, which is the browser's own line for a JavaScript frame.
 */
function stackTable(frames: RawFrame[], hashed: number[], traceLine: (string | null)[] = [], locations: Location[][] | null = null): Page {
	const marked = new Set(hashed);
	const rows = frames.flatMap((f, i) => {
		const locs = locations?.[i] ?? [];
		const plain = f.name === null ? html`<small>unnamed</small>` : html`${f.name}`;
		const name = locs.length === 0 ? plain : html`${locs[0]!.function}`;
		const line = traceLine[i] ?? null;
		const location = locs.length > 0 ? where(locs[0]!) : line === null ? html`` : html`<code>${line}</code>`;
		const first = html`<tr>
<td>${i}</td>
<td>${marked.has(i) ? html`<mark>${name}</mark>` : name}</td>${locations === null ? "" : html`
<td>${location}</td>`}
<td><code>${f.module}</code></td>
<td>${f.offset === undefined ? "" : html`<code>${hex(f.offset)}</code>`}</td>
</tr>`;
		const outer = locs.slice(1).map((loc) => html`<tr>
<td></td>
<td>&nbsp;&nbsp;&nbsp;&nbsp;↳ ${marked.has(i) ? html`<mark>${loc.function}</mark>` : loc.function}</td>
<td>${where(loc)}</td>
<td></td>
<td></td>
</tr>`);
		return [first, ...outer];
	});
	return html`<table>
<thead><tr><th>#</th><th>Function</th>${locations === null ? "" : html`<th>Location</th>`}<th>Module</th><th>Offset</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

/**
 * Key-value pairs the game set, as a table, or a note that it set none.
 * With `shared`, a third column says how many of the group's samples
 * carry the same value, and a row every sample agrees on is bold.
 */
function pairsTable(pairs: Record<string, string>, shared: { total: number; counts: Record<string, number> } | null): Page {
	const entries = Object.entries(pairs);
	if (entries.length === 0) return html`<p><small>none</small></p>`;
	const rows = entries.map(([k, v]) => {
		if (shared === null) return html`<tr><th scope="row">${k}</th><td>${v}</td></tr>`;
		const n = shared.counts[k] ?? 1;
		return n === shared.total
			? html`<tr><th scope="row"><strong>${k}</strong></th><td><strong>${v}</strong></td><td>${n} of ${shared.total}</td></tr>`
			: html`<tr><th scope="row">${k}</th><td>${v}</td><td>${n} of ${shared.total}</td></tr>`;
	});
	return html`<table>
${shared === null ? "" : html`<thead><tr><th>Key</th><th>Value</th><th>Samples</th></tr></thead>`}
<tbody>${rows}</tbody>
</table>`;
}

/** A run of a raw message: text the normalizer keeps, or one it replaces by `placeholder`. */
interface MessageSpan {
	text: string;
	placeholder: string | null;
}

/**
 * Splits a raw message as the client's normalizer does: a hex address
 * becomes `<ADDR>`, a run of digits `<N>`, and a control character a
 * space, which `text` carries as written.
 */
function messageSpans(raw: string): MessageSpan[] {
	const spans: MessageSpan[] = [];
	for (const m of raw.matchAll(/0x[0-9a-fA-F]+|[0-9]+|[^0-9]+?(?=0x[0-9a-fA-F]|[0-9]|$)/g)) {
		const text = m[0];
		spans.push({ text, placeholder: text.startsWith("0x") ? "<ADDR>" : /^[0-9]/.test(text) ? "<N>" : null });
	}
	return spans;
}

/** What the normalizer makes of the spans; equal to the stored message when the page's reading of it is right. */
function normalized(spans: MessageSpan[]): string {
	return spans.map((s) => s.placeholder ?? s.text.replace(/[\u0000-\u001f]/g, " ")).join("");
}

/**
 * The raw message with the parts the normalizer replaced underlined, so
 * the reader sees both the specific values and what the group hashes.
 * When the stored normalized message is not what this reading gives, the
 * two are shown apart instead.
 */
function messageBlock(raw: string, norm: string): Page {
	if (raw === "") return norm === "" ? html`` : html`<pre>${norm}</pre>`;
	const spans = messageSpans(raw);
	if (normalized(spans) !== norm) {
		return html`<pre>${raw}</pre>
${norm === "" ? "" : html`<p>Normalized: <code>${norm}</code></p>`}`;
	}
	const marked = spans.map((s) => (s.placeholder === null ? html`${s.text}` : html`<u title="${s.placeholder}">${s.text}</u>`));
	return html`<pre>${marked}</pre>`;
}

/** The facts of a group that hold whichever sample is shown. */
function crashHeader(g: CrashSummary): Page {
	return html`<h2>${g.title}</h2>
<p><code>${g.fault}</code> · first seen ${when(g.first_seen)} · last seen ${when(g.last_seen)}</p>
<p>${g.count} reports in total · ${g.recent_count} reports from ${g.recent_users} users in the last seven days · urgency ${g.urgency}</p>`;
}

/** The releases the group was reported on, most reported first, each version linking to its row of the versions page. */
function versionsBlock(app: AppRow, releases: GroupRelease[]): Page {
	if (releases.length === 0) return html`<h3>Versions</h3>
<p><small>no reports counted</small></p>`;
	const rows = releases.map(
		(r) => html`<tr><td><a href="${versionsPath(app.name)}?version=${encodeURIComponent(r.version)}">${r.version}</a></td><td>${r.channel}</td><td>${r.count}</td></tr>`
	);
	return html`<h3>Versions</h3>
<table>
<thead><tr><th>Version</th><th>Channel</th><th>Reports</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

/**
 * How the group's reports spread over installs: one line when every
 * report came from another one, else the installs that repeat, each
 * with a link to a sample of its own when the group holds one.
 */
function usersBlock(path: string, users: GroupUsers, samples: SampleSummary[]): Page {
	const noun = (n: number, one: string, many: string) => `${n} ${n === 1 ? one : many}`;
	const spread = html`<p>${noun(users.reports, "report", "reports")} from ${noun(users.users, "install", "installs")}${users.top.length === 0 ? ", none of them twice" : ""}.</p>`;
	if (users.top.length === 0) return html`<h3>Users</h3>
${spread}`;
	const rows = users.top.map((u) => {
		const sample = samples.find((s) => s.trust === u.trust && s.user_key === u.user_key);
		return html`<tr>
<td><code>${u.user_key}</code></td>
<td>${u.count}</td>
<td>${when(u.last_seen)}</td>
<td>${sample === undefined ? html`<small>none</small>` : html`<a href="${path}?sample=${sample.report_id}"><code>${sample.report_id}</code></a>`}</td>
</tr>`;
	});
	return html`<h3>Users</h3>
${spread}
<table>
<thead><tr><th>Install</th><th>Reports</th><th>Last seen</th><th>Sample</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

/**
 * What a sample tells about the crash: the exception, the stack with the
 * fingerprinted frames marked, then what the game recorded around it and
 * the files that came with it.
 */
function sampleBody(path: string, s: SampleView, shared: SharedValues | null): Page {
	const last = s.breadcrumbs.at(-1)?.t ?? 0;
	const thread = (th: number) => (th === s.thread ? html`<mark>${th}</mark>` : html`${th}`);
	const crumbs = s.breadcrumbs.length === 0
		? html`<p><small>none</small></p>`
		: html`<p>Marked breadcrumbs are from the crashing thread.</p>
<table>
<thead><tr><th>Before last</th><th>Thread</th><th>Category</th><th>Message</th></tr></thead>
<tbody>${s.breadcrumbs.map((c) => html`<tr><td>${((c.t - last) / 1000).toFixed(3)} s</td><td>${thread(c.th)}</td><td>${c.c}</td><td>${c.m}</td></tr>`)}</tbody>
</table>`;
	const modules = s.modules.length === 0
		? html`<p><small>none</small></p>`
		: html`<table>
<thead><tr><th>Module</th><th>Build id</th><th>Base</th><th>Size</th></tr></thead>
<tbody>${s.modules.map((m) => html`<tr><td><code>${m.name ?? "?"}</code></td><td>${m.build_id === null ? "" : html`<code>${m.build_id}</code>`}</td><td><code>${m.base}</code></td><td>${m.size}</td></tr>`)}</tbody>
</table>`;
	const file = (name: string) => `${path}/samples/${s.report_id}/${name}`;
	const attachments = s.attachments.map((a) => html`<li><a href="${file(a.name)}">${a.name}</a> <small>${a.size} bytes</small></li>`);
	// A kind the client declared and whose file never arrived: lost on the way, or still to come.
	const awaited = Object.entries(s.declared)
		.filter(([kind, yes]) => yes && !s.attachments.some((a) => a.name.endsWith(`.${ATTACHMENT_EXT[kind] ?? kind}`)))
		.map(([kind]) => html`<li><s title="Not received">${kind}</s></li>`);
	return html`<h3>Exception</h3>
<p><code>${s.type}</code>${s.thread === null ? "" : html` on thread ${s.thread}`}</p>
${messageBlock(s.message_raw, s.message_norm)}
<h3>Stack</h3>
<p>Marked frames entered the fingerprint.</p>
${stackTable(s.frames, s.hashed, s.trace_line, s.locations)}
<h3>Breadcrumbs</h3>
${crumbs}
<h3>State</h3>
${pairsTable(s.state, shared === null ? null : { total: shared.total, counts: shared.state })}
<h3>Environment</h3>
${pairsTable(s.env, shared === null ? null : { total: shared.total, counts: shared.env })}
<h3>Modules</h3>
${modules}
<h3>Files</h3>
<ul><li><a href="${file(ENVELOPE_OBJECT)}">${ENVELOPE_OBJECT}</a></li>${attachments}${awaited}</ul>`;
}

/** The sidecar extension of each attachment kind the envelope declares. */
const ATTACHMENT_EXT: Record<string, string> = { log_tail: "log", minidump: "dmp", snapshot: "snap" };

/** What the page shows in place of a sample it cannot load. */
function sampleProblem(reason: SampleError): Page {
	return reason === "missing"
		? html`<p>The envelope of this sample is no longer stored.</p>`
		: html`<p>The envelope of this sample is not one this collector reads.</p>`;
}

/** The frames the group stored, for a group that holds no sample. */
function storedFrames(g: CrashSummary): Page {
	return html`<h3>Stack</h3>
<p>No sample is held for this crash; these are the frames of the report that opened it. Marked frames entered the fingerprint.</p>
${stackTable(g.frames, keptFrames(g.frames, skipList))}`;
}

/**
 * Every sample of the group, newest first, each a link. An arrow marks the
 * current one, whose link is its permanent URL and which alone shows its
 * install, the id not being worth a column.
 */
function samplesSection(path: string, samples: SampleSummary[], shown: string | null): Page {
	if (samples.length === 0) return html``;
	const rows = samples.map(
		(s) => html`<tr>
<td>${s.report_id === shown
	? html`→ <a href="${path}?sample=${s.report_id}" aria-current="page"><code>${s.report_id}</code></a><br><small>install <code>${s.user_key}</code>${s.user_reports > 1 ? html` · ${s.user_reports} reports` : ""}</small>`
	: html`<a href="${path}?sample=${s.report_id}"><code>${s.report_id}</code></a>`}</td>
<td><code>${s.version}</code></td>
<td>${s.channel}</td>
<td>${when(s.received_at)}</td>
</tr>`
	);
	return html`<h3>Samples</h3>
<table>
<thead><tr><th>Report</th><th>Version</th><th>Channel</th><th>Received</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

/** The group of the request, or null when the id is not one or names no group. */
async function crashOf(c: Context<App>, app: AppRow) {
	const id = Number(c.req.param("id"));
	if (!Number.isInteger(id) || id <= 0) return null;
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	return shard.getGroup(id, Math.floor(Date.now() / 1000));
}

/**
 * One crash: its facts and the releases it was seen on, then what one of
 * its samples shows, the newest unless the query names another, then the
 * list of every sample the group holds, where the current one links to
 * its permanent URL. The state and env of the other samples are read too,
 * so each value can say how many samples share it; with fewer than two
 * envelopes readable the comparison is left out. A group without samples
 * shows its stored frames instead. The permanent link names the sample,
 * since the newest changes.
 */
dashboard.get("/apps/:name/crashes/:id", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const found = await crashOf(c, app);
	if (!found) return c.text("No such crash", 404);
	const crash = summarizeCrash(found.group);
	const wanted = c.req.query("sample");
	const selected = wanted === undefined ? found.samples[0] ?? null : found.samples.find((s) => s.report_id === wanted) ?? null;
	if (wanted !== undefined && selected === null) return c.text("No such sample", 404);
	const loaded = selected === null ? null : await loadSample(c.env.BUCKET, app.name, selected, skipList);
	const sample = loaded?.ok ? loaded.sample : null;
	const problem = loaded !== null && !loaded.ok ? loaded.reason : null;
	let shared: SharedValues | null = null;
	if (sample !== null) {
		const others = await Promise.all(
			found.samples.filter((s) => s.report_id !== sample.report_id).map((s) => loadContext(c.env.BUCKET, s))
		);
		const read = others.filter((o) => o !== null);
		if (read.length > 0) shared = sharedValues(sample, read);
	}
	const path = crashPath(app.name, crash.id);
	const permalink = new URL(c.req.url).origin + path + (selected === null ? "" : `?sample=${selected.report_id}`);
	if (wantsJson(c)) {
		return c.json({ app, crash, permalink, releases: found.releases, users: found.users, sample, shared, problem, samples: found.samples });
	}
	const body = sample !== null ? sampleBody(path, sample, shared) : problem !== null ? sampleProblem(problem) : storedFrames(crash);
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "crashes", html`${crashHeader(crash)}
${versionsBlock(app, found.releases)}
${usersBlock(path, found.users, found.samples)}
${body}
${samplesSection(path, found.samples, selected?.report_id ?? null)}`, permalink));
});

/**
 * One object of a sample, the envelope or an attachment, as stored: with
 * its type and encoding, so a gzipped attachment inflates in the browser,
 * and as a download when it is not the envelope.
 */
dashboard.get("/apps/:name/crashes/:id/samples/:report/:file", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const found = await crashOf(c, app);
	if (!found) return c.text("No such crash", 404);
	const report = c.req.param("report");
	if (!found.samples.some((s) => s.report_id === report)) return c.text("No such sample", 404);
	const file = c.req.param("file");
	if (file !== ENVELOPE_OBJECT && attachmentKind(file, report) === null) return c.text("No such file", 404);
	const object = await c.env.BUCKET.get(sampleKey(app.name, report) + file);
	if (object === null) return c.text("No such file", 404);
	const headers = new Headers();
	object.writeHttpMetadata(headers);
	headers.set("Content-Length", String(object.size));
	if (file !== ENVELOPE_OBJECT) headers.set("Content-Disposition", `attachment; filename="${file}"`);
	return new Response(object.body, { headers, encodeBody: object.httpMetadata?.contentEncoding ? "manual" : "automatic" });
});
