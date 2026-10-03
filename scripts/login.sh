#!/bin/sh
# Logs a browser in to the dashboard served by a local `wrangler dev`.
# Fetches an Access token for the deployed dashboard with cloudflared,
# logging in first when there is none, and opens the dev server's
# /dev/login route, which stores the token as the Access cookie.
#
# usage: scripts/login.sh <dashboard-url> [port]
#        CW_DASHBOARD_URL=https://... scripts/login.sh
set -eu

app=${1:-${CW_DASHBOARD_URL:-}}
port=${2:-8787}
if [ -z "$app" ]; then
	echo "usage: $0 <dashboard-url> [port]" >&2
	exit 2
fi

token=$(cloudflared access token --app="$app" 2>/dev/null || true)
case $token in
	ey*.*.*) ;;
	*)
		cloudflared access login "$app"
		token=$(cloudflared access token --app="$app")
		;;
esac

url="http://localhost:$port/dev/login?token=$token"
if command -v xdg-open >/dev/null 2>&1; then
	xdg-open "$url"
elif command -v open >/dev/null 2>&1; then
	open "$url"
else
	echo "$url"
fi
