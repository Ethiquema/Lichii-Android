#!/bin/bash
# Builds the RelWithDebInfo APK and installs it for the main user only.
set -e
. "$(dirname "$0")/env.sh"
cd "$REPO"
export ANDROID_HOME=$HOME/Library/Android/sdk JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-17.jdk/Contents/Home
APK=artifacts/app-mainline-relWithDebInfo.apk
# build.sh is run through sh, which ignores its "-e" shebang flag, so a failed gradle build still
# ends in "Done!" with exit status 0. Check the log and the APK's age as well as the exit status.
STAMP=$(mktemp)
trap 'rm -f "$STAMP"' EXIT
fail() { grep -m5 -E "error:|BUILD FAILED" build-android.log || true; echo "build_install: $1, not installing" >&2; exit 1; }
sh -e .ci/android/build.sh -t standard -b RelWithDebInfo > build-android.log 2>&1 || fail "build exited non-zero"
grep -q "BUILD FAILED" build-android.log && fail "gradle reported BUILD FAILED"
[ "$APK" -nt "$STAMP" ] || fail "$APK was not rebuilt"
$ADB install --user 0 -r "$APK"
