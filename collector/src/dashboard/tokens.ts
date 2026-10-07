import { Hono } from "hono";
import { deleteCookie, getCookie, setCookie } from "hono/cookie";
import { html } from "hono/html";
import { type AppRow, getApp } from "../apps";
import type { App } from "../env";
import { type Page, render } from "../page";
import { createToken, listTokens, MAX_LABEL, revokeToken, type TokenRow, validLabel } from "../tokens";
import { appPage, tokensPath } from "./app-page";
import { day, fields, wantsJson } from "./common";

/** The upload tokens page of an app. */
export const tokens = new Hono<App>();

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

/**
 * A token just minted reaches the page that shows it through this cookie,
 * scoped to the app's tokens page and cleared on the first read, so that it
 * enters neither a URL nor the browsing history and the form post can
 * redirect like every other.
 */
const FRESH_TOKEN_COOKIE = "cw_new_token";
const FRESH_TOKEN_SECONDS = 60;

tokens.get("/apps/:name/tokens", async (c) => {
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
tokens.post("/apps/:name/tokens", async (c) => {
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

tokens.post("/apps/:name/tokens/:id/revoke", async (c) => {
	const app = await getApp(c.get("db"), c.req.param("name"));
	if (!app) return c.text("No such app", 404);
	const revoked = await revokeToken(c.get("db"), app.id, Number(c.req.param("id")), Math.floor(Date.now() / 1000));
	if (wantsJson(c)) return c.json({ revoked }, revoked ? 200 : 404);
	return c.redirect(tokensPath(app.name), 303);
});
