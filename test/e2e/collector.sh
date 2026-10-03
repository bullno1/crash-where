# Shared by the end-to-end scripts: the collector under `wrangler dev` with
# its state in $WORK, the dashboard in password mode, and the requests a
# person would make to it. The sourcing script sets ROOT and WORK, and its
# exit trap calls collector_stop with the exit status.
PASSWORD=e2e-correct-horse-battery
WRANGLER=

export WRANGLER_SEND_METRICS=false

fail() {
	echo "e2e: $*" >&2
	exit 1
}

# A dashboard request as a logged-in browser: password login and this
# origin, which the CSRF check wants on a form post.
dash() {
	curl -sS -u "alice:$PASSWORD" -H "Origin: $ENDPOINT" "$@"
}

# Starts the collector on a free port, waits for it, and sets ENDPOINT.
collector_start() {
	PORT=$(node -e 'const s = require("net").createServer(); s.listen(0, "127.0.0.1", () => { process.stdout.write(s.address().port + "\n"); s.close(); });')
	ENDPOINT="http://127.0.0.1:$PORT"
	wrangler="$ROOT/collector/node_modules/.bin/wrangler"

	(cd "$ROOT/collector" && "$wrangler" d1 migrations apply DB --local --persist-to "$WORK/state") > "$WORK/migrate.log" 2>&1 \
		|| { cat "$WORK/migrate.log" >&2; fail "migrations failed"; }
	# The empty Access values override a developer's .dev.vars, which would
	# otherwise put the dashboard behind Access instead of the password.
	(cd "$ROOT/collector" && exec "$wrangler" dev --ip 127.0.0.1 --port "$PORT" --persist-to "$WORK/state" \
		--var "ACCESS_TEAM_DOMAIN:" --var "ACCESS_AUD:" --var "DASHBOARD_PASSWORD:$PASSWORD" \
		--show-interactive-dev-session false) > "$WORK/wrangler.log" 2>&1 &
	WRANGLER=$!
	for _ in $(seq 1 120); do
		code=$(curl -s -o /dev/null -w '%{http_code}' "$ENDPOINT/" || true)
		[ "$code" = "302" ] && break
		kill -0 "$WRANGLER" 2>/dev/null || fail "wrangler dev exited"
		sleep 0.5
	done
	[ "$code" = "302" ] || fail "the collector did not come up on $ENDPOINT"
	echo "e2e: collector up on $ENDPOINT"
}

# Stops the collector; a nonzero $1 is a failed run, whose log is printed.
collector_stop() {
	if [ -n "$WRANGLER" ]; then
		kill "$WRANGLER" 2>/dev/null || true
		wait "$WRANGLER" 2>/dev/null || true
	fi
	if [ "$1" -ne 0 ]; then
		echo "e2e: failed; wrangler log follows" >&2
		cat "$WORK/wrangler.log" >&2 || true
	fi
}

# Creates the app $1, displayed as $2, through the dashboard, mints an
# upload token for it as a person would, and prints the token.
create_app() {
	code=$(dash -o /dev/null -w '%{http_code}' --data-urlencode "name=$1" --data-urlencode "display_name=$2" "$ENDPOINT/dashboard/apps")
	[ "$code" = "303" ] || fail "creating the app: HTTP $code"
	dash -D "$WORK/token.headers" -o /dev/null --data-urlencode "label=e2e" "$ENDPOINT/dashboard/apps/$1/tokens"
	token=$(tr -d '\r' < "$WORK/token.headers" | sed -n 's/^[Ss]et-[Cc]ookie: cw_new_token=\([^;]*\);.*/\1/p')
	[ -n "$token" ] || { cat "$WORK/token.headers" >&2; fail "no token cookie on the create reply"; }
	printf '%s\n' "$token"
}
