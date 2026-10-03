import { Hono } from "hono";
import { html } from "hono/html";
import { type AppInput, type AppRow, createApp, listApps, validateApp } from "./apps";
import { requireLogin } from "./auth";
import type { App } from "./env";
import { layout } from "./layout";

/** The dashboard: a login is required before any of its routes runs. */
export const dashboard = new Hono<App>();

dashboard.use("*", requireLogin);

function day(unix: number): string {
	return new Date(unix * 1000).toISOString().slice(0, 10);
}

function appsPage(who: string, apps: AppRow[], form: AppInput, error: string | null) {
	const rows = apps.map(
		(app) => html`<tr>
<td><a href="/dashboard/apps/${app.name}">${app.display_name}</a></td>
<td><code>${app.name}</code></td>
<td>${app.disabled_at === null ? "active" : html`<span class="muted">disabled</span>`}</td>
<td>${day(app.created_at)}</td>
<td>${app.created_by}</td>
</tr>`
	);
	const table =
		apps.length === 0
			? html`<p class="muted">No apps yet.</p>`
			: html`<table>
<thead><tr><th>App</th><th>Name</th><th>Status</th><th>Created</th><th>By</th></tr></thead>
<tbody>${rows}</tbody>
</table>`;
	return layout(
		"Apps", who,
		html`<h1>Apps</h1>
${table}
<form method="post" action="/dashboard/apps">
<label>Name <input name="name" value="${form.name}" required pattern="[a-z0-9_-]{1,63}" placeholder="forest-quest"></label>
<label>Display name <input name="display_name" value="${form.display_name}" required maxlength="100" placeholder="Forest Quest"></label>
<button>Create app</button>
${error ? html`<p class="error">${error}</p>` : ""}
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
	const field = (key: string) => (typeof body[key] === "string" ? (body[key] as string).trim() : "");
	const input: AppInput = { name: field("name"), display_name: field("display_name") };
	const error = validateApp(input);
	if (error) {
		return c.html(appsPage(who.email ?? who.sub, await listApps(c.get("db")), input, error), 400);
	}
	const created = await createApp(c.get("db"), input, who);
	if (!created) {
		const taken = `An app named '${input.name}' already exists.`;
		return c.html(appsPage(who.email ?? who.sub, await listApps(c.get("db")), input, taken), 409);
	}
	return c.redirect("/dashboard", 303);
});
