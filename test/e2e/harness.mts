/**
 * The collector under `wrangler dev` with its state in a temporary
 * directory, the dashboard in password mode, and the requests a script
 * makes to it. The end-to-end tests drive the native binaries against it.
 */
import { type ChildProcess, spawn, spawnSync } from "node:child_process";
import { closeSync, openSync } from "node:fs";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import path from "node:path";
import { setTimeout as sleep } from "node:timers/promises";

/** The repository, two levels above this directory's parent. */
export const ROOT = path.resolve(import.meta.dirname, "../../..");
const COLLECTOR = path.join(ROOT, "collector");
const WRANGLER = path.join(COLLECTOR, "node_modules/.bin/wrangler");
const PASSWORD = "e2e-correct-horse-battery";

/** A built binary, from the directory `CW_BIN_DIR` names. */
export function binary(name: string): string {
	const dir = process.env.CW_BIN_DIR;
	if (!dir) throw new Error("CW_BIN_DIR must name the directory holding the built binaries");
	return path.resolve(dir, name);
}

/** Runs a program to completion and returns its exit status with its combined output. */
export function run(file: string, args: string[], env: Record<string, string> = {}): { status: number; output: string } {
	const r = spawnSync(file, args, { encoding: "utf8", env: { ...process.env, ...env } });
	if (r.error) throw r.error;
	return { status: r.status ?? -1, output: r.stdout + r.stderr };
}

/**
 * Runs a program with its output appended to the file, which a process it
 * leaves behind keeps writing to after it exits. The status is not
 * checked: the caller may expect a signal.
 */
export function runLogging(file: string, args: string[], logPath: string): void {
	const log = openSync(logPath, "a");
	try {
		const r = spawnSync(file, args, { stdio: ["ignore", log, log] });
		if (r.error) throw r.error;
	} finally {
		closeSync(log);
	}
}

/** Wrangler colors its log; the escape sequences come off before a line is matched. */
function uncolored(text: string): string {
	return text.replace(/\x1b\[[0-9;]*m/g, "");
}

async function freePort(): Promise<number> {
	return new Promise((resolve, reject) => {
		const server = createServer();
		server.on("error", reject);
		server.listen(0, "127.0.0.1", () => {
			const { port } = server.address() as { port: number };
			server.close(() => resolve(port));
		});
	});
}

export class Collector {
	readonly endpoint: string;
	/** Scratch directory: the collector's state, its log, and whatever a test adds. */
	readonly work: string;
	private readonly proc: ChildProcess;

	private constructor(endpoint: string, work: string, proc: ChildProcess) {
		this.endpoint = endpoint;
		this.work = work;
		this.proc = proc;
	}

	/** Migrates a fresh local database and starts the dev server on a free port. */
	static async start(): Promise<Collector> {
		const work = await mkdtemp(path.join(tmpdir(), "cw-e2e-"));
		const state = path.join(work, "state");
		const env = { ...process.env, WRANGLER_SEND_METRICS: "false" };
		const migrate = spawnSync(WRANGLER, ["d1", "migrations", "apply", "DB", "--local", "--persist-to", state], {
			cwd: COLLECTOR, env, encoding: "utf8",
		});
		if (migrate.status !== 0) throw new Error(`migrations failed:\n${migrate.stdout}${migrate.stderr}`);

		const port = await freePort();
		// The inspector gets its own free port too, or two dev servers, this
		// one and a developer's, would collide on the default.
		const inspector = await freePort();
		const endpoint = `http://127.0.0.1:${port}`;
		const log = openSync(path.join(work, "wrangler.log"), "w");
		// The empty Access values override a developer's .dev.vars, which would
		// otherwise put the dashboard behind Access instead of the password.
		const proc = spawn(WRANGLER, [
			"dev", "--ip", "127.0.0.1", "--port", String(port), "--inspector-port", String(inspector), "--persist-to", state,
			"--var", "ACCESS_TEAM_DOMAIN:", "--var", "ACCESS_AUD:", "--var", `DASHBOARD_PASSWORD:${PASSWORD}`,
			"--show-interactive-dev-session", "false",
		], { cwd: COLLECTOR, env, stdio: ["ignore", log, log] });
		closeSync(log);
		const collector = new Collector(endpoint, work, proc);
		for (let i = 0; i < 120; ++i) {
			if (proc.exitCode !== null) throw new Error(`wrangler dev exited:\n${await collector.log()}`);
			const code = await fetch(`${endpoint}/`, { redirect: "manual" }).then((r) => r.status, () => 0);
			if (code === 302) return collector;
			await sleep(500);
		}
		await collector.stop(true);
		throw new Error(`the collector did not come up on ${endpoint}`);
	}

	/** Kills the dev server and removes the scratch directory; a failed run prints the log first. */
	async stop(failed: boolean): Promise<void> {
		if (this.proc.exitCode === null) {
			const exited = new Promise<void>((resolve) => this.proc.once("exit", () => resolve()));
			this.proc.kill();
			await exited;
		}
		if (failed) console.error(`e2e: failed; wrangler log follows\n${await this.log()}`);
		await rm(this.work, { recursive: true, force: true });
	}

	/** The dev server's log so far, without colors. */
	async log(): Promise<string> {
		return uncolored(await readFile(path.join(this.work, "wrangler.log"), "utf8").catch(() => ""));
	}

	/**
	 * A dashboard request as a script makes it: password login, this origin,
	 * and a preference for JSON. A body is sent as a JSON object.
	 */
	fetch(route: string, body?: Record<string, string>): Promise<Response> {
		const headers: Record<string, string> = {
			Authorization: `Basic ${Buffer.from(`alice:${PASSWORD}`).toString("base64")}`,
			Origin: this.endpoint,
			Accept: "application/json",
		};
		if (body !== undefined) headers["Content-Type"] = "application/json";
		return fetch(`${this.endpoint}${route}`, {
			method: body === undefined ? "GET" : "POST",
			headers,
			body: body === undefined ? undefined : JSON.stringify(body),
		});
	}

	/** A dashboard request whose reply must be JSON with the given status. */
	async json<T>(route: string, status: number, body?: Record<string, string>): Promise<T> {
		const r = await this.fetch(route, body);
		const text = await r.text();
		if (r.status !== status) throw new Error(`${body ? "POST" : "GET"} ${route}: HTTP ${r.status}, expected ${status}\n${text}`);
		return JSON.parse(text) as T;
	}

	/** Creates the app through the dashboard and mints an upload token for it. */
	async createApp(name: string, display: string): Promise<string> {
		await this.json(`/dashboard/apps`, 201, { name, display_name: display });
		const minted = await this.json<{ token: string }>(`/dashboard/apps/${name}/tokens`, 201, { label: "e2e" });
		return minted.token;
	}
}

/** What the pages of an app return as JSON; the fields the tests read. */
export interface CrashesPage {
	app: { name: string; display_name: string; disabled_at: number | null };
	crashes: {
		id: number;
		title: string;
		fault: string;
		message: string | null;
		frames: { module: string; name: string | null }[];
		count: number;
		recent_users: number;
		urgency: number;
	}[];
}

export interface VersionsPage {
	versions: {
		version: string;
		channels: { channel: string; supported_until: number | null }[];
		builds: string[];
	}[];
}

export interface TokensPage {
	tokens: { id: number; label: string; last_used_at: number | null }[];
}
