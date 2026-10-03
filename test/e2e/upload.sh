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
WORK=$(mktemp -d)

. "$ROOT/test/e2e/collector.sh"

cleanup() {
	status=$?
	collector_stop "$status"
	rm -rf "$WORK"
}
trap cleanup EXIT

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

collector_start
TOKEN=$(create_app "$APP" "E2E Game")
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
