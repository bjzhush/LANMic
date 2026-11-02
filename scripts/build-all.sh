#!/bin/bash
# Release build of sender/probe (sender/.build/release/) and, once it exists,
# the driver (driver/build/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> swift build -c release (netmic-sender, netmic-probe)"
(cd "$ROOT/sender" && swift build -c release)
echo "    $ROOT/sender/.build/release/netmic-sender"
echo "    $ROOT/sender/.build/release/netmic-probe"

echo "==> driver (cmake)"
if ! command -v cmake >/dev/null; then
    echo "cmake not found: brew install cmake" >&2
    exit 1
fi
cmake -S "$ROOT/driver" -B "$ROOT/driver/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/driver/build" -j
