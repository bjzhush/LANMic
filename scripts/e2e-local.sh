#!/bin/bash
# End-to-end test on this Mac without coreaudiod or sudo:
#   netmic-sender --tone 440 (real sender, UDP loopback)
#   → NetMic.driver loaded in-process by netmic_plugin_smoke (copy of the build
#     with NetMicConfig SenderAddress=127.0.0.1)
#   → IO cycles paced like the HAL; output must be a continuous 440 Hz sine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/driver/build"
SENDER="$ROOT/sender/.build/release/netmic-sender"
PORT=47899
WORK="$BUILD/e2e"

[[ -x "$SENDER" ]] || (cd "$ROOT/sender" && swift build -c release --product netmic-sender)
[[ -x "$BUILD/netmic_plugin_smoke" ]] || { echo "build the driver first (scripts/build-all.sh)" >&2; exit 1; }

rm -rf "$WORK" && mkdir -p "$WORK"
cp -R "$BUILD/NetMic.driver" "$WORK/"
PLIST="$WORK/NetMic.driver/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :NetMicConfig:SenderAddress 127.0.0.1" -c "Set :NetMicConfig:Port $PORT" "$PLIST"

"$SENDER" --tone 440 --port "$PORT" --stop-grace 1 -v >"$WORK/sender.log" 2>&1 &
SENDER_PID=$!
trap 'kill $SENDER_PID 2>/dev/null || true' EXIT
sleep 0.5

"$BUILD/netmic_plugin_smoke" "$WORK/NetMic.driver" --expect-tone
sleep 1.5  # stop-grace

fail=0
grep -q "subscriber 127.0.0.1:.* added" "$WORK/sender.log" || { echo "sender never saw a SUBSCRIBE" >&2; fail=1; }
grep -q "subscriber 127.0.0.1:.* unsubscribed" "$WORK/sender.log" || { echo "sender never saw an UNSUBSCRIBE" >&2; fail=1; }
grep -q "tone: stopped" "$WORK/sender.log" || { echo "sender did not stop after the driver stopped IO" >&2; fail=1; }
if [[ $fail != 0 ]]; then
    echo "--- sender log ---" >&2
    cat "$WORK/sender.log" >&2
    exit 1
fi
echo "e2e OK (sender subscribed, streamed, unsubscribed and stopped)"
