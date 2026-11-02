#!/bin/bash
# Installs NetMic.driver into /Library/Audio/Plug-Ins/HAL and restarts coreaudiod.
# Run as your normal user on M2; sudo asks for the password. All sound on this
# Mac stops for a few seconds while coreaudiod restarts.
#
#   scripts/install-driver.sh [--set Key=Value ...] [path/to/NetMic.driver]
#
# Default bundle: driver/build/NetMic.driver. --set overrides a NetMicConfig
# entry for this install only (the bundle is copied, edited and re-signed), e.g.
#   scripts/install-driver.sh --set SenderAddress=192.168.1.4 --set TargetLatencyMs=40
# Keys: DeviceName SenderAddress Token (strings), Port TargetLatencyMs Channels (integers).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST=/Library/Audio/Plug-Ins/HAL/NetMic.driver

SETS=()
SRC=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --set) [[ $# -ge 2 && "$2" == *=* ]] || { echo "--set needs Key=Value" >&2; exit 2; }
               SETS+=("$2"); shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) SRC="$1"; shift ;;
    esac
done
SRC="${SRC:-$ROOT/driver/build/NetMic.driver}"

if [[ ! -f "$SRC/Contents/MacOS/NetMic" ]]; then
    echo "not a built driver: $SRC (run scripts/build-all.sh first)" >&2
    exit 1
fi
codesign --verify --strict "$SRC" || { echo "signature check failed: $SRC" >&2; exit 1; }

if [[ ${#SETS[@]} -gt 0 ]]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    cp -R "$SRC" "$TMP/"
    SRC="$TMP/NetMic.driver"
    for kv in "${SETS[@]}"; do
        key="${kv%%=*}"; value="${kv#*=}"
        case "$key" in
            DeviceName|SenderAddress|Token) type=string ;;
            Port|TargetLatencyMs|Channels) type=integer ;;
            *) echo "unknown NetMicConfig key: $key" >&2; exit 2 ;;
        esac
        /usr/libexec/PlistBuddy -c "Delete :NetMicConfig:$key" "$SRC/Contents/Info.plist" 2>/dev/null || true
        /usr/libexec/PlistBuddy -c "Add :NetMicConfig:$key $type $value" "$SRC/Contents/Info.plist"
        echo "    NetMicConfig $key = $value"
    done
    codesign --force --sign "${CODESIGN_IDENTITY:--}" --timestamp=none "$SRC" 2>/dev/null
fi
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
