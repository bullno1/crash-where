import { Hono } from "hono";
import { html } from "hono/html";
import {
	type AppError, type AppRow, getApp, MAX_DISPLAY_NAME, MAX_SAMPLE_CAP, parseSettings, setDisabled, type SettingsInput,
	updateSettings,
} from "../apps";
import { ALL_ORIGINS, MAX_CORS_ORIGINS } from "../cors";
import type { App } from "../env";
import { type Page, render } from "../page";
import { MAX_SOURCE_LINK_TEMPLATE } from "../source-link";
import { appPage, settingsPath } from "./app-page";
import { day, field, fields, textarea, wantsJson } from "./common";

/** The settings page of an app: its editable columns and the kill switch. */
export const settings = new Hono<App>();

function settingsSection(app: AppRow, form: SettingsInput, error: AppError | null): Page {
	const cap = html`required type="number" inputmode="numeric" min="0" max="${MAX_SAMPLE_CAP}" step="1"`;
	const path = settingsPath(app.name);
	const allowAll = form.cors_origins === ALL_ORIGINS;
	return html`<h2>Settings</h2>
<form method="post" action="${path}">
<h3 id="display-name">Display name</h3>
${field(null, "display_name", form.display_name, html`required maxlength="${MAX_DISPLAY_NAME}" aria-labelledby="display-name"`, error)}
<h3>Sample caps</h3>
<p>How many full reports to keep per crash, version and trust level.</p>
${field("Authorized reports", "sample_cap_trusted", form.sample_cap_trusted, cap, error)}
${field("Unauthorized reports", "sample_cap_untrusted", form.sample_cap_untrusted, cap, error)}
<h3 id="source-link">Source link</h3>
<p>Where a crash's source locations link to. This is a URI template (RFC 6570) with:
<ul>
<li><code>{commit}</code>: The commit hash</li>
<li><code>{version}</code>: The version in the symbol file</li>
<li><code>{+file}</code>: Relative to the upload's source root</li>
<li><code>{line}</code>: Resolved line number</li>
</ul>
Set to empty for no links.
A location is linked only when every variable the template names is known.
</p>
<p><small>GitHub: <code>https://github.com/org/repo/blob/{commit}/{+file}#L{line}</code><br>GitLab: <code>https://gitlab.com/org/repo/-/blob/{commit}/{+file}#L{line}</code></small></p>
${field(null, "source_link_template", form.source_link_template, html`maxlength="${MAX_SOURCE_LINK_TEMPLATE}" aria-labelledby="source-link" placeholder="https://github.com/org/repo/blob/{commit}/{+file}#L{line}"`, error)}
<h3 id="cors-origins">Web origins</h3>
<p>Which pages may send reports from a web build, as the browser sends them in <code>Origin</code>.
One origin per line, <code>scheme://host[:port]</code>, where <code>*</code> matches anything.
Leave empty to accept reports only from native builds.</p>
<style>label:has(#cors-all:checked) ~ textarea, label:has(#cors-all:checked) ~ small { display: none; }</style>
<label><input type="checkbox" id="cors-all" name="cors_allow_all" ${allowAll ? "checked" : ""}> Allow all</label>
${textarea("cors_origins", allowAll ? "" : form.cors_origins, html`rows="4" maxlength="${MAX_CORS_ORIGINS}" aria-labelledby="cors-origins" placeholder="https://game.example.com&#10;https://*.itch.io"`, error)}
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
		source_link_template: app.source_link_template ?? "",
		cors_origins: app.cors_origins ?? "",
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
		source_link_template: query.source_link_template ?? "",
		cors_origins: query.cors_origins ?? "",
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
 * field at fault. The "allow all" checkbox stands for the `*` setting,
 * which a JSON client sends as the field itself.
 */
settings.post("/apps/:name/settings", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const body = await fields(c);
	const input: SettingsInput = {
		display_name: body.display_name ?? "",
		sample_cap_trusted: body.sample_cap_trusted ?? "",
		sample_cap_untrusted: body.sample_cap_untrusted ?? "",
		source_link_template: body.source_link_template ?? "",
		cors_origins: body.cors_allow_all !== undefined ? ALL_ORIGINS : body.cors_origins ?? "",
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
