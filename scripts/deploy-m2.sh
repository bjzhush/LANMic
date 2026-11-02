#!/bin/bash
# Copies the build to M2 over ssh (M2 needs System Settings → General → Sharing → Remote Login).
#
#   scripts/deploy-m2.sh user@m2-host     (or set M2_HOST)
#
# Always copies netmic-probe to ~/netmic/ on M2. If driver/build/NetMic.driver
# exists (stage 2+), also copies it and runs install-driver.sh there, which
# asks for M2's sudo password.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${1:-${M2_HOST:-}}"
if [[ -z "$HOST" ]]; then
    echo "usage: $0 user@m2-host   (or set M2_HOST)" >&2
    exit 2
fi

PROBE="$ROOT/sender/.build/release/netmic-probe"
DRIVER="$ROOT/driver/build/NetMic.driver"
if [[ ! -x "$PROBE" ]]; then
    echo "==> building netmic-probe (release)"
    (cd "$ROOT/sender" && swift build -c release --product netmic-probe)
fi

echo "==> M2 system version"
ssh "$HOST" 'sw_vers; uname -m'

echo "==> copying to $HOST:~/netmic/"
ssh "$HOST" 'mkdir -p ~/netmic'
FILES=("$PROBE")
if [[ -d "$DRIVER" ]]; then
    FILES+=("$DRIVER" "$ROOT/scripts/install-driver.sh" "$ROOT/scripts/uninstall-driver.sh")
fi
scp -rq "${FILES[@]}" "$HOST:netmic/"
echo "    copied: ${FILES[*]##*/}"

if [[ -d "$DRIVER" ]]; then
    echo "==> installing driver on M2 (sudo password for M2 will be asked)"
    ssh -t "$HOST" 'cd ~/netmic && bash install-driver.sh NetMic.driver'
fi
echo "done. On M2: ~/netmic/netmic-probe --discover"
