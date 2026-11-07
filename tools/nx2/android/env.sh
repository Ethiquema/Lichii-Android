# Shared settings for the phone test scripts. Source it: . tools/nx2/android/env.sh
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
NDK=${NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}
PKG=dev.eden.nx2.relWithDebInfo
FILES=/sdcard/Android/data/$PKG/files
GAME=${GAME:-/sdcard/Switch2/Game/458752}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../../.." && pwd)
OUT=${OUT:-$REPO/nx2-runs}
mkdir -p "$OUT"
unstripped_lib() { ls "$REPO"/src/android/app/build/intermediates/cxx/RelWithDebInfo/*/obj/arm64-v8a/libyuzu-android.so | head -1; }
# Screenshots only while Eden is in front, so other apps (calls, messages) are never captured.
eden_in_front() { $ADB shell dumpsys window | grep -E "mCurrentFocus|mFocusedApp" | grep -q "$PKG"; }
shot() { eden_in_front && $ADB exec-out screencap ${DISPLAY_ID:+-d $DISPLAY_ID} -p > "$1"; }
