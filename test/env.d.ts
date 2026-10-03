import type { D1Migration } from "@cloudflare/vitest-pool-workers";
import type { Env as WorkerEnv } from "../src/env";

declare global {
	namespace Cloudflare {
		/** What `env` from `cloudflare:test` carries: the Worker's bindings plus the test ones. */
		interface Env extends WorkerEnv {
			/** The SQL files under migrations/, read by the Vitest config. */
			TEST_MIGRATIONS: D1Migration[];
		}
	}
}
