import { html } from "hono/html";
import type { AppRow } from "../apps";
import { layout } from "../layout";
import type { Page } from "../page";
import { day } from "./common";

export function appPath(name: string): string {
	return `/dashboard/apps/${name}`;
}

export function tokensPath(name: string): string {
	return `${appPath(name)}/tokens`;
}

export function versionsPath(name: string): string {
	return `${appPath(name)}/versions`;
}

export function crashPath(name: string, id: number): string {
	return `${appPath(name)}/crashes/${id}`;
}

/** The pages of an app, in the order the sub-navigation lists them. */
const APP_PAGES = [
	{ key: "crashes", label: "Crashes", path: appPath },
	{ key: "versions", label: "Versions", path: versionsPath },
	{ key: "tokens", label: "Upload tokens", path: tokensPath },
] as const;

type AppPageKey = (typeof APP_PAGES)[number]["key"];

/**
 * One page of an app: its heading and details, the links to its other
 * pages with this one marked, then the section. `canonical` is the page's
 * permanent URL when the request's is not it.
 */
export function appPage(who: string, app: AppRow, current: AppPageKey, section: Page, canonical: string | null = null): Page {
	const links = APP_PAGES.map(
		(p) => html`<li><a href="${p.path(app.name)}"${p.key === current ? html` aria-current="page"` : ""}>${p.label}</a></li>`
	);
	const label = APP_PAGES.find((p) => p.key === current)!.label;
	return layout(
		current === "crashes" ? app.display_name : `${label} · ${app.display_name}`, who,
		html`<h1>${app.display_name}</h1>
<p><code>${app.name}</code> · ${app.disabled_at === null ? "active" : `disabled since ${day(app.disabled_at)}`} · created at ${day(app.created_at)} by ${app.created_by}</p>
<nav><ul>${links}</ul></nav>
${section}`,
		canonical
	);
}
