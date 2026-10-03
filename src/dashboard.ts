import pico from "@picocss/pico/css/pico.classless.min.css";
import { Hono } from "hono";
import { html } from "hono/html";
import type { HtmlEscapedString } from "hono/utils/html";
import { type AppError, type AppInput, type AppRow, createApp, listApps, validateApp } from "./apps";
import { requireLogin } from "./auth";
import type { App } from "./env";
import { layout } from "./layout";

/** The dashboard: a login is required before any of its routes runs. */
export const dashboard = new Hono<App>();

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
	label: string, name: keyof AppInput, value: string,
	attrs: HtmlEscapedString | Promise<HtmlEscapedString>, error: AppError | null
) {
	const mine = error?.field === name;
	return html`<label>${label}
<input name="${name}" value="${value}" ${attrs} ${mine ? html`aria-invalid="true" aria-describedby="${name}-error"` : ""}>
${mine ? html`<small id="${name}-error">${error.message}</small>` : ""}
</label>`;
}

function appsPage(who: string, apps: AppRow[], form: AppInput, error: AppError | null) {
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

dashboard.get("/", async (c) => {
	const who = c.get("identity");
	const apps = await listApps(c.get("db"));
	return c.html(appsPage(who.email ?? who.sub, apps, { name: "", display_name: "" }, null));
});

dashboard.post("/apps", async (c) => {
	const who = c.get("identity");
	const body = await c.req.parseBody();
	const text = (key: string) => (typeof body[key] === "string" ? (body[key] as string).trim() : "");
	const input: AppInput = { name: text("name"), display_name: text("display_name") };
	const error = validateApp(input);
	if (error) {
		return c.html(appsPage(who.email ?? who.sub, await listApps(c.get("db")), input, error), 400);
	}
	const created = await createApp(c.get("db"), input, who);
	if (!created) {
		const taken: AppError = { field: "name", message: `An app named '${input.name}' already exists.` };
		return c.html(appsPage(who.email ?? who.sub, await listApps(c.get("db")), input, taken), 409);
	}
	return c.redirect("/dashboard", 303);
});
