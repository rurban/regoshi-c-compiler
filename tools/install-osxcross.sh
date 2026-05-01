#!/bin/sh
# Install arm64-darwin cross-compilation support
# Uses the gotson/crossbuild:m1 container (no local osxcross build needed).
#
# Usage: ./tools/install-osxcross.sh
set -e

RUNTIME="${RUNTIME:-podman}"
IMAGE="docker.io/gotson/crossbuild:latest"

if ! command -v "$RUNTIME" >/dev/null 2>&1; then
    echo "ERROR: $RUNTIME not found. Install podman or set RUNTIME=docker." >&2
    exit 1
fi

echo "==> Pulling $IMAGE..."
$RUNTIME pull "$IMAGE"

echo ""
echo "==> Testing aarch64-apple-darwin cross-compilation..."
$RUNTIME run --rm "$IMAGE" \
    sh -c 'echo "int main(){return 0;}" | aarch64-apple-darwin20.4-clang -x c - -o /tmp/a.out && file /tmp/a.out && rm /tmp/a.out'
echo ""
echo "==> Done. Use ./darwin-cross.sh to cross-compile with rcc."
echo ""
echo "  ./darwin-cross.sh -o test test.c"
echo ""
echo "Or use the container directly:"
echo "  $RUNTIME run --rm -v \$PWD:/work $IMAGE sh -c 'aarch64-apple-darwin20.4-clang -o /work/a.out /work/src.c'"
