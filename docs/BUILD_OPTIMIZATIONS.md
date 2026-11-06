# Android build optimizations

This document describes the native build optimizations applied to the Lichii
Android APK (NDK clang, arm64-v8a, CMake driven by Gradle) and the per-device
CI/CD matrix.

**Principle: universal optimizations are enabled by default for everyone, and
the Android CI/CD pipeline builds one APK per device profile via a matrix
generated from [.ci/android/devices.yml](../.ci/android/devices.yml) — no flag
needed: the published builds are already optimized.**

## Tier 1 — universal optimizations, enabled by default

Applied automatically to **every** Android build (all flavors: mainline,
legacy, genshinSpoof; all build types: release, relWithDebInfo), for **all
arm64-v8a devices** — including old ARMv8.0 cores. Nothing to do: the CI
(`android.yml`, via `.ci/android/build.sh -t standard -b Release`) gets them
by default.

| Optimization | Compatibility guarantee |
|---|---|
| `-O3` on every non-Debug config | Zero risk: same instruction set, just more optimization. Brings the `relWithDebInfo` variant up from `-O2 -g` (and drops the `-g` blob from the `.so`). |
| `-march=armv8-a+crc+crypto` | CRC and AES/SHA (crypto) extensions are implemented by **every** 64-bit Android SoC (Cortex-A35/A53/A57 and later), so this stays universal; it speeds up the software AES-XTS/SHA paths (content decryption, mbedtls) among others. |
| `-mtune=cortex-a76` | **No instruction change**: only biases instruction scheduling toward the modern big cores emulators actually run on (A76/X2/X3). Harmless on A53/A57/A72. |
| ThinLTO (`-flto=thin`) | Does not affect the instruction set. The NDK only ships clang + lld, so ThinLTO is always available there. Cross-TU inlining in the shader recompiler, the GPU thread and dynarmic: typically 5–10%. |

Implementation: the "Android default optimizations" block in `CMakeLists.txt`
and the `ENABLE_LTO` default ON on Android in
`externals/cmake-modules/UseLTO.cmake`.

Escape hatches (occasional debug):
`-PYUZU_ANDROID_ARGS="-DENABLE_LTO=OFF"` (disables ThinLTO);
`-PYUZU_ANDROID_ARGS="-DYUZU_BUILD_PRESET=generic"` (removes march/mtune).

## Tier 2 — per-device CI/CD matrix: one APK per device class

The Android CI/CD follows a two-level `build_targets` / `devices` model:

1. The `matrix` job of `.github/workflows/android.yml` runs
   `.ci/android/gen_matrix.py`, which reads `.ci/android/devices.yml` and
   produces the GitHub Actions matrix (one entry per `build_target`, not per
   device — devices sharing a target share the same APK).
2. The `build` job runs as `strategy.matrix` and builds one APK per target,
   each with its own ccache cache (object files are not interchangeable
   between different `-march`).
3. The `release` job gathers every APK and publishes them in a single GitHub
   release (`Lichii-android-<target>.apk`), plus a device → APK table in the
   run summary.

### The matrix (current state of `devices.yml`)

| Published APK | CMake preset | Flags | Compatibility |
|---|---|---|---|
| `Lichii-android-universal-<commit>.apk` | `universal` | `-march=armv8-a+crc+crypto -mtune=cortex-a76` | ARMv8.0+ — **every** arm64 Android device |
| `Lichii-android-armv82-modern-<commit>.apk` | `modern` | `-march=armv8.2-a+crypto+fp16+rcpc+dotprod -mtune=cortex-a76` | ARMv8.2+ — Snapdragon 845→888, RK3588, Dimensity 8100/9000 |
| `Lichii-android-armv9-<commit>.apk` | `armv9` | `-march=armv9-a+crypto -mtune=cortex-x3` | ARMv9+ — Snapdragon 8 Gen 1/2/3, 8 Elite, Dimensity 9200+ |

`<commit>` is the `git describe --always --long` of the exact commit the APK
was built from — check it against the release page before installing, so you
never flash a stale (e.g. pre-fix) build by accident. The rolling test release
prunes its old assets on every build. `versionCode` is derived from the build
time (always increases), `versionName` from `git describe`.

### APK versioning and updates (sideload)

- `versionCode` = seconds/10 since 2016-01-01 (`build.gradle.kts`): it
  strictly increases on every build, so two APKs from this repo's CI can
  update over each other — no manual uninstall needed between CI builds.
- All CI APKs are signed with the committed
  `src/android/app/debug.keystore` (stable across runs). They can therefore
  **update over each other**; they can NOT update over an APK signed by a
  different key (e.g. an official upstream release or a locally built APK
  using `~/.android/debug.keystore`): uninstall once, then updates work.
- If a freshly downloaded CI APK performs a fresh **install** instead of an
  update, the previously installed app was signed by a different key — check
  where the old APK came from before assuming the build is wrong.

Every row also inherits Tier 1 (-O3, ThinLTO). The full device mapping (AYN
Thor → `armv9`, Retroid Pocket 5 → `armv82-modern`, etc.) lives in
`devices.yml` and is re-displayed in every CI run summary.

⚠️ **Compatibility**: installing an `armv82-modern`/`armv9` APK on older
hardware **will** crash (SIGILL) — by design. Every target's hardware floor is
listed in `devices.yml`; when in doubt, `universal` always runs.

To add a device: one entry under `devices:` in `devices.yml` (with an existing
`build_target`), commit, push — CI automatically publishes the new APK. For a
new SoC class: a new entry under `build_targets:` plus a matching CMake preset.

### CI/CD caching strategy (no cache entry per build)

- **Cold/warm detection**: the `matrix` job probes the GitHub cache API
  (`gh cache list`) for any `android-ccache-*` entry before anything is
  scheduled.
- **Cold start** (no ccache stored anywhere): a `preseed` job builds ONLY the
  universal profile to fill the shared cache; the matrix then runs on top of
  it.
- **Warm start** (any ccache entry exists): the preseed job is skipped
  entirely and the whole matrix runs in parallel right away.
- **No cache-per-build growth**: ccache keys carry the ISO **week**, not the
  run id (`android-ccache-<week>-<profile>`). The first run of a week saves
  the entry; every other run that week exact-hits it and saves nothing.
  GitHub Actions caches are immutable, so a run-id key would save a new
  multi-GB entry on every single build — the weekly rotation caps it at one
  entry per profile per week.
- **Per-profile ccache is not waste**: `-march`/`-mtune` are part of every
  compile command, so every object file differs between profiles — ccache
  cannot share them (the `ccache -s` stats in the job log make this visible).
  Cross-profile/`restore-keys` fallbacks exist only as a safety net.
- **Shared caches**: CPM source downloads and the Gradle cache (dependencies,
  Kotlin/AGP caches, build cache via `setup-java`'s `cache: gradle`) are
  shared by all matrix jobs since they do not depend on the flags.
- **Why x86_64 runners and not ARM64**: the native build is a plain NDK
  **cross-compilation** — an x86_64 compiler natively producing arm64 code,
  no emulation anywhere. Google publishes no NDK for Linux aarch64 hosts
  (only `linux.zip` x86_64 — verified for r27d through r30), so an ARM64
  runner could only run the compiler under emulation, which is far slower.
  The current slowness of a cold upstream build is the lack of ccache plus
  ThinLTO link time, not the host architecture.

### Local build (single profile)

```bash
# Per-device profile, exactly like the CI matrix:
./gradlew -PdeviceProfile=universal assembleMainlineRelease
./gradlew -PdeviceProfile=modern assembleMainlineRelease   # ARMv8.2+
./gradlew -PdeviceProfile=armv9 assembleMainlineRelease    # ARMv9
```

## Applicability matrix

| Optimization | Scope in the Lichii APK |
|---|---|
| `-O3` | ✅ Tier 1 (universal, default) |
| Per-device-class `-march`/`-mtune` | ✅ Tier 1 (universal ARMv8.0-safe) + Tier 2 (**CI/CD matrix: one APK per class**) |
| ThinLTO (`-flto=thin`) | ✅ Tier 1 (default on Android); desktop unchanged |
| clang + lld | ✅ (already the case — the NDK only emits clang/lld since r19) |
| ccache | ✅ already enabled in CI (`CCACHE=true` → `-DUSE_CCACHE=true` in `.ci/android/build.sh`); locally: `-PYUZU_ANDROID_ARGS="-DUSE_CCACHE=ON"` |
| Strip | ✅ (already the case — AGP strips `.so` symbols in release) |
| jemalloc (`-ljemalloc`) | ❌ Not possible on Android: bionic is the enforced libc; allocator substitution inside an APK process is not reliable (LD_PRELOAD blocked, no global interposition). Android 11+ already uses Scudo; 32-bit processes already use jemalloc. |
| PGO | ⚙️ Done on desktop (see `release-binaries.yml` with `eglantine.profdata`). For Android it would require collecting a profile **on device** (a desktop `.profdata` is unusable cross-target) — future work, not implemented here. |

## Command summary

```bash
# Standard build = already optimized for everyone (universal matrix target)
./gradlew assembleMainlineRelease

# Debuggable variant with the same native performance
./gradlew assembleMainlineRelWithDebInfo

# One specific matrix target (local)
./gradlew -PdeviceProfile=modern assembleMainlineRelease   # ARMv8.2+ only
./gradlew -PdeviceProfile=armv9 assembleMainlineRelease    # ARMv9 only

# Disable ThinLTO temporarily (debug)
./gradlew -PYUZU_ANDROID_ARGS="-DENABLE_LTO=OFF" assembleMainlineRelease
```

## Expected gains

- **ThinLTO**: typically 5–10% on this kind of codebase (cross-TU inlining in
  the shader recompiler, the GPU thread, dynarmic).
- **-O3 on relWithDebInfo**: closes the -O2 → -O3 gap and removes hundreds of
  MB from the `.so`.
- **march crc+crypto / mtune a76**: a few percent (decryption, NEON paths,
  big-core-oriented scheduling).
- The main gain on AYN Thor remains the **GPU fix** described in
  [AYN_THOR_GPU_FIX.md](AYN_THOR_GPU_FIX.md): these optimizations speed up the
  CPU side, they do not replace unblocking presentation.

## Files changed

| File | Change |
|---|---|
| `CMakeLists.txt` | Universal Android optimizations by default (-O3 everywhere, ARMv8.0-safe march, mtune a76) + `universal`, `modern`, `armv9` presets |
| `.ci/android/devices.yml` | **New** — per-device matrix (build_targets + devices, two-level model) |
| `.ci/android/gen_matrix.py` | **New** — generates the GitHub Actions matrix (JSON + Markdown table) from `devices.yml` |
| `externals/cmake-modules/UseLTO.cmake` | `ENABLE_LTO` default ON on Android → `-flto=thin`; desktop unchanged |
| `src/android/app/build.gradle.kts` | `deviceProfile` property (local selection of a matrix target) |
| `.github/workflows/android.yml` | **CI/CD matrix**: `matrix` job (generated from devices.yml) + `build` job as `strategy.matrix` (one APK per target, per-target ccache) + single `release` job (all APKs) |
| `docs/BUILD_OPTIMIZATIONS.md` | This document |
| `README.md` | "Optimized builds" section |
