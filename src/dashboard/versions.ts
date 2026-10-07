import { Hono } from "hono";
import { html } from "hono/html";
import { getApp } from "../apps";
import type { App } from "../env";
import { type Page, render } from "../page";
import type { VersionSummary } from "../shard";
import { appPage } from "./app-page";
import { day, wantsJson } from "./common";

/** The versions page of an app. */
export const versions = new Hono<App>();

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

versions.get("/apps/:name/versions", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const versions = await shard.listVersions();
	if (wantsJson(c)) return c.json({ app, versions });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "versions", versionsSection(versions, c.req.query("version") ?? null)));
});
