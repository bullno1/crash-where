import { Hono } from "hono";
import { html } from "hono/html";
import { type AppError, type AppInput, type AppRow, createApp, listApps, validateApp } from "../apps";
import type { App } from "../env";
import { layout } from "../layout";
import { type Page, render } from "../page";
import { day, fields, wantsJson } from "./common";

/** The app list, with the form that creates one. */
export const apps = new Hono<App>();

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

apps.get("/", async (c) => {
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
apps.post("/apps", async (c) => {
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
