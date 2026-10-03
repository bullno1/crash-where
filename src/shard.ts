import { DurableObject } from "cloudflare:workers";
import { Kysely } from "kysely";
import { shardMigrations } from "../migrations/shard";
import type { Env } from "./env";
import type { DB as ShardSchema } from "./shard.generated";
import { ShardDialect } from "./shard-dialect";

export type ShardDb = Kysely<ShardSchema>;

/** A version with where it is released and which builds it has. */
export interface VersionSummary {
	version: string;
	/** Unix seconds of the first upload. */
	created_at: number;
	channels: { channel: string; released_at: number; supported_until: number | null }[];
	builds: string[];
}

/** What an upload asks the shard to record. */
export interface ReleaseRequest {
	version: string;
	channel: string;
	/** Hex, as in the table header. */
	buildId: string;
	/** Unix seconds. */
	now: number;
}

/** What a registration wrote, or why it wrote nothing. */
export interface ReleaseResult {
	/** Set when the build id is already recorded under another version, which it names. */
	conflict?: string;
	/** Which rows this call inserted; all false for a repeat of an earlier upload. */
	created: { version: boolean; build: boolean; release: boolean };
}

/** Seconds a release keeps accepting reports after the next one on its channel. */
export const SUPPORT_WINDOW = 21 * 86400;

/**
 * One app's crash index, named after the app's `name`.
 * The object brings its storage up to date with migrations/shard on every
 * wake, before serving anything, so the code always sees the current
 * schema. Applied migrations are recorded in `shard_migrations`; object
 * storage refuses `PRAGMA user_version`. Migrations must stay additive: a
 * rolled-back Worker meets the newer schema on any shard that woke since.
 */
export class AppShard extends DurableObject<Env> {
	readonly db: ShardDb;

	constructor(ctx: DurableObjectState, env: Env) {
		super(ctx, env);
		this.db = new Kysely<ShardSchema>({ dialect: new ShardDialect(ctx.storage.sql) });
		ctx.blockConcurrencyWhile(async () => this.migrate());
	}

	/** Applies the migrations not yet recorded, each with its record in one transaction. */
	private migrate(): void {
		const sql = this.ctx.storage.sql;
		sql.exec(
			"CREATE TABLE IF NOT EXISTS shard_migrations (name TEXT PRIMARY KEY NOT NULL, applied_at INTEGER NOT NULL)"
		);
		const applied = new Set(sql.exec("SELECT name FROM shard_migrations").toArray().map((r) => r.name));
		for (const m of shardMigrations) {
			if (applied.has(m.name)) continue;
			this.ctx.storage.transactionSync(() => {
				sql.exec(m.sql);
				sql.exec(
					"INSERT INTO shard_migrations (name, applied_at) VALUES (?, ?)",
					m.name, Math.floor(Date.now() / 1000)
				);
			});
		}
	}

	/** Every version, most recently uploaded first, with its channel releases and build ids. */
	async listVersions(): Promise<VersionSummary[]> {
		const versions = await this.db
			.selectFrom("versions")
			.selectAll()
			.orderBy("created_at", "desc")
			.orderBy("version", "desc")
			.execute();
		const releases = await this.db.selectFrom("releases").selectAll().orderBy("channel").execute();
		const builds = await this.db.selectFrom("builds").select(["version", "build_id"]).orderBy("build_id").execute();
		return versions.map((v) => ({
			...v,
			channels: releases
				.filter((r) => r.version === v.version)
				.map(({ channel, released_at, supported_until }) => ({ channel, released_at, supported_until })),
			builds: builds.filter((b) => b.version === v.version).map((b) => b.build_id),
		}));
	}

	/**
	 * Records an uploaded build: its version if new, the build itself if
	 * new, and the release on the channel if new, in which case the channel's
	 * previous current release gets its support window. Storage queries
	 * complete without leaving the event loop, so the whole call is atomic.
	 */
	async registerRelease(req: ReleaseRequest): Promise<ReleaseResult> {
		const created = { version: false, build: false, release: false };
		const build = await this.db
			.selectFrom("builds")
			.select("version")
			.where("build_id", "=", req.buildId)
			.executeTakeFirst();
		if (build && build.version !== req.version) return { conflict: build.version, created };
		const version = await this.db
			.selectFrom("versions")
			.select("version")
			.where("version", "=", req.version)
			.executeTakeFirst();
		if (!version) {
			await this.db.insertInto("versions").values({ version: req.version, created_at: req.now }).execute();
			created.version = true;
		}
		if (!build) {
			await this.db
				.insertInto("builds")
				.values({ build_id: req.buildId, version: req.version, uploaded_at: req.now })
				.execute();
			created.build = true;
		}
		const release = await this.db
			.selectFrom("releases")
			.select("version")
			.where("channel", "=", req.channel)
			.where("version", "=", req.version)
			.executeTakeFirst();
		if (!release) {
			await this.db
				.updateTable("releases")
				.set({ supported_until: req.now + SUPPORT_WINDOW })
				.where("channel", "=", req.channel)
				.where("supported_until", "is", null)
				.execute();
			await this.db
				.insertInto("releases")
				.values({ channel: req.channel, version: req.version, released_at: req.now, supported_until: null })
				.execute();
			created.release = true;
		}
		return { created };
	}

	/** Names of the migrations this shard has applied, in order. */
	async appliedMigrations(): Promise<string[]> {
		return this.ctx.storage.sql
			.exec("SELECT name FROM shard_migrations ORDER BY name")
			.toArray()
			.map((r) => r.name as string);
	}
}
