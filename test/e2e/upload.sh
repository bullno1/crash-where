#!/bin/sh
# End to end: `cwsym upload` against the collector running under
# `wrangler dev`, with its state in a temporary directory. The dashboard
# runs in password mode; the app and its upload token are created through
# the dashboard as a person would, then the tool uploads the golden table,
# uploads it again, uploads it under another version, and tries a bad
# token. The app page is checked last.
#
# Usage: test/e2e/upload.sh <cwsym binary>
# Needs: node and the collector's dependencies installed (`npm ci` in
# collector/), curl.
set -eu

[ $# -eq 1 ] || { echo "usage: $0 <cwsym binary>" >&2; exit 2; }
CWSYM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FIXTURE="$ROOT/test/fixtures/synthetic.cwsym"
BUILD_ID=0102030405060708090a0b0c0d0e0f1011121314
APP=e2e-game
PASSWORD=e2e-correct-horse-battery
WORK=$(mktemp -d)
WRANGLER=

export WRANGLER_SEND_METRICS=false

cleanup() {
	status=$?
	if [ -n "$WRANGLER" ]; then
		kill "$WRANGLER" 2>/dev/null || true
		wait "$WRANGLER" 2>/dev/null || true
	fi
	if [ "$status" -ne 0 ]; then
		echo "e2e: failed; wrangler log follows" >&2
		cat "$WORK/wrangler.log" >&2 || true
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
	echo "e2e: $*" >&2
	exit 1
}

# Runs `cwsym upload` of the fixture as $1 with token $2; prints the tool's
# output and returns its status.
upload() {
	CWSYM_TOKEN=$2 "$CWSYM" upload --endpoint "$ENDPOINT" --app "$APP" --version "$1" --channel stable "$FIXTURE" 2>&1
}

# Checks that `upload $1 $2` exits with $3 and prints a line matching $4.
expect_upload() {
	if out=$(upload "$1" "$2"); then status=0; else status=$?; fi
	printf '%s\n' "$out" | sed 's/^/  /'
	[ "$status" -eq "$3" ] || fail "upload of $1: exit $status, expected $3"
	printf '%s\n' "$out" | grep -q -- "$4" || fail "upload of $1: no line matching '$4'"
	echo "e2e: upload of $1: ok"
}

# A dashboard request as a logged-in browser: password login and this
# origin, which the CSRF check wants on a form post.
dash() {
	curl -sS -u "alice:$PASSWORD" -H "Origin: $ENDPOINT" "$@"
}

cd "$ROOT/collector"
PORT=$(node -e 'const s = require("net").createServer(); s.listen(0, "127.0.0.1", () => { process.stdout.write(s.address().port + "\n"); s.close(); });')
ENDPOINT="http://127.0.0.1:$PORT"

node_modules/.bin/wrangler d1 migrations apply DB --local --persist-to "$WORK/state" > "$WORK/migrate.log" 2>&1 \
	|| { cat "$WORK/migrate.log" >&2; fail "migrations failed"; }
# The empty Access values override a developer's .dev.vars, which would
# otherwise put the dashboard behind Access instead of the password.
node_modules/.bin/wrangler dev --ip 127.0.0.1 --port "$PORT" --persist-to "$WORK/state" \
	--var "ACCESS_TEAM_DOMAIN:" --var "ACCESS_AUD:" --var "DASHBOARD_PASSWORD:$PASSWORD" \
	--show-interactive-dev-session false > "$WORK/wrangler.log" 2>&1 &
WRANGLER=$!
for _ in $(seq 1 120); do
	code=$(curl -s -o /dev/null -w '%{http_code}' "$ENDPOINT/" || true)
	[ "$code" = "302" ] && break
	kill -0 "$WRANGLER" 2>/dev/null || fail "wrangler dev exited"
	sleep 0.5
done
[ "$code" = "302" ] || fail "the collector did not come up on $ENDPOINT"
echo "e2e: collector up on $ENDPOINT"

code=$(dash -o /dev/null -w '%{http_code}' --data-urlencode "name=$APP" --data-urlencode "display_name=E2E Game" "$ENDPOINT/dashboard/apps")
[ "$code" = "303" ] || fail "creating the app: HTTP $code"
dash -D "$WORK/token.headers" -o /dev/null --data-urlencode "label=e2e" "$ENDPOINT/dashboard/apps/$APP/tokens"
TOKEN=$(tr -d '\r' < "$WORK/token.headers" | sed -n 's/^[Ss]et-[Cc]ookie: cw_new_token=\([^;]*\);.*/\1/p')
[ -n "$TOKEN" ] || { cat "$WORK/token.headers" >&2; fail "no token cookie on the create reply"; }
echo "e2e: app and token created"

expect_upload 1.0.0 "$TOKEN" 0 "uploaded $BUILD_ID as 1.0.0 (stable): HTTP 201"
expect_upload 1.0.0 "$TOKEN" 0 "uploaded $BUILD_ID as 1.0.0 (stable): HTTP 200"
expect_upload 1.0.1 "$TOKEN" 1 "HTTP 409: Build $BUILD_ID is already registered under version 1.0.0"
expect_upload 1.0.0 "cwu_not-a-token" 1 "HTTP 401: The upload token is not valid"

page=$(dash "$ENDPOINT/dashboard/apps/$APP")
printf '%s' "$page" | grep -q "<code>1.0.0</code>" || fail "the app page does not list version 1.0.0"
printf '%s' "$page" | grep -q "<code>$BUILD_ID</code>" || fail "the app page does not list the build"
printf '%s' "$page" | grep -q "<code>1.0.1</code>" && fail "the app page lists the refused version"
printf '%s' "$page" | grep -q "<td>e2e</td>" || fail "the app page does not list the token"
echo "e2e: app page lists the release"
echo "e2e: passed"
