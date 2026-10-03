import type { Db } from "./db";
import type { AppShard } from "./shard";

/** Bindings and secrets the Worker reads; which secrets are set selects the login mode. */
export interface Env {
	DB: D1Database;
	/** One Durable Object per app, addressed by the app's `name`. */
	SHARD: DurableObjectNamespace<AppShard>;
	ACCESS_TEAM_DOMAIN?: string;
	ACCESS_AUD?: string;
	DASHBOARD_PASSWORD?: string;
}

/** Who is using the dashboard, whichever login mode admitted them. */
export interface Identity {
	sub: string;
	email?: string;
}

/** Hono type parameter shared by every sub-app. */
export type App = { Bindings: Env; Variables: { identity: Identity; db: Db } };

export function accessConfigured(env: Env): boolean {
	return Boolean(env.ACCESS_TEAM_DOMAIN && env.ACCESS_AUD);
}
