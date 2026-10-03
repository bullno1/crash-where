# Sourced after env.sh by the test scripts of the Linux toolchains.
#
# The test binary checks the core it writes with readers of its own. What
# a debugger makes of that core is the one thing it cannot check, so the
# script opens the core a crash test leaves behind with gdb and eu-stack,
# whichever of the two are installed, and looks for the frames, the
# libraries, and a local variable in their output.

# The core left by minidump/crash_writes_dump, if that test ran after
# `stamp` was touched.
fresh_core() {
	find "$BUILD_DIR/test/work/minidump_crash_writes_dump/report/pending" \
		-name '*.dmp' -newer "$1" 2>/dev/null | head -n 1
}

# $1: tool name, $2: its output, then the strings the output must hold.
# Prints the output in full when one is missing.
expect_in_output() {
	tool=$1
	out=$2
	shift 2
	ok=1
	for want in "$@"; do
		case "$out" in
		*"$want"*) ;;
		*)
			echo "core check: $tool output lacks '$want'"
			ok=0
			;;
		esac
	done
	if [ "$ok" = 1 ]; then
		return 0
	fi
	printf '%s\n' "$out"
	return 1
}

# gdb must list the shared libraries, unwind to the scenario, and print
# the local the scenario left on the stack. `-nx` keeps the user's init
# file out of it.
check_core_gdb() {
	out=$(gdb -batch -nx \
		-ex 'info sharedlibrary' -ex 'bt' -ex 'frame 1' -ex 'p/x marker' \
		"$2" "$1" 2>&1) || true
	expect_in_output gdb "$out" \
		'write_null ()' 'crash_with_marker ()' 'libc.so' '0xc5, 0x3a, 0x9e, 0x11' \
		&& echo "core check: gdb lists the libraries, unwinds, and shows the marker local"
}

# eu-stack must name the scenario's frames and reach libc, which it can
# only do through the link map the core carries. When the binary alone
# does not name them and a split debug file sits beside it, that file is
# handed over as the executable instead: elfutils pairs a binary with
# its debuglink file but resolves symbols only when the two share a
# section layout, which mold's `--separate-debug-file` output does not.
check_core_eu_stack() {
	candidates=$2
	for suffix in .debug .dbg; do
		if [ -f "$2$suffix" ]; then
			candidates="$candidates $2$suffix"
		fi
	done
	for exe in $candidates; do
		out=$(eu-stack --core="$1" -e "$exe" 2>&1) || true
		case "$out" in
		*write_null*crash_with_marker*__libc_start_main*)
			echo "core check: eu-stack unwinds through libc"
			return 0
			;;
		esac
	done
	expect_in_output eu-stack "$out" 'write_null' 'crash_with_marker' '__libc_start_main'
}

# Run cw_test with the given filters, then the debugger checks when a
# core came out of this run. A missing debugger is noted and skipped; a
# check that fails makes the script fail.
run_tests() {
	stamp="$BUILD_DIR/test/.started"
	: > "$stamp"
	status=0
	"$BIN_DIR/cw_test" "$@" || status=$?
	core=$(fresh_core "$stamp")
	if [ -z "$core" ]; then
		echo "core check: no core from minidump/crash_writes_dump in this run, skipped"
		return "$status"
	fi
	for tool in gdb eu-stack; do
		if ! command -v "$tool" >/dev/null 2>&1; then
			echo "core check: $tool is not installed, skipped"
			continue
		fi
		case "$tool" in
		gdb) check_core_gdb "$core" "$BIN_DIR/cw_test" || status=1 ;;
		eu-stack) check_core_eu_stack "$core" "$BIN_DIR/cw_test" || status=1 ;;
		esac
	done
	return "$status"
}
