import { Hono } from "hono";
import { api } from "./api";
import { dashboard } from "./dashboard";
import { dev } from "./dev";
import type { App } from "./env";

const app = new Hono<App>();

app.get("/", (c) => c.redirect("/dashboard"));
app.route("/v1", api);
app.route("/dashboard", dashboard);
app.route("/dev", dev);

export default app;
