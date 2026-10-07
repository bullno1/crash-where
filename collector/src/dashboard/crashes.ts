import { Hono } from "hono";
import { html } from "hono/html";
import { type AppRow, getApp } from "../apps";
import type { App } from "../env";
import { compileSkipList, DEFAULT_SKIP_LIST, groupTitle } from "../grouping";
import { type Page, render } from "../page";
import type { GroupSummary } from "../shard";
import type { RawFrame } from "../symbols";
import { appPage, crashPath } from "./app-page";
import { day, wantsJson } from "./common";

/** The crash list of an app, which is also its front page. */
export const crashes = new Hono<App>();

export const skipList = compileSkipList(DEFAULT_SKIP_LIST);

/** A group as the page and JSON show it, with the title the rule gives it. */
export interface CrashSummary {
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

export function summarizeCrash(g: GroupSummary): CrashSummary {
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

crashes.get("/apps/:name", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const crashes = (await shard.listGroups(Math.floor(Date.now() / 1000))).map(summarizeCrash);
	if (wantsJson(c)) return c.json({ app, crashes });
	const who = c.get("identity");
	return render(c, appPage(who.email ?? who.sub, app, "crashes", crashesSection(app, crashes)));
});
