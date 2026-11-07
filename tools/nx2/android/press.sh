#!/bin/bash
# Taps an on-screen overlay button on the outer display (game in landscape, 2420x1080), e.g. to
# accept the game's first-boot accessibility prompt without touching the phone.
# Usage: press.sh [A|B|X|Y|PLUS|MINUS|UP|DOWN|LEFT|RIGHT] (default A)
. "$(dirname "$0")/env.sh"
case "${1:-A}" in
    A) xy="1779 853" ;; B) xy="1670 972" ;; X) xy="1670 733" ;; Y) xy="1561 853" ;;
    PLUS) xy="1297 1026" ;; MINUS) xy="1122 1026" ;;
    UP) xy="683 760" ;; DOWN) xy="683 945" ;; LEFT) xy="590 853" ;; RIGHT) xy="776 853" ;;
    *) echo "unknown button $1"; exit 1 ;;
esac
$ADB shell input keyevent KEYCODE_WAKEUP
$ADB shell input -d 0 tap $xy
