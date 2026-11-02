#!/bin/bash
# Builds the menu-bar sender and wraps it into sender/build/NetMicSender.app (ad-hoc signed;
# shown as "NetMic Sender").
# Copy it to M4 (run on M4; only M4 → M2 ssh works):
#   scp -r zs@192.168.1.2:Dev/Claude/LANMic/sender/build/NetMicSender.app /Applications/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/sender/build/NetMicSender.app"

echo "==> swift build -c release (NetMicSender)"
(cd "$ROOT/sender" && swift build -c release --product NetMicSender)

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/sender/.build/release/NetMicSender" "$APP/Contents/MacOS/NetMicSender"
cp "$ROOT/sender/App/Info.plist" "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
# Ad-hoc signature: macOS ties the microphone permission to it, so each rebuild
# asks for permission again.
codesign --force --sign - --identifier com.netmic.sender "$APP"
codesign --verify --strict "$APP"
echo "    $APP"
