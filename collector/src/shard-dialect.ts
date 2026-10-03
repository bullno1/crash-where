import {
	type CompiledQuery, type DatabaseConnection, type DatabaseIntrospector, type Dialect,
	type DialectAdapter, type Driver, type Kysely, type QueryCompiler, type QueryResult,
	SqliteAdapter, SqliteIntrospector, SqliteQueryCompiler,
} from "kysely";

/**
 * Kysely over a Durable Object's SQLite storage.
 * Object storage has no BEGIN or COMMIT: a synchronous run of a method is
 * already atomic and `ctx.storage.transactionSync` covers the rest, so
 * `db.transaction()` is refused.
 */
export class ShardDialect implements Dialect {
	constructor(private readonly sql: SqlStorage) {}

	createAdapter(): DialectAdapter {
		return new SqliteAdapter();
	}

	createDriver(): Driver {
		return new ShardDriver(this.sql);
	}

	createIntrospector(db: Kysely<unknown>): DatabaseIntrospector {
		return new SqliteIntrospector(db);
	}

	createQueryCompiler(): QueryCompiler {
		return new SqliteQueryCompiler();
	}
}

class ShardDriver implements Driver {
	private readonly connection: ShardConnection;

	constructor(sql: SqlStorage) {
		this.connection = new ShardConnection(sql);
	}

	async init(): Promise<void> {}

	async acquireConnection(): Promise<DatabaseConnection> {
		return this.connection;
	}

	async beginTransaction(): Promise<void> {
		throw new Error("Durable Object storage has no explicit transactions; use transactionSync");
	}

	async commitTransaction(): Promise<void> {
		throw new Error("Durable Object storage has no explicit transactions");
	}

	async rollbackTransaction(): Promise<void> {
		throw new Error("Durable Object storage has no explicit transactions");
	}

	async releaseConnection(): Promise<void> {}

	async destroy(): Promise<void> {}
}

class ShardConnection implements DatabaseConnection {
	constructor(private readonly sql: SqlStorage) {}

	async executeQuery<R>(query: CompiledQuery): Promise<QueryResult<R>> {
		const cursor = this.sql.exec(query.sql, ...(query.parameters as SqlStorageValue[]));
		const rows = cursor.toArray() as R[];
		return { rows, numAffectedRows: BigInt(cursor.rowsWritten) };
	}

	async *streamQuery<R>(query: CompiledQuery): AsyncIterableIterator<QueryResult<R>> {
		const cursor = this.sql.exec(query.sql, ...(query.parameters as SqlStorageValue[]));
		for (const row of cursor) yield { rows: [row as R] };
	}
}
