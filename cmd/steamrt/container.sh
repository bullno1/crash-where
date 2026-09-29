# Sourced after env.sh: re-executes the calling script inside the Steam
# Runtime SDK container unless it is already running in one, with the
# repository mounted at the same path.
if ! grep -q '^ID=steamrt$' /etc/os-release 2>/dev/null; then
	tty=""
	[ -t 0 ] && tty="-t"
	exec "${STEAMRT_RUNTIME:-podman}" run --rm -i $tty \
		-v "$ROOT:$ROOT" -w "$PWD" -e BUILD_TYPE="$BUILD_TYPE" \
		-e CW_SPLIT_DEBUG="${CW_SPLIT_DEBUG:-ON}" \
		"${STEAMRT_IMAGE:-registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk:latest}" \
		"$0" "$@"
fi
