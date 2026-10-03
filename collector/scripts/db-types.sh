#!/bin/sh
# Regenerates src/db.generated.ts, the Kysely types, from the schema that
# the files under migrations/ produce. The migrations are applied to the
# local D1 database first, so the types always describe the whole set.
# Extra arguments go to kysely-codegen: `--verify` only checks the file.
set -eu

cd "$(dirname "$0")/.."

npx wrangler d1 migrations apply DB --local >/dev/null

db=$(find .wrangler/state/v3/d1/miniflare-D1DatabaseObject -name '*.sqlite' ! -name metadata.sqlite)
if [ "$(printf '%s\n' "$db" | wc -l)" -ne 1 ]; then
	echo "db-types: expected one local database under .wrangler/state/v3/d1, found:" >&2
	printf '%s\n' "$db" >&2
	exit 1
fi

exec npx kysely-codegen --dialect sqlite --url "$db" \
	--exclude-pattern '{_cf_*,d1_migrations}' \
	--out-file src/db.generated.ts "$@"
