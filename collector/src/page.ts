import type { Context } from "hono";
import type { ContentfulStatusCode } from "hono/utils/http-status";
import type { HtmlEscapedString } from "hono/utils/html";

/** A rendered page: only the `html` tag produces this type, so raw strings cannot reach the response. */
export type Page = HtmlEscapedString | Promise<HtmlEscapedString>;

/** The one place a page becomes a response. Handlers call this, never `c.html`. */
export function render(c: Context, page: Page, status?: ContentfulStatusCode): Promise<Response> {
	// eslint-disable-next-line no-restricted-syntax -- the wrapper the rule funnels every page through
	return Promise.resolve(c.html(page, status));
}
