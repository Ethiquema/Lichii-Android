#!/bin/bash
# Collects a run: emulator log, newest native crash (symbolized), exit reason, screenshots.
# Usage: collect.sh TAG [PID]
. "$(dirname "$0")/env.sh"
tag=$1; pid=$2; dir="$OUT/$tag"; mkdir -p "$dir"
alive=$($ADB shell pidof $PKG | tr -d '\r')
# The app rotates its log when it starts again, so a crashed run may be in the .old file.
if [ -n "$pid" ] && [ "$alive" != "$pid" ] && [ -n "$alive" ]; then
    $ADB pull "$FILES/log/eden_log.txt.old.txt" "$dir/eden_log.txt" >/dev/null
else
    $ADB pull "$FILES/log/eden_log.txt" "$dir/eden_log.txt" >/dev/null
fi
$ADB shell dumpsys activity exit-info $PKG | sed -n 4,9p > "$dir/exit.txt"
$ADB shell dumpsys dropbox --print data_app_native_crash > "$dir/dropbox_all.txt"
# Newest crash entry only.
awk '/data_app_native_crash \(text/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$dir/dropbox_all.txt" > "$dir/crash.txt"
lib=$(unstripped_lib)
want=$(file "$lib" | sed -n 's/.*BuildID\[sha1\]=\([0-9a-f]*\).*/\1/p')
grep -E "libyuzu-android.so" "$dir/crash.txt" | grep -q "$want" || echo "warning: crash Build ID differs from $lib" >> "$dir/crash_sym.txt"
grep -E "^ +#[0-9]+ pc" "$dir/crash.txt" | while read -r n _ pc where rest; do
    if [[ $where == *libyuzu-android.so ]]; then
        echo "$n $($NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-addr2line -f -C -e "$lib" 0x$pc | head -1 | cut -c1-140)"
    else
        echo "$n ${where##*/} $(echo "$rest" | cut -c1-100)"
    fi
done >> "$dir/crash_sym.txt"
if eden_in_front; then
    for id in $($ADB shell dumpsys SurfaceFlinger --display-id | sed -n 's/^Display \([0-9]*\).*/\1/p'); do
        $ADB exec-out screencap -d "$id" -p > "$dir/screen_$id.png"
    done
fi
echo "== $dir"; cat "$dir/exit.txt" | grep -E "timestamp|reason"; grep -E "Abort message|signal" "$dir/crash.txt" | head -3
head -25 "$dir/crash_sym.txt"
grep -nE "Device loss|unbacked sparse|instruction limit|Unhandled|Backtrace|Critical" "$dir/eden_log.txt" | grep -v aes_util | tail -8 | cut -c1-200
