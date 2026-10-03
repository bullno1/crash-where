import { Hono } from "hono";
import { api } from "./api";
import { dashboard } from "./dashboard";
import { createDb } from "./db";
import { dev } from "./dev";
import type { App } from "./env";

const app = new Hono<App>();

app.use("*", async (c, next) => {
	c.set("db", createDb(c.env.DB));
	await next();
});

app.get("/", (c) => c.redirect("/dashboard"));
app.route("/v1", api);
app.route("/dashboard", dashboard);
app.route("/dev", dev);

export default app;
