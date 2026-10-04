import { html } from "hono/html";
import type { Page } from "./page";

/**
 * Shared chrome of every dashboard page. Pico styles the elements and
 * follows the system colour scheme. `canonical` is the page's permanent
 * URL when one differs from the request's; null for none.
 */
export function layout(title: string, who: string, body: Page, canonical: string | null = null): Page {
	return html`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>${title} · crash-where</title>
<link rel="stylesheet" href="/dashboard/pico.css">
${canonical === null ? "" : html`<link rel="canonical" href="${canonical}">`}
</head>
<body>
<header>
<nav>
<ul><li><strong>crash-where</strong></li></ul>
<ul>
<li><a href="/dashboard">Apps</a></li>
<li><small>Signed in as ${who}</small></li>
</ul>
</nav>
</header>
<main>${body}</main>
</body>
</html>`;
}
