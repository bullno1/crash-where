import { Hono } from "hono";
import { html } from "hono/html";
import {
	type AppError, type AppRow, getApp, MAX_DISPLAY_NAME, MAX_SAMPLE_CAP, parseSettings, setDisabled, type SettingsInput,
	updateSettings,
} from "../apps";
import type { App } from "../env";
import { type Page, render } from "../page";
import { appPage, settingsPath } from "./app-page";
import { day, field, fields, wantsJson } from "./common";

/** The settings page of an app: its editable columns and the kill switch. */
export const settings = new Hono<App>();

function settingsSection(app: AppRow, form: SettingsInput, error: AppError | null): Page {
	const cap = html`required type="number" inputmode="numeric" min="0" max="${MAX_SAMPLE_CAP}" step="1"`;
	const path = settingsPath(app.name);
	return html`<h2>Settings</h2>
<form method="post" action="${path}">
<h3 id="display-name">Display name</h3>
${field(null, "display_name", form.display_name, html`required maxlength="${MAX_DISPLAY_NAME}" aria-labelledby="display-name"`, error)}
<h3>Sample caps</h3>
<p>How many full reports to keep per crash, version and trust level.</p>
${field("Authorized reports", "sample_cap_trusted", form.sample_cap_trusted, cap, error)}
${field("Unauthorized reports", "sample_cap_untrusted", form.sample_cap_untrusted, cap, error)}
<button>Save</button>
</form>
<h3>Kill switch</h3>
${app.disabled_at === null
	? html`<p>The app accepts reports and symbol uploads.</p>
<form method="post" action="${path}/disable"><button class="secondary">Disable app</button></form>`
	: html`<p>Disabled since ${day(app.disabled_at)}: reports and symbol uploads are refused.</p>
<form method="post" action="${path}/enable"><button>Enable app</button></form>`}`;
}

/** The form's fields as the row holds them. */
function currentSettings(app: AppRow): SettingsInput {
	return {
		display_name: app.display_name,
		sample_cap_trusted: String(app.sample_cap_trusted),
		sample_cap_untrusted: String(app.sample_cap_untrusted),
	};
}

/**
 * Rebuilds the form from the query string, where a failed submission left
 * its values, and works out what was wrong with them. A query without form
 * fields shows the stored settings.
 */
function formState(query: Record<string, string>, app: AppRow): { form: SettingsInput; error: AppError | null } {
	const stored = currentSettings(app);
	if (!Object.keys(stored).some((key) => key in query)) return { form: stored, error: null };
	const form: SettingsInput = {
		display_name: query.display_name ?? "",
		sample_cap_trusted: query.sample_cap_trusted ?? "",
		sample_cap_untrusted: query.sample_cap_untrusted ?? "",
	};
	const parsed = parseSettings(form);
	return { form, error: "error" in parsed ? parsed.error : null };
}

settings.get("/apps/:name/settings", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	if (wantsJson(c)) return c.json({ app });
	const { form, error } = formState(c.req.query(), app);
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "settings", settingsSection(app, form, error)));
});

/**
 * Every outcome of the form redirects back to the page, so a refresh never
 * resubmits it. A JSON client gets the updated row, or the error on the
 * field at fault.
 */
settings.post("/apps/:name/settings", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const body = await fields(c);
	const input: SettingsInput = {
		display_name: body.display_name ?? "",
		sample_cap_trusted: body.sample_cap_trusted ?? "",
		sample_cap_untrusted: body.sample_cap_untrusted ?? "",
	};
	const parsed = parseSettings(input);
	if ("error" in parsed) {
		if (wantsJson(c)) return c.json({ error: parsed.error }, 400);
		return c.redirect(`${settingsPath(app.name)}?${new URLSearchParams({ ...input })}`, 303);
	}
	const updated = await updateSettings(c.get("db"), app.id, parsed.settings);
	if (wantsJson(c)) return c.json({ app: updated });
	return c.redirect(settingsPath(app.name), 303);
});

settings.post("/apps/:name/settings/disable", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const at = app.disabled_at ?? Math.floor(Date.now() / 1000);
	const updated = await setDisabled(c.get("db"), app.id, at);
	if (wantsJson(c)) return c.json({ app: updated });
	return c.redirect(settingsPath(app.name), 303);
});

settings.post("/apps/:name/settings/enable", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const updated = await setDisabled(c.get("db"), app.id, null);
	if (wantsJson(c)) return c.json({ app: updated });
	return c.redirect(settingsPath(app.name), 303);
});
