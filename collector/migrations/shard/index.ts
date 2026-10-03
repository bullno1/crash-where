import releases from "./0001_releases.sql";

/** A shard migration: the file's name without extension and its SQL. */
export interface ShardMigration {
	name: string;
	sql: string;
}

/**
 * Every file in this directory, in order. A new file must be added here;
 * the test suite checks the list against the directory.
 */
export const shardMigrations: ShardMigration[] = [
	{ name: "0001_releases", sql: releases },
];
