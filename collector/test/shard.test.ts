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
