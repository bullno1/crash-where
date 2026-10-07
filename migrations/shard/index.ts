import releases from "./0001_releases.sql";
import crashes from "./0002_crashes.sql";
import samples from "./0003_samples.sql";
import purge from "./0004_purge.sql";
import source from "./0005_source.sql";

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
	{ name: "0002_crashes", sql: crashes },
	{ name: "0003_samples", sql: samples },
	{ name: "0004_purge", sql: purge },
	{ name: "0005_source", sql: source },
];
