import { html } from "hono/html";
import type { HtmlEscapedString } from "hono/utils/html";

/** Shared chrome of every dashboard page, following the system colour scheme. */
export function layout(title: string, who: string, body: HtmlEscapedString | Promise<HtmlEscapedString>) {
	return html`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${title} · crash-where</title>
<style>
:root { color-scheme: light dark; font: 16px/1.5 system-ui, sans-serif; }
body { max-width: 60rem; margin: 2rem auto; padding: 0 1rem; }
header { display: flex; justify-content: space-between; align-items: baseline; gap: 1rem; flex-wrap: wrap; }
header nav a { margin-right: 1rem; }
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 0.4rem 0.6rem; border-bottom: 1px solid light-dark(#ddd, #444); vertical-align: top; }
code { font-family: ui-monospace, monospace; }
form { display: flex; gap: 0.75rem; flex-wrap: wrap; align-items: end; margin-top: 2rem; }
label { display: grid; gap: 0.2rem; }
input { font: inherit; padding: 0.3rem 0.5rem; }
button { font: inherit; padding: 0.35rem 0.9rem; }
.muted { color: light-dark(#666, #999); }
.error { color: light-dark(#b00020, #ff6b6b); }
</style>
</head>
<body>
<header>
<nav><a href="/dashboard">Apps</a></nav>
<span class="muted">Signed in as ${who}</span>
</header>
<main>${body}</main>
</body>
</html>`;
}
