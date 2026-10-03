import { Hono } from "hono";
import { requireLogin } from "./auth";
import type { App } from "./env";

/** The dashboard: a login is required before any of its routes runs. */
export const dashboard = new Hono<App>();

dashboard.use("*", requireLogin);

dashboard.get("/", (c) => {
	const who = c.get("identity");
	return c.text(`Hello ${who.email ?? who.sub}`);
});
