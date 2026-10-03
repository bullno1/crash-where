#!/bin/sh
# Regenerates the Kysely types from the schema that the migrations produce:
# src/db.generated.ts from migrations/root, applied to the local D1
# database, and src/shard.generated.ts from migrations/shard, applied to a
# scratch SQLite file. Extra arguments go to kysely-codegen: `--verify`
# only checks the files.
set -eu

cd "$(dirname "$0")/.."

npx wrangler d1 migrations apply DB --local >/dev/null

root=$(find .wrangler/state/v3/d1/miniflare-D1DatabaseObject -name '*.sqlite' ! -name metadata.sqlite)
if [ "$(printf '%s\n' "$root" | wc -l)" -ne 1 ]; then
	echo "db-types: expected one local database under .wrangler/state/v3/d1, found:" >&2
	printf '%s\n' "$root" >&2
	exit 1
fi

shard=$(mktemp)
trap 'rm -f "$shard"' EXIT
node -e '
	const Database = require("better-sqlite3");
	const fs = require("fs");
	const [dir, out] = process.argv.slice(1);
	const db = new Database(out);
	for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".sql")).sort()) {
		db.exec(fs.readFileSync(`${dir}/${f}`, "utf8"));
	}
' migrations/shard "$shard"

npx kysely-codegen --dialect sqlite --url "$root" \
	--exclude-pattern '{_cf_*,d1_migrations}' \
	--out-file src/db.generated.ts "$@"
npx kysely-codegen --dialect sqlite --url "$shard" \
	--out-file src/shard.generated.ts "$@"
