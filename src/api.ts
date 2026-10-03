import { Hono } from "hono";
import type { App } from "./env";

/** The client API, outside the dashboard login. */
export const api = new Hono<App>();

api.all("*", (c) => c.text("Not found", 404));
