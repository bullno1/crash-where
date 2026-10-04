import { env, runInDurableObject } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import { shardMigrations } from "../migrations/shard";
import type { AppShard } from "../src/shard";

function shard(name: string) {
	return env.SHARD.get(env.SHARD.idFromName(name));
}

/** Runs `fn` inside the named app's object, with the instance typed as what it is. */
function inShard<T>(name: string, fn: (obj: AppShard, state: DurableObjectState) => T | Promise<T>): Promise<T> {
	return runInDurableObject(shard(name), (obj, state) => fn(obj as AppShard, state));
}

describe("shard migrations", () => {
	it("lists every file under migrations/shard, in order", () => {
		const files = env.TEST_SHARD_MIGRATIONS.map((m) => m.name.replace(/\.sql$/, ""));
		expect(shardMigrations.map((m) => m.name)).toEqual(files);
	});
	it("are applied when the object first wakes", async () => {
		expect(await shard("forest-quest").appliedMigrations()).toEqual(shardMigrations.map((m) => m.name));
		const tables = await inShard("forest-quest", (_obj, state) =>
			state.storage.sql
				.exec("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE '\\_cf\\_%' ESCAPE '\\' ORDER BY name")
				.toArray()
				.map((r) => r.name)
		);
		expect(tables).toEqual(["builds", "crash_counts", "crash_groups", "crash_samples", "releases", "reports", "shard_migrations", "versions"]);
	});
	it("record each migration once", async () => {
		await shard("forest-quest").appliedMigrations();
		const count = await inShard("forest-quest", (_obj, state) =>
			state.storage.sql.exec("SELECT count(*) AS n FROM shard_migrations").one().n
		);
		expect(count).toBe(shardMigrations.length);
	});
	it("keep apps apart", async () => {
		await inShard("a", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
		});
		const inA = await inShard("a", (obj) => obj.db.selectFrom("versions").select("version").execute());
		const inB = await inShard("b", (obj) => obj.db.selectFrom("versions").select("version").execute());
		expect(inA).toEqual([{ version: "1.0.0" }]);
		expect(inB).toEqual([]);
	});
});

// Object storage is shared by the tests of this file, so each test uses its own shard.
describe("release schema", () => {
	it("holds one version on several channels and with several builds", async () => {
		const rows = await inShard("channels", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "1.4.2", created_at: 1 }).execute();
			await obj.db
				.insertInto("builds")
				.values([{ build_id: "w", version: "1.4.2", uploaded_at: 1 }, { build_id: "l", version: "1.4.2", uploaded_at: 1 }])
				.execute();
			await obj.db
				.insertInto("releases")
				.values([
					{ channel: "beta", version: "1.4.2", released_at: 1 },
					{ channel: "stable", version: "1.4.2", released_at: 2 },
				])
				.execute();
			return obj.db
				.selectFrom("releases")
				.innerJoin("builds", "builds.version", "releases.version")
				.select(["releases.channel", "builds.build_id"])
				.orderBy("releases.channel")
				.orderBy("builds.build_id")
				.execute();
		});
		expect(rows).toEqual([
			{ channel: "beta", build_id: "l" }, { channel: "beta", build_id: "w" },
			{ channel: "stable", build_id: "l" }, { channel: "stable", build_id: "w" },
		]);
	});
	it("refuses the same version twice on one channel", async () => {
		await expect(
			inShard("dup", async (obj) => {
				await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
				await obj.db.insertInto("releases").values({ channel: "stable", version: "1.0.0", released_at: 1 }).execute();
				await obj.db.insertInto("releases").values({ channel: "stable", version: "1.0.0", released_at: 2 }).execute();
			})
		).rejects.toThrow(/UNIQUE/);
	});
});

describe("shard dialect", () => {
	it("runs typed queries with parameters and returning", async () => {
		const row = await inShard("returning", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
			return obj.db
				.insertInto("releases")
				.values({ channel: "stable", version: "1.0.0", released_at: 10 })
				.returningAll()
				.executeTakeFirstOrThrow();
		});
		expect(row).toEqual({ channel: "stable", version: "1.0.0", released_at: 10, supported_until: null });
	});
	it("reports affected rows", async () => {
		const result = await inShard("affected", async (obj) => {
			await obj.db.insertInto("versions").values({ version: "1.0.0", created_at: 1 }).execute();
			await obj.db
				.insertInto("builds")
				.values([
					{ build_id: "win", version: "1.0.0", uploaded_at: 1 },
					{ build_id: "linux", version: "1.0.0", uploaded_at: 2 },
				])
				.execute();
			return obj.db.updateTable("builds").set({ uploaded_at: 3 }).where("version", "=", "1.0.0").executeTakeFirst();
		});
		expect(result.numUpdatedRows).toBe(2n);
	});
	it("refuses explicit transactions", async () => {
		await expect(
			inShard("transaction", (obj) => obj.db.transaction().execute(async () => {}))
		).rejects.toThrow(/transactionSync/);
	});
});

describe("sampling", () => {
	type Ingest = Parameters<AppShard["ingest"]>[0];
	/** A report of the one crash, in version 1.0.0 on stable, with the draw and cap given. */
	function report(n: number, draw: number, over: Partial<Ingest> = {}): Ingest {
		return {
			reportId: `r${n}`, version: "1.0.0", channel: "stable", trust: 0, userKey: `u${n}`, now: 100 + n,
			group: { fingerprint: "f", fault: "memory", frames: "[]", message: null },
			sampleCap: 2, sampleKey: `app/r${n}/`, draw,
			...over,
		};
	}
	async function released(name: string) {
		await inShard(name, (obj) => obj.registerRelease({ version: "1.0.0", channel: "stable", buildId: "b", now: 1 }));
		await inShard(name, (obj) => obj.registerRelease({ version: "2.0.0", channel: "stable", buildId: "c", now: 2 }));
	}
	const stored = (name: string) =>
		inShard(name, (obj) => obj.db.selectFrom("crash_samples").select(["report_id", "r2_key"]).orderBy("id").execute());

	it("keeps every report until the bucket is full, then one in k by the draw", async () => {
		await released("reservoir");
		const ingest = (n: number, draw: number) => inShard("reservoir", (obj) => obj.ingest(report(n, draw)));
		expect(await ingest(1, 0.99)).toMatchObject({ outcome: "counted", sampled: true, evicted: null });
		expect(await ingest(2, 0.99)).toMatchObject({ outcome: "counted", sampled: true, evicted: null });
		// The third report: slot 2 of 3 is past the cap, so it is not kept.
		expect(await ingest(3, 0.7)).toMatchObject({ outcome: "counted", sampled: false, evicted: null });
		// The fourth: slot 1 of 4 is the second stored sample, which it replaces.
		expect(await ingest(4, 0.3)).toMatchObject({ outcome: "counted", sampled: true, evicted: "app/r2/" });
		expect(await stored("reservoir")).toEqual([{ report_id: "r1", r2_key: "app/r1/" }, { report_id: "r4", r2_key: "app/r4/" }]);
	});
	it("tells a retry whether its first delivery was sampled", async () => {
		await released("retry");
		await inShard("retry", (obj) => obj.ingest(report(1, 0.5)));
		await inShard("retry", (obj) => obj.ingest(report(2, 0.5, { sampleCap: 0 })));
		expect(await inShard("retry", (obj) => obj.ingest(report(1, 0.5)))).toEqual({ outcome: "duplicate", sampled: true });
		expect(await inShard("retry", (obj) => obj.ingest(report(2, 0.5)))).toEqual({ outcome: "duplicate", sampled: false });
	});
	it("keeps nothing under a cap of zero and prefers the group's own cap", async () => {
		await released("caps");
		expect(await inShard("caps", (obj) => obj.ingest(report(1, 0.5, { sampleCap: 0 })))).toMatchObject({ sampled: false });
		expect(await stored("caps")).toEqual([]);
		await inShard("caps", (obj) => obj.db.updateTable("crash_groups").set({ sample_cap: 1 }).execute());
		expect(await inShard("caps", (obj) => obj.ingest(report(2, 0.5, { sampleCap: 0 })))).toMatchObject({ sampled: true });
		// Full at one: the third report's slot 2 of 3 misses, whatever the app allows.
		expect(await inShard("caps", (obj) => obj.ingest(report(3, 0.9, { sampleCap: 5 })))).toMatchObject({ sampled: false });
	});
	it("fills a bucket per version and trust level", async () => {
		await released("buckets");
		const keep = (n: number, over: Partial<Ingest>) =>
			inShard("buckets", (obj) => obj.ingest(report(n, 0.99, { sampleCap: 1, ...over })));
		expect(await keep(1, {})).toMatchObject({ sampled: true });
		expect(await keep(2, {})).toMatchObject({ sampled: false });
		expect(await keep(3, { version: "2.0.0" })).toMatchObject({ sampled: true });
		expect(await keep(4, { trust: 1 })).toMatchObject({ sampled: true });
		expect((await stored("buckets")).map((s) => s.report_id)).toEqual(["r1", "r3", "r4"]);
	});
});

describe("remap", () => {
	type Ingest = Parameters<AppShard["ingest"]>[0];
	function report(n: number, fp: string, over: Partial<Ingest> = {}): Ingest {
		return {
			reportId: `r${n}`, version: "1.0.0", channel: "stable", trust: 0, userKey: `u${n}`, now: 100 + n,
			group: { fingerprint: fp, fault: "memory", frames: `["${fp}"]`, message: null },
			sampleCap: 5, sampleKey: `app/r${n}/`, draw: 0.5,
			...over,
		};
	}
	async function groupsOf(name: string) {
		return inShard(name, async (obj) => ({
			groups: await obj.db.selectFrom("crash_groups").selectAll().orderBy("id").execute(),
			counts: await obj.db.selectFrom("crash_counts").selectAll().orderBy("group_id").orderBy("day").execute(),
			reports: await obj.db.selectFrom("reports").select(["report_id", "group_id"]).orderBy("report_id").execute(),
			samples: await obj.db.selectFrom("crash_samples").select(["report_id", "group_id"]).orderBy("id").execute(),
		}));
	}
	async function seed(name: string) {
		await inShard(name, (obj) => obj.registerRelease({ version: "1.0.0", channel: "stable", buildId: "b", now: 1 }));
		await inShard(name, (obj) => obj.ingest(report(1, "a")));
		await inShard(name, (obj) => obj.ingest(report(2, "b", { now: 86400 * 3 })));
		await inShard(name, (obj) => obj.ingest(report(3, "b", { now: 86400 * 3 + 1 })));
	}
	it("updates a group in place when its new hash is its own", async () => {
		await seed("remap-place");
		const [a] = (await groupsOf("remap-place")).groups;
		expect(await inShard("remap-place", (obj) => obj.remap([{ id: a!.id, frames: '["x"]', fingerprint: "x" }])))
			.toEqual({ updated: 1, merged: 0 });
		const { groups } = await groupsOf("remap-place");
		expect(groups).toHaveLength(2);
		expect(groups[0]).toMatchObject({ id: a!.id, frames: '["x"]', fingerprint: "x", first_seen: 101, last_seen: 101 });
	});
	it("merges a group into the older one that holds its new hash", async () => {
		await seed("remap-older");
		const [a, b] = (await groupsOf("remap-older")).groups;
		expect(await inShard("remap-older", (obj) => obj.remap([{ id: b!.id, frames: '["a2"]', fingerprint: "a" }])))
			.toEqual({ updated: 0, merged: 1 });
		const { groups, counts, reports, samples } = await groupsOf("remap-older");
		expect(groups).toHaveLength(1);
		// The older group keeps its id, frames and hash, and spans both.
		expect(groups[0]).toMatchObject({ id: a!.id, frames: '["a"]', fingerprint: "a", first_seen: 101, last_seen: 86400 * 3 + 1 });
		expect(counts).toEqual([
			{ group_id: a!.id, version: "1.0.0", channel: "stable", trust: 0, day: 0, count: 1 },
			{ group_id: a!.id, version: "1.0.0", channel: "stable", trust: 0, day: 3, count: 2 },
		]);
		expect(reports.map((r) => r.group_id)).toEqual([a!.id, a!.id, a!.id]);
		expect(samples.map((s) => s.group_id)).toEqual([a!.id, a!.id, a!.id]);
	});
	it("merges the newer group into an older one that takes its hash, summing a shared day", async () => {
		await seed("remap-newer");
		await inShard("remap-newer", (obj) => obj.ingest(report(4, "a", { now: 86400 * 3 + 2 })));
		const [a, b] = (await groupsOf("remap-newer")).groups;
		expect(await inShard("remap-newer", (obj) => obj.remap([{ id: a!.id, frames: '["b2"]', fingerprint: "b" }])))
			.toEqual({ updated: 1, merged: 1 });
		const { groups, counts } = await groupsOf("remap-newer");
		expect(groups).toHaveLength(1);
		expect(groups[0]).toMatchObject({ id: a!.id, frames: '["b2"]', fingerprint: "b", first_seen: 101, last_seen: 86400 * 3 + 2 });
		expect(b!.id).not.toBe(a!.id);
		expect(counts.map((c) => [c.day, c.count])).toEqual([[0, 1], [3, 3]]);
	});
});
