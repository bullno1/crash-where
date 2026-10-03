import pico from "@picocss/pico/css/pico.classless.min.css";
import { type Context, Hono } from "hono";
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
	const error =
		validateApp(form) ??
		(apps.some((app) => app.name === form.name)
			? { field: "name" as const, message: `An app named '${form.name}' already exists.` }
			: null);
	return { form, error };
}

dashboard.get("/", async (c) => {
	const who = c.get("identity");
	const apps = await listApps(c.get("db"));
	const { form, error } = formState(c.req.query(), apps);
	return render(c, appsPage(who.email ?? who.sub, apps, form, error));
});

/** Every outcome redirects, so a refresh of the result never resubmits the form. */
dashboard.post("/apps", async (c) => {
	const body = await c.req.parseBody();
	const text = (key: string) => (typeof body[key] === "string" ? (body[key] as string).trim() : "");
	const input: AppInput = { name: text("name"), display_name: text("display_name") };
	const created = validateApp(input) === null && (await createApp(c.get("db"), input, c.get("identity")));
	if (!created) return c.redirect(`/dashboard?${new URLSearchParams({ ...input })}`, 303);
	return c.redirect("/dashboard", 303);
});

/** The upload tokens of an app, with the one just minted shown in clear. */
function tokensSection(app: AppRow, tokens: TokenRow[], fresh: string | null): Page {
	const rows = tokens.map(
		(t) => html`<tr>
<td>${t.label}</td>
<td>${day(t.created_at)}</td>
<td>${t.created_by}</td>
<td>${t.last_used_at === null ? html`<small>never</small>` : day(t.last_used_at)}</td>
<td>${t.revoked_at === null
	? html`<form method="post" action="/dashboard/apps/${app.name}/tokens/${t.id}/revoke"><button class="secondary">Revoke</button></form>`
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
<form method="post" action="/dashboard/apps/${app.name}/tokens">
<label>Label
<input name="label" required maxlength="${MAX_LABEL}" placeholder="GitHub Actions">
</label>
<button>Create upload token</button>
</form>`;
}

const skipList = compileSkipList(DEFAULT_SKIP_LIST);

/** The crashes of an app, most recently seen first, each named by its fault and frames. */
function crashesSection(groups: GroupSummary[]): Page {
	if (groups.length === 0) return html`<h2>Crashes</h2>
<p>No crashes reported yet.</p>`;
	const rows = groups.map(
		(g) => html`<tr>
<td>${groupTitle(g.fault, JSON.parse(g.frames) as RawFrame[], g.message, skipList)}</td>
<td>${g.count}</td>
<td>${day(g.first_seen)}</td>
<td>${day(g.last_seen)}</td>
</tr>`
	);
	return html`<h2>Crashes</h2>
<table>
<thead><tr><th>Crash</th><th>Reports</th><th>First seen</th><th>Last seen</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
}

function appPage(
	who: string, app: AppRow, versions: VersionSummary[], groups: GroupSummary[], tokens: TokenRow[], fresh: string | null
): Page {
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
	const table =
		versions.length === 0
			? html`<p>No versions yet. The first symbol upload for this app registers one.</p>`
			: html`<table>
<thead><tr><th>Version</th><th>Channels</th><th>Builds</th><th>Created</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
	return layout(
		app.display_name, who,
		html`<h1>${app.display_name}</h1>
<p><code>${app.name}</code> · ${app.disabled_at === null ? "active" : `disabled since ${day(app.disabled_at)}`} · created at ${day(app.created_at)} by ${app.created_by}</p>
${crashesSection(groups)}
<h2>Versions</h2>
${table}
${tokensSection(app, tokens, fresh)}`
	);
}

/**
 * A token just minted reaches the page that shows it through this cookie,
 * scoped to the app's page and cleared on the first read, so that it enters
 * neither a URL nor the browsing history and the form post can redirect
 * like every other.
 */
const FRESH_TOKEN_COOKIE = "cw_new_token";
const FRESH_TOKEN_SECONDS = 60;

function appPath(name: string): string {
	return `/dashboard/apps/${name}`;
}

dashboard.get("/apps/:name", async (c) => {
	const who = c.get("identity");
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const fresh = getCookie(c, FRESH_TOKEN_COOKIE) ?? null;
	if (fresh !== null) deleteCookie(c, FRESH_TOKEN_COOKIE, { path: appPath(app.name), secure: true });
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const versions = await shard.listVersions();
	const groups = await shard.listGroups();
	const tokens = await listTokens(c.get("db"), app.id);
	return render(c, appPage(who.email ?? who.sub, app, versions, groups, tokens, fresh));
});

dashboard.post("/apps/:name/tokens", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const body = await c.req.parseBody();
	const label = validLabel(typeof body.label === "string" ? body.label : "");
	if (label === null) return c.text(`A label of 1 to ${MAX_LABEL} characters is required`, 400);
	const { token } = await createToken(c.get("db"), app.id, label, c.get("identity"), Math.floor(Date.now() / 1000));
	setCookie(c, FRESH_TOKEN_COOKIE, token, {
		path: appPath(app.name), httpOnly: true, secure: true, sameSite: "Strict", maxAge: FRESH_TOKEN_SECONDS,
	});
	return c.redirect(appPath(app.name), 303);
});

dashboard.post("/apps/:name/tokens/:id/revoke", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	await revokeToken(c.get("db"), app.id, Number(c.req.param("id")), Math.floor(Date.now() / 1000));
	return c.redirect(appPath(app.name), 303);
});
