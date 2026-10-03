import { cloudflareTest, readD1Migrations } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

export default defineConfig(async () => {
	const migrations = await readD1Migrations("migrations/root");
	const shardMigrations = await readD1Migrations("migrations/shard");
	return {
		plugins: [
			cloudflareTest({
				wrangler: { configPath: "./wrangler.toml" },
				miniflare: { bindings: { TEST_MIGRATIONS: migrations, TEST_SHARD_MIGRATIONS: shardMigrations } },
			}),
		],
		test: { setupFiles: ["./test/setup.ts"] },
	};
});
