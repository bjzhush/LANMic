#!/bin/bash
# Streams the driver's log. -v also shows libASPL's per-operation trace (very verbose).
#   scripts/logs.sh [-v]
if [[ "${1:-}" == "-v" ]]; then
    exec /usr/bin/log stream --level debug --predicate 'subsystem == "com.netmic.driver"'
fi
exec /usr/bin/log stream --level info --predicate 'subsystem == "com.netmic.driver" AND category == "driver"'
