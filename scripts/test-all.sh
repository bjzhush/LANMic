#!/bin/bash
# Runs the Swift (sender/) and C++ (driver/) unit tests.
#   TSAN=1 scripts/test-all.sh   also runs the C++ concurrency test under ThreadSanitizer
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> swift test (sender)"
# With only Command Line Tools, the default swiftbuild system doesn't pass the
# swift-testing macro plugin path ("plugin for module 'TestingMacros' not found").
SWIFT_TEST_FLAGS=()
TESTING_PLUGINS="$(xcode-select -p)/usr/lib/swift/host/plugins/testing"
if [[ -d "$TESTING_PLUGINS" ]]; then
    SWIFT_TEST_FLAGS=(-Xswiftc -plugin-path -Xswiftc "$TESTING_PLUGINS")
fi
(cd "$ROOT/sender" && swift test ${SWIFT_TEST_FLAGS[@]+"${SWIFT_TEST_FLAGS[@]}"})

echo "==> C++ tests (driver)"
if command -v cmake >/dev/null; then
    cmake -S "$ROOT/driver" -B "$ROOT/driver/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT/driver/build" --target netmic_tests -j
    "$ROOT/driver/build/netmic_tests"
else
    echo "(cmake not found — building tests directly with clang++; 'brew install cmake' is needed for the driver)"
    mkdir -p "$ROOT/driver/build"
    clang++ -std=c++17 -O2 -Wall -Wextra -I"$ROOT/shared" -I"$ROOT/driver/src" \
        -DNETMIC_TEST_VECTORS="\"$ROOT/shared/test_vectors.txt\"" \
        "$ROOT/driver/src/JitterBuffer.cpp" "$ROOT"/driver/tests/*.cpp -o "$ROOT/driver/build/netmic_tests"
    "$ROOT/driver/build/netmic_tests"
fi

if [[ "${TSAN:-0}" == 1 ]]; then
    echo "==> C++ concurrency test under ThreadSanitizer"
    clang++ -std=c++17 -O1 -g -fsanitize=thread -I"$ROOT/shared" -I"$ROOT/driver/src" \
        -DNETMIC_TEST_VECTORS="\"$ROOT/shared/test_vectors.txt\"" \
        "$ROOT/driver/src/JitterBuffer.cpp" "$ROOT"/driver/tests/*.cpp -o "$ROOT/driver/build/netmic_tests_tsan"
    "$ROOT/driver/build/netmic_tests_tsan" concurrent
fi
