import { type Context, Hono } from "hono";
import { html } from "hono/html";
import { type AppRow, getApp } from "../apps";
import type { Location } from "../cwsym";
import type { App } from "../env";
import { keptFrames } from "../grouping";
import { type Page, render } from "../page";
import { loadContext, loadSample, type SampleError, type SampleView, sharedValues, type SharedValues } from "../sample";
import { attachmentKind, ENVELOPE_OBJECT, sampleKey } from "../samples";
import type { AppShard, GroupRelease, GroupUsers, SampleSummary } from "../shard";
import { relativeSource, sourceLink } from "../source-link";
import type { RawFrame } from "../symbols";
import { appPage, crashPath, versionsPath } from "./app-page";
import { when, wantsJson } from "./common";
import { type CrashSummary, skipList, summarizeCrash } from "./crashes";

/** One crash of an app: its samples and their files. */
export const crash = new Hono<App>();

/** Hex as a debugger prints it. */
function hex(n: number): string {
	return `0x${n.toString(16)}`;
}

/** A source location as `file:line`, the file alone when the line is unknown, or nothing; a link when the app has one for it. */
function where(loc: Location, link: string | null): Page {
	if (loc.file === null) return html``;
	const text = html`<code>${loc.file}${loc.line === 0 ? "" : `:${loc.line}`}</code>`;
	return link === null ? text : html`<a href="${link}">${text}</a>`;
}

/**
 * Where each location of a sample can be read, aligned with its
 * `locations`: the app's template filled with the commit and version of
 * the frame's build, the file relative to the build's source root and the
 * line. Null for a location the template cannot be filled for, and
 * everywhere when the app has no template.
 */
async function sourceLinks(shard: DurableObjectStub<AppShard>, app: AppRow, sample: SampleView): Promise<(string | null)[][]> {
	const template = app.source_link_template;
	if (template === null) return sample.locations.map((locs) => locs.map(() => null));
	const ids = new Set<string>();
	for (const [i, f] of sample.frames.entries()) {
		if (f.buildId != null && sample.locations[i]!.length > 0) ids.add(f.buildId);
	}
	const builds = new Map((await shard.getBuilds([...ids])).map((b) => [b.build_id, b]));
	return sample.locations.map((locs, i) => {
		const build = builds.get(sample.frames[i]!.buildId ?? "");
		return locs.map((loc) => {
			if (build === undefined || loc.file === null) return null;
			return sourceLink(template, {
				commit: build.source_commit ?? undefined,
				version: build.version,
				file: relativeSource(loc.file, build.source_root),
				line: loc.line === 0 ? undefined : String(loc.line),
			});
		});
	});
}

/**
 * A stack as a table, the frames the fingerprint took marked. With
 * `locations`, a Location column gives each frame's file and line, the
 * function is its display name, and a frame inside inlined code takes
 * one row per inline level, innermost first, the outer levels indented
 * under it, each location a link where `links` has one for it. A frame
 * no table locates shows its `traceLine` there when it has one, which is
 * the browser's own line for a JavaScript frame.
 */
function stackTable(
	frames: RawFrame[], hashed: number[], traceLine: (string | null)[] = [],
	locations: Location[][] | null = null, links: (string | null)[][] = []
): Page {
	const marked = new Set(hashed);
	const rows = frames.flatMap((f, i) => {
		const locs = locations?.[i] ?? [];
		const link = (level: number) => links[i]?.[level] ?? null;
		const plain = f.name === null ? html`<small>unnamed</small>` : html`${f.name}`;
		const name = locs.length === 0 ? plain : html`${locs[0]!.function}`;
		const line = traceLine[i] ?? null;
		const location = locs.length > 0 ? where(locs[0]!, link(0)) : line === null ? html`` : html`<code>${line}</code>`;
		const first = html`<tr>
<td>${i}</td>
<td>${marked.has(i) ? html`<mark>${name}</mark>` : name}</td>${locations === null ? "" : html`
<td>${location}</td>`}
<td><code>${f.module}</code></td>
<td>${f.offset === undefined ? "" : html`<code>${hex(f.offset)}</code>`}</td>
</tr>`;
		const outer = locs.slice(1).map((loc, level) => html`<tr>
<td></td>
<td>&nbsp;&nbsp;&nbsp;&nbsp;↳ ${marked.has(i) ? html`<mark>${loc.function}</mark>` : loc.function}</td>
<td>${where(loc, link(level + 1))}</td>
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
function sampleBody(path: string, s: LinkedSample, shared: SharedValues | null): Page {
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
${stackTable(s.frames, s.hashed, s.trace_line, s.locations, s.links)}
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

/** A sample with where each of its locations can be read, aligned with `locations`. */
type LinkedSample = SampleView & { links: (string | null)[][] };

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
 * its permanent URL, each source location linked where the app's template
 * applies. The state and env of the other samples are read too,
 * so each value can say how many samples share it; with fewer than two
 * envelopes readable the comparison is left out. A group without samples
 * shows its stored frames instead. The permanent link names the sample,
 * since the newest changes.
 */
crash.get("/apps/:name/crashes/:id", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const found = await crashOf(c, app);
	if (!found) return c.text("No such crash", 404);
	const crash = summarizeCrash(found.group);
	const wanted = c.req.query("sample");
	const selected = wanted === undefined ? found.samples[0] ?? null : found.samples.find((s) => s.report_id === wanted) ?? null;
	if (wanted !== undefined && selected === null) return c.text("No such sample", 404);
	const loaded = selected === null ? null : await loadSample(c.env.BUCKET, app.name, selected, skipList);
	const shard = c.env.SHARD.get(c.env.SHARD.idFromName(app.name));
	const sample: LinkedSample | null = loaded?.ok ? { ...loaded.sample, links: await sourceLinks(shard, app, loaded.sample) } : null;
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
crash.get("/apps/:name/crashes/:id/samples/:report/:file", async (c) => {
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
