import { DurableObject } from "cloudflare:workers";
import { Kysely, sql } from "kysely";
import { shardMigrations } from "../migrations/shard";
import type { Env } from "./env";
import { deleteSample, samplesPrefix } from "./samples";
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

/** A report after the Worker grouped it, ready to count. */
export interface IngestRequest {
	reportId: string;
	version: string;
	channel: string;
	/** 0 until the auth route exists. */
	trust: number;
	/** Who crashed, in the key space `trust` names: the install id for 0, the token's sub for 1. */
	userKey: string;
	/** Unix seconds. */
	now: number;
	group: {
		fingerprint: string;
		fault: string;
		/** JSON of the raw frames the rule saw. */
		frames: string;
		message: string | null;
	};
	/** Samples the app keeps per bucket at this trust level; 0 keeps none. */
	sampleCap: number;
	/** R2 prefix of this report's objects, should it be sampled. */
	sampleKey: string;
	/** A uniform draw in [0, 1) that decides the sample once the bucket is full. */
	draw: number;
}

/**
 * How a report was received. `unknown` and `expired` describe the
 * release; `duplicate` is a retry of a counted report; `counted` names
 * the group the report went to. `sampled` says whether the report's
 * objects belong in the bucket: for a retry, whether they were wanted
 * the first time, so the client sends what is left. `evicted` is the
 * prefix of the sample this one replaced, whose objects must go.
 */
export type IngestResult =
	| { outcome: "unknown" | "expired" }
	| { outcome: "duplicate"; sampled: boolean }
	| { outcome: "counted"; groupId: number; sampled: boolean; evicted: string | null };

/** What a remap changes on one group: its frames as named now and the hash they give. */
export interface GroupChange {
	id: number;
	frames: string;
	fingerprint: string;
}

/** A group as stored, for a remap to name again. */
export interface StoredGroup {
	id: number;
	fingerprint: string;
	fault: string;
	frames: string;
	message: string | null;
}

/** A group with its totals and urgency, for the app page. */
export interface GroupSummary {
	id: number;
	fault: string;
	/** JSON of the raw frames, as stored. */
	frames: string;
	message: string | null;
	first_seen: number;
	last_seen: number;
	/** Reports counted, over every version, channel, trust level and day. */
	count: number;
	/** Reports received within the urgency window. */
	recent_count: number;
	/** Distinct users those reports came from. */
	recent_users: number;
	/**
	 * The square root of `recent_count` times `recent_users`, rounded: the
	 * users affected, scaled by the square root of how often each is hit.
	 */
	urgency: number;
}

/** What one run of the purge removed. */
export interface PurgeResult {
	/** Releases past their window whose reports, counts and samples went. */
	releases: { channel: string; version: string }[];
	/** Sample prefixes in the bucket that no row claimed. */
	orphans: number;
}

/** Seconds a release keeps accepting reports after the next one on its channel. */
export const SUPPORT_WINDOW = 21 * 86400;

/** Seconds between sweeps of the app's sample prefix while the app holds samples. */
export const SWEEP_INTERVAL = 86400;

/** Seconds of reports the urgency of a group is computed over. */
export const URGENCY_WINDOW = 7 * 86400;

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
		ctx.blockConcurrencyWhile(async () => {
			this.migrate();
			await this.schedule(Math.floor(Date.now() / 1000));
		});
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
			await this.schedule(req.now);
		}
		return { created };
	}

	/**
	 * Counts a report: refuses one for a release that is unknown or past
	 * its window, counts a retried report id once, finds or creates the
	 * group, records the report under it with its user key, adds one to
	 * the day's count, and decides whether to sample it. Atomic as
	 * `registerRelease`.
	 *
	 * A bucket is a group, a version and a trust level, and keeps up to
	 * the group's own cap or, without one, the app's for that trust. While
	 * the bucket holds fewer samples than its cap every report is kept, so
	 * a raised cap fills at once; after that the `k`th report of the bucket
	 * is kept with probability `cap / k` and replaces the sample the draw
	 * points at, so the bucket stays a uniform sample of its whole life.
	 */
	async ingest(req: IngestRequest): Promise<IngestResult> {
		const release = await this.db
			.selectFrom("releases")
			.select("supported_until")
			.where("channel", "=", req.channel)
			.where("version", "=", req.version)
			.executeTakeFirst();
		if (!release) return { outcome: "unknown" };
		if (release.supported_until !== null && release.supported_until < req.now) return { outcome: "expired" };
		const seen = await this.db
			.selectFrom("reports")
			.select("report_id")
			.where("report_id", "=", req.reportId)
			.executeTakeFirst();
		if (seen) {
			const sample = await this.db
				.selectFrom("crash_samples")
				.select("id")
				.where("report_id", "=", req.reportId)
				.executeTakeFirst();
			return { outcome: "duplicate", sampled: sample !== undefined };
		}

		const existing = await this.db
			.selectFrom("crash_groups")
			.select(["id", "sample_cap"])
			.where("fingerprint", "=", req.group.fingerprint)
			.executeTakeFirst();
		let groupId: number;
		const cap = existing?.sample_cap ?? req.sampleCap;
		if (existing) {
			groupId = existing.id;
			await this.db.updateTable("crash_groups").set({ last_seen: req.now }).where("id", "=", groupId).execute();
		} else {
			const row = await this.db
				.insertInto("crash_groups")
				.values({ ...req.group, first_seen: req.now, last_seen: req.now })
				.returning("id")
				.executeTakeFirstOrThrow();
			groupId = row.id;
		}
		await this.db
			.insertInto("reports")
			.values({
				report_id: req.reportId,
				group_id: groupId,
				version: req.version,
				channel: req.channel,
				trust: req.trust,
				user_key: req.userKey,
				received_at: req.now,
			})
			.execute();
		await this.db
			.insertInto("crash_counts")
			.values({
				group_id: groupId,
				version: req.version,
				channel: req.channel,
				trust: req.trust,
				day: Math.floor(req.now / 86400),
				count: 1,
			})
			.onConflict((oc) =>
				oc.columns(["group_id", "version", "channel", "trust", "day"]).doUpdateSet({ count: sql`count + 1` })
			)
			.execute();

		const { sampled, evicted } = await this.sample(req, groupId, cap);
		await this.schedule(req.now);
		return { outcome: "counted", groupId, sampled, evicted };
	}

	/** The reservoir step of `ingest`: whether the report is kept in its bucket, and whose place it takes. */
	private async sample(req: IngestRequest, groupId: number, cap: number): Promise<{ sampled: boolean; evicted: string | null }> {
		if (cap <= 0) return { sampled: false, evicted: null };
		const bucket = await this.db
			.selectFrom("crash_counts")
			.select((eb) => eb.fn.coalesce(eb.fn.sum<number>("count"), sql<number>`0`).as("k"))
			.where("group_id", "=", groupId)
			.where("version", "=", req.version)
			.where("trust", "=", req.trust)
			.executeTakeFirstOrThrow();
		const k = Number(bucket.k);
		const held = await this.db
			.selectFrom("crash_samples")
			.select((eb) => eb.fn.countAll<number>().as("n"))
			.where("group_id", "=", groupId)
			.where("version", "=", req.version)
			.where("trust", "=", req.trust)
			.executeTakeFirstOrThrow();
		let evicted: string | null = null;
		if (Number(held.n) >= cap) {
			const slot = Math.floor(req.draw * k);
			if (slot >= cap) return { sampled: false, evicted: null };
			const old = await this.db
				.selectFrom("crash_samples")
				.select(["id", "r2_key"])
				.where("group_id", "=", groupId)
				.where("version", "=", req.version)
				.where("trust", "=", req.trust)
				.orderBy("id")
				.offset(slot)
				.limit(1)
				.executeTakeFirst();
			if (old) {
				await this.db.deleteFrom("crash_samples").where("id", "=", old.id).execute();
				evicted = old.r2_key;
			}
		}
		await this.db
			.insertInto("crash_samples")
			.values({
				report_id: req.reportId,
				group_id: groupId,
				version: req.version,
				trust: req.trust,
				r2_key: req.sampleKey,
				received_at: req.now,
			})
			.execute();
		return { sampled: true, evicted };
	}

	/**
	 * Deletes what the app no longer keeps: every release past its window
	 * loses its reports, counts and samples, the objects of each sample
	 * before its row, and the sweep then removes the objects under the
	 * app's sample prefix that no sample row claims. Safe to repeat: a
	 * release with no reports left is done, and a run that fails midway
	 * leaves rows that the next run reaches again.
	 */
	async purge(now: number): Promise<PurgeResult> {
		const releases = await this.db
			.selectFrom("releases")
			.select(["channel", "version"])
			.where("supported_until", "<", now)
			.where((eb) =>
				eb.exists(
					eb.selectFrom("reports")
						.select("report_id")
						.whereRef("reports.version", "=", "releases.version")
						.whereRef("reports.channel", "=", "releases.channel")
				)
			)
			.orderBy("supported_until")
			.execute();
		for (const release of releases) await this.purgeRelease(release);
		return { releases, orphans: await this.sweep() };
	}

	private async purgeRelease(release: { channel: string; version: string }): Promise<void> {
		const samples = await this.db
			.selectFrom("crash_samples")
			.innerJoin("reports", "reports.report_id", "crash_samples.report_id")
			.select("crash_samples.r2_key")
			.where("reports.version", "=", release.version)
			.where("reports.channel", "=", release.channel)
			.execute();
		for (const sample of samples) await deleteSample(this.env.BUCKET, sample.r2_key);
		this.ctx.storage.transactionSync(() => {
			const sql = this.ctx.storage.sql;
			sql.exec(
				"DELETE FROM crash_samples WHERE report_id IN (SELECT report_id FROM reports WHERE version = ? AND channel = ?)",
				release.version, release.channel
			);
			sql.exec("DELETE FROM reports WHERE version = ? AND channel = ?", release.version, release.channel);
			sql.exec("DELETE FROM crash_counts WHERE version = ? AND channel = ?", release.version, release.channel);
		});
	}

	/**
	 * Deletes the objects under the app's sample prefix whose report has no
	 * sample row. A row is written before its first object and deleted
	 * before its objects, so such objects are always leftovers: an eviction
	 * or purge that failed after its row went, or an attachment that landed
	 * after one. Returns how many prefixes went.
	 */
	private async sweep(): Promise<number> {
		const app = this.ctx.id.name;
		if (app === undefined) return 0;
		const prefix = samplesPrefix(app);
		let removed = 0;
		let cursor: string | undefined;
		do {
			const page = await this.env.BUCKET.list({ prefix, cursor });
			const objects = new Map<string, string[]>();
			for (const object of page.objects) {
				const reportId = object.key.slice(prefix.length).split("/")[0]!;
				objects.set(reportId, [...(objects.get(reportId) ?? []), object.key]);
			}
			if (objects.size > 0) {
				const rows = await this.db
					.selectFrom("crash_samples")
					.select("report_id")
					.where("report_id", "in", [...objects.keys()])
					.execute();
				for (const row of rows) objects.delete(row.report_id);
				if (objects.size > 0) await this.env.BUCKET.delete([...objects.values()].flat());
				removed += objects.size;
			}
			cursor = page.truncated ? page.cursor : undefined;
		} while (cursor !== undefined);
		return removed;
	}

	/**
	 * Arms the alarm for the next time the purge has work: the first second
	 * after the earliest window to close on a release that still has
	 * reports, and a sweep every `SWEEP_INTERVAL` while the app holds
	 * samples. Clears it when neither applies, so an idle app never wakes.
	 */
	private async schedule(now: number): Promise<void> {
		const expiry = await this.db
			.selectFrom("releases")
			.select((eb) => eb.fn.min("supported_until").as("at"))
			.where((eb) =>
				eb.exists(
					eb.selectFrom("reports")
						.select("report_id")
						.whereRef("reports.version", "=", "releases.version")
						.whereRef("reports.channel", "=", "releases.channel")
				)
			)
			.executeTakeFirstOrThrow();
		const sample = await this.db.selectFrom("crash_samples").select("id").limit(1).executeTakeFirst();
		const due = [
			...(expiry.at === null ? [] : [Math.max(Number(expiry.at) + 1, now)]),
			...(sample === undefined ? [] : [now + SWEEP_INTERVAL]),
		];
		if (due.length === 0) await this.ctx.storage.deleteAlarm();
		else await this.ctx.storage.setAlarm(Math.min(...due) * 1000);
	}

	/** Runs the purge. A failure is retried by the platform; the sweep interval is armed first in case it is not. */
	async alarm(): Promise<void> {
		const now = Math.floor(Date.now() / 1000);
		await this.ctx.storage.setAlarm((now + SWEEP_INTERVAL) * 1000);
		await this.purge(now);
		await this.schedule(now);
	}

	/** Every group's stored row, oldest first. */
	async listStoredGroups(): Promise<StoredGroup[]> {
		return this.db
			.selectFrom("crash_groups")
			.select(["id", "fingerprint", "fault", "frames", "message"])
			.orderBy("id")
			.execute();
	}

	/**
	 * Applies new frames and hashes to groups, after a rule change or a
	 * table that names frames a group stored unnamed. A group whose new
	 * hash is still its own is updated in place and keeps its id. One
	 * whose hash now belongs to another group is merged with it: the
	 * older id survives, so links to it hold, with the earliest first
	 * seen, the latest last seen, the counts of both summed where they
	 * share a key, and the reports and samples of both. Atomic as
	 * `registerRelease`.
	 */
	async remap(changes: GroupChange[]): Promise<{ updated: number; merged: number }> {
		let updated = 0;
		let merged = 0;
		for (const change of changes) {
			const other = await this.db
				.selectFrom("crash_groups")
				.select("id")
				.where("fingerprint", "=", change.fingerprint)
				.where("id", "!=", change.id)
				.executeTakeFirst();
			if (other) {
				const survivor = Math.min(other.id, change.id);
				await this.merge(Math.max(other.id, change.id), survivor);
				merged += 1;
				if (survivor !== change.id) continue;
			}
			await this.db
				.updateTable("crash_groups")
				.set({ frames: change.frames, fingerprint: change.fingerprint })
				.where("id", "=", change.id)
				.execute();
			updated += 1;
		}
		return { updated, merged };
	}

	/** Moves everything of group `gone` into group `into` and deletes it. */
	private async merge(gone: number, into: number): Promise<void> {
		const counts = await this.db.selectFrom("crash_counts").selectAll().where("group_id", "=", gone).execute();
		for (const row of counts) {
			await this.db
				.insertInto("crash_counts")
				.values({ ...row, group_id: into })
				.onConflict((oc) =>
					oc.columns(["group_id", "version", "channel", "trust", "day"]).doUpdateSet({ count: sql`count + excluded.count` })
				)
				.execute();
		}
		await this.db.deleteFrom("crash_counts").where("group_id", "=", gone).execute();
		await this.db.updateTable("reports").set({ group_id: into }).where("group_id", "=", gone).execute();
		await this.db.updateTable("crash_samples").set({ group_id: into }).where("group_id", "=", gone).execute();
		const span = await this.db
			.selectFrom("crash_groups")
			.select((eb) => [eb.fn.min("first_seen").as("first"), eb.fn.max("last_seen").as("last")])
			.where("id", "in", [gone, into])
			.executeTakeFirstOrThrow();
		await this.db.deleteFrom("crash_groups").where("id", "=", gone).execute();
		await this.db
			.updateTable("crash_groups")
			.set({ first_seen: Number(span.first), last_seen: Number(span.last) })
			.where("id", "=", into)
			.execute();
	}

	/**
	 * Every group, most urgent first, then most recently seen. Urgency is
	 * taken over the reports of the `URGENCY_WINDOW` before `now`, so a
	 * group nothing has hit lately sinks whatever its total.
	 */
	async listGroups(now: number): Promise<GroupSummary[]> {
		const rows = await this.db
			.selectFrom("crash_groups")
			.leftJoin("crash_counts", "crash_counts.group_id", "crash_groups.id")
			.select([
				"crash_groups.id", "crash_groups.fault", "crash_groups.frames", "crash_groups.message",
				"crash_groups.first_seen", "crash_groups.last_seen",
				(eb) => eb.fn.coalesce(eb.fn.sum<number>("crash_counts.count"), sql<number>`0`).as("count"),
			])
			.groupBy("crash_groups.id")
			.execute();
		const recent = await this.db
			.selectFrom("reports")
			.select([
				"group_id",
				(eb) => eb.fn.countAll<number>().as("reports"),
				(eb) => eb.fn.count<number>("user_key").distinct().as("users"),
			])
			.where("received_at", ">=", now - URGENCY_WINDOW)
			.groupBy("group_id")
			.execute();
		const byGroup = new Map(recent.map((r) => [r.group_id, r]));
		return rows
			.map((r) => {
				const w = byGroup.get(r.id);
				const recent_count = Number(w?.reports ?? 0);
				const recent_users = Number(w?.users ?? 0);
				const urgency = Math.round(Math.sqrt(recent_count * recent_users));
				return { ...r, count: Number(r.count), recent_count, recent_users, urgency };
			})
			.sort((a, b) => b.urgency - a.urgency || b.last_seen - a.last_seen || b.count - a.count);
	}

	/** Names of the migrations this shard has applied, in order. */
	async appliedMigrations(): Promise<string[]> {
		return this.ctx.storage.sql
			.exec("SELECT name FROM shard_migrations ORDER BY name")
			.toArray()
			.map((r) => r.name as string);
	}
}
