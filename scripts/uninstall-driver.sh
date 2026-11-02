#!/bin/bash
# Removes NetMic.driver and restarts coreaudiod. Safe to run at any time,
# including when the driver is not installed.
set -euo pipefail
DEST=/Library/Audio/Plug-Ins/HAL/NetMic.driver

if [[ -e "$DEST" ]]; then
    NAME=$(/usr/libexec/PlistBuddy -c 'Print :NetMicConfig:DeviceName' "$DEST/Contents/Info.plist" 2>/dev/null || echo "M4 Mic")
    echo "==> removing $DEST"
    sudo rm -rf "$DEST"
else
    NAME="M4 Mic"
    echo "$DEST is not installed; restarting coreaudiod anyway"
fi

echo "==> restarting coreaudiod"
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || sudo killall coreaudiod
sleep 3
if system_profiler SPAudioDataType 2>/dev/null | grep -q "^ *$NAME:"; then
    echo "\"$NAME\" is still listed" >&2
    exit 1
fi
echo "OK: \"$NAME\" is gone."
