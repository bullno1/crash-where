#!/bin/sh
# End to end: a crash in the `crashme` sample reaches the collector running
# under `wrangler dev`. The sample's own symbols are uploaded with `cwsym`
# first, so the frames the collector groups on are the sample's functions.
# Consent is stored as `always` before the first run, so the watcher
# uploads without a prompt. A null dereference and a `cw_abort` are each
# run once, and the app page must list both crashes by name.
#
# Usage: test/e2e/report.sh <crashme binary> <cwsym binary>
# Needs: node and the collector's dependencies installed (`npm ci` in
# collector/), curl, and the sample's debug file beside the binary.
set -eu

[ $# -eq 2 ] || { echo "usage: $0 <crashme binary> <cwsym binary>" >&2; exit 2; }
CRASHME=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CWSYM=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# The slug, version and channel the sample is built with.
APP=crashme
VERSION=0.0.1
CHANNEL=dev
WORK=$(mktemp -d)
REPORT_DIR="$WORK/report"

. "$ROOT/test/e2e/collector.sh"

cleanup() {
	status=$?
	collector_stop "$status"
	if [ "$status" -ne 0 ]; then
		for log in "$WORK"/crashme-*.log; do
			[ -f "$log" ] || continue
			echo "e2e: $(basename "$log") follows" >&2
			cat "$log" >&2
		done
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

# Runs the sample in mode $1 and waits for its watcher, which outlives it
# and shares its log, to say the report was uploaded.
crash() {
	log="$WORK/crashme-$1.log"
	# In a subshell, so the shell's own note about the signal lands in the log.
	( "$CRASHME" "$1" "$REPORT_DIR" "$ENDPOINT" ) > "$log" 2>&1 || true
	for _ in $(seq 1 60); do
		grep -q 'report .* uploaded' "$log" && break
		sleep 0.5
	done
	grep -q 'report .* uploaded' "$log" || fail "mode $1: the watcher did not report an upload"
	echo "e2e: mode $1: uploaded"
}

collector_start
TOKEN=$(create_app "$APP" "Crashme")
CWSYM_TOKEN=$TOKEN "$CWSYM" upload --endpoint "$ENDPOINT" --app "$APP" --version "$VERSION" --channel "$CHANNEL" "$CRASHME" 2>&1 \
	| sed 's/^/  /' || fail "symbol upload failed"
echo "e2e: symbols uploaded"

mkdir -p "$REPORT_DIR"
printf 'always\n' > "$REPORT_DIR/consent"
crash null
crash assert

left=$(find "$REPORT_DIR/pending" -name '*.json' 2>/dev/null | wc -l)
[ "$left" -eq 0 ] || fail "$left report(s) left in pending/"
# Wrangler colors its request log; the escape sequences come off first.
accepted=$(sed "s/$(printf '\033')\[[0-9;]*m//g" "$WORK/wrangler.log" | grep -c "POST /v1/$APP/report 201" || true)
[ "$accepted" -eq 2 ] || fail "the collector accepted $accepted report(s), expected 2"

page=$(dash "$ENDPOINT/dashboard/apps/$APP")

# Fails with the page's crash rows when none matches $1; $2 says what was expected.
expect_crash() {
	printf '%s' "$page" | grep -q -- "$1" && return
	printf '%s' "$page" | grep '<td>' | sed 's/^/  /' >&2
	fail "the app page does not list $2"
}
expect_crash "memory in crashme:crash_here, from crashme:level_two" "the null dereference by its frames"
expect_crash "ASSERT in main: mode != assert" "the abort by its message"
echo "e2e: app page lists both crashes"
echo "e2e: passed"
