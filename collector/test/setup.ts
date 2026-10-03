import { applyD1Migrations, env } from "cloudflare:test";

// Brings the test database to the schema a deployment would have.
await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);
