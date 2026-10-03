import { Kysely } from "kysely";
import { D1Dialect } from "kysely-d1";
import type { DB } from "./db.generated";

export type Db = Kysely<DB>;

/** A query builder over the D1 binding. Cheap; one per request is fine. */
export function createDb(d1: D1Database): Db {
	return new Kysely<DB>({ dialect: new D1Dialect({ database: d1 }) });
}
