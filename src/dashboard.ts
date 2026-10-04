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
import { compileSkipList, DEFAULT_SKIP_LIST, groupTitle } from "./grouping";
import type { GroupSummary, VersionSummary } from "./shard";
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
function crashesSection(crashes: CrashSummary[]): Page {
	if (crashes.length === 0) return html`<h2>Crashes</h2>
<p>No crashes reported yet.</p>`;
	const rows = crashes.map(
		(g) => html`<tr>
<td>${groupTitle(g.fault, g.frames, g.message, skipList, true)}</td>
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

/** The versions of an app, newest first, with the channels they are released on and their builds. */
function versionsSection(versions: VersionSummary[]): Page {
	if (versions.length === 0) return html`<h2>Versions</h2>
<p>No versions yet. The first symbol upload for this app registers one.</p>`;
	const rows = versions.map(
		(v) => html`<tr>
<td><code>${v.version}</code></td>
<td>${v.channels.length === 0
	? html`<small>none</small>`
	: v.channels.map(
			(r) => html`<div>${r.channel} ${r.supported_until === null
				? html`<small>current</small>`
				: html`<small>until ${day(r.supported_until)}</small>`}</div>`
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

/** The pages of an app, in the order the sub-navigation lists them. */
const APP_PAGES = [
	{ key: "crashes", label: "Crashes", path: appPath },
	{ key: "versions", label: "Versions", path: (name: string) => `${appPath(name)}/versions` },
	{ key: "tokens", label: "Upload tokens", path: tokensPath },
] as const;

type AppPageKey = (typeof APP_PAGES)[number]["key"];

/** One page of an app: its heading and details, the links to its other pages with this one marked, then the section. */
function appPage(who: string, app: AppRow, current: AppPageKey, section: Page): Page {
	const links = APP_PAGES.map(
		(p) => html`<li><a href="${p.path(app.name)}"${p.key === current ? html` aria-current="page"` : ""}>${p.label}</a></li>`
	);
	const label = APP_PAGES.find((p) => p.key === current)!.label;
	return layout(
		current === "crashes" ? app.display_name : `${label} · ${app.display_name}`, who,
		html`<h1>${app.display_name}</h1>
<p><code>${app.name}</code> · ${app.disabled_at === null ? "active" : `disabled since ${day(app.disabled_at)}`} · created at ${day(app.created_at)} by ${app.created_by}</p>
<nav><ul>${links}</ul></nav>
${section}`
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
	return render(c, appPage(who.email ?? who.sub, app, "crashes", crashesSection(crashes)));
});

dashboard.get("/apps/:name/versions", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const versions = await shard.listVersions();
	if (wantsJson(c)) return c.json({ app, versions });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "versions", versionsSection(versions)));
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
