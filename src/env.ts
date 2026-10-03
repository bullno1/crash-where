/** Secrets the Worker reads; which ones are set selects the login mode. */
export interface Env {
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
export type App = { Bindings: Env; Variables: { identity: Identity } };

export function accessConfigured(env: Env): boolean {
	return Boolean(env.ACCESS_TEAM_DOMAIN && env.ACCESS_AUD);
}
