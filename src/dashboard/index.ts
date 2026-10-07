import pico from "@picocss/pico/css/pico.classless.min.css";
import { Hono } from "hono";
import { csrf } from "hono/csrf";
import { requireLogin } from "../auth";
import type { App } from "../env";
import { apps } from "./apps";
import { crash } from "./crash";
import { crashes } from "./crashes";
import { tokens } from "./tokens";
import { versions } from "./versions";

/**
 * The dashboard: a login is required before any of its routes runs, and a
 * form submission must come from this origin, which refuses forged posts
 * before the ambient credentials of either login mode can authorize them.
 */
export const dashboard = new Hono<App>();

dashboard.use("*", csrf());
dashboard.use("*", requireLogin);

dashboard.get("/pico.css", (c) => {
	c.header("Content-Type", "text/css; charset=utf-8");
	c.header("Cache-Control", "public, max-age=86400");
	return c.body(pico);
});

dashboard.route("/", apps);
dashboard.route("/", crashes);
dashboard.route("/", versions);
dashboard.route("/", tokens);
dashboard.route("/", crash);
