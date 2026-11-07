#!/bin/bash
# Samples the app's and the phone's memory every 2 s until the process exits or TIMEOUT
# seconds pass, keeps the screen awake, then collects the run (collect.sh).
# Usage: watch.sh [TIMEOUT=300] [TAG]
. "$(dirname "$0")/env.sh"
timeout=${1:-300}; tag=${2:-run-$(date +%H%M%S)}; dir="$OUT/$tag"; mkdir -p "$dir"
pid=$($ADB shell pidof $PKG | tr -d '\r')
[ -z "$pid" ] && { echo "app not running"; exit 1; }
end=$(( $(date +%s) + timeout ))
while [ "$(date +%s)" -lt "$end" ]; do
    st=$($ADB shell "run-as $PKG grep -E 'VmRSS|RssAnon|RssShmem|VmSwap' /proc/$pid/status" 2>/dev/null | awk '{printf "%s%d ", $1, $2/1024}')
    [ -z "$st" ] && break
    m=$($ADB shell "grep -E 'MemAvailable|SwapFree' /proc/meminfo" | awk '{printf "%s%d ", $1, $2/1024}')
    echo "$(date +%T) $st| $m" | tee -a "$dir/mem.txt"
    $ADB shell input keyevent KEYCODE_WAKEUP
    sleep 2
done
"$(dirname "$0")/collect.sh" "$tag" "$pid"
