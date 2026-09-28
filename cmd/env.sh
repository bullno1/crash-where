# Sourced by every cmd/<toolchain>/* script; the caller sets HERE first.
ROOT=$(cd "$HERE/../.." && pwd)
TOOLCHAIN=$(basename "$HERE")
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
BUILD_DIR="$ROOT/.build/$TOOLCHAIN/$BUILD_TYPE"
BIN_DIR="$ROOT/bin/$TOOLCHAIN/$BUILD_TYPE"
