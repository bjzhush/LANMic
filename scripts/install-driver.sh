#!/bin/bash
# Installs NetMic.driver into /Library/Audio/Plug-Ins/HAL and restarts coreaudiod.
# Run as your normal user on M2; sudo asks for the password. All sound on this
# Mac stops for a few seconds while coreaudiod restarts.
#
#   scripts/install-driver.sh [path/to/NetMic.driver]   (default: driver/build/NetMic.driver)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/driver/build/NetMic.driver}"
DEST=/Library/Audio/Plug-Ins/HAL/NetMic.driver

if [[ ! -f "$SRC/Contents/MacOS/NetMic" ]]; then
    echo "not a built driver: $SRC (run scripts/build-all.sh first)" >&2
    exit 1
fi
codesign --verify --strict "$SRC" || { echo "signature check failed: $SRC" >&2; exit 1; }
NAME=$(/usr/libexec/PlistBuddy -c 'Print :NetMicConfig:DeviceName' "$SRC/Contents/Info.plist" 2>/dev/null || echo "M4 Mic")

echo "==> installing $SRC → $DEST"
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"

echo "==> restarting coreaudiod"
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod

echo "==> waiting for \"$NAME\""
for _ in $(seq 1 15); do
    sleep 1
    if system_profiler SPAudioDataType 2>/dev/null | grep -q "^ *$NAME:"; then
        echo "OK: \"$NAME\" is present."
        exit 0
    fi
done
echo "\"$NAME\" did not appear within 15 s. Check: scripts/logs.sh, and Console.app crash reports" >&2
exit 1
