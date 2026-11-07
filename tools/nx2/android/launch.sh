#!/bin/bash
# Restarts the app straight into the game (exefs_path intent extra) with the screen awake.
# Usage: launch.sh [KEY=VALUE ...]   extra args replace files/nx2_env.txt (pass "-" to keep it)
. "$(dirname "$0")/env.sh"
if [ $# -gt 0 ] && [ "$1" != "-" ]; then
    printf '%s\n' "$@" > "$OUT/nx2_env.txt"
    $ADB push "$OUT/nx2_env.txt" "$FILES/" >/dev/null
fi
$ADB shell cat "$FILES/nx2_env.txt" 2>/dev/null | sed 's/^/env: /'
# GPU driver: empty driver_path means the system (Qualcomm) driver, not Turnip.
$ADB shell "grep '^driver_path=' $FILES/config/config.ini" | sed 's|=.*/|=|; s/^/driver: /'
$ADB shell am force-stop --user 0 $PKG
$ADB shell input keyevent KEYCODE_WAKEUP
$ADB shell am start --user 0 -n $PKG/org.yuzu.yuzu_emu.activities.EmulationActivity --es exefs_path "$GAME" >/dev/null
sleep 3
echo "pid $($ADB shell pidof $PKG) started $(date +%T)"
