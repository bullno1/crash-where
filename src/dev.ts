import { Hono } from "hono";
import { accessConfigured, type App } from "./env";

/** Helpers for a local `wrangler dev`; every route answers only on localhost. */
export const dev = new Hono<App>();

dev.use("*", async (c, next) => {
	const { hostname } = new URL(c.req.url);
	if (hostname !== "localhost" && hostname !== "127.0.0.1") return c.notFound();
	await next();
});

/**
 * Stores a token obtained with `cloudflared access token` as the cookie
 * Access itself would set, so a browser can use the local dashboard.
 * Exists only in Access mode; the token is still verified on every
 * dashboard request.
 */
dev.get("/login", (c) => {
	if (!accessConfigured(c.env)) return c.notFound();
	const token = c.req.query("token");
	if (!token) return c.text("Missing token", 400);
	c.header("Set-Cookie", `CF_Authorization=${token}; Path=/; HttpOnly; SameSite=Lax`);
	return c.redirect("/dashboard", 303);
});
