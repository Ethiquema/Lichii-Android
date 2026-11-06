# AYN Thor fix — 0% GPU / slideshow gameplay

This document describes the diagnosis and the changes made to fix the bug
observed on the **AYN Thor** (Snapdragon, Adreno): the GPU is unused (0% in the
KGSL monitor), the device stays cool, the CPU is far from saturated, RAM has
headroom — yet the game advances like a slideshow.

## Round 2 (2026-11-07): the GPU is starved by the experimental thread split

Field evidence from an on-device session log (AYN Thor, Donkey Kong Bananza,
APK `nx2-3af00df905` containing round 1) showed a *different* — and dominant —
cause of the 0% GPU: the fork's experimental **GPU thread split**
(`src/video_core/gpu_split.cpp`, `NX2_GPU_THREAD_SPLIT.md`) was enabled by
default for Switch 2 titles and starved the GPU:

```
GPU back thread per second: | other 897.6 ms | draw 4.4 ms (9225/s) | ... | split wait 927.1 ms (629/s) | ...
GPU back thread per second: | other 886.5 ms | draw 5.5 ms (10046/s) | ... | split wait 33.6 ms | ...
```

(10 s profile windows.) The back thread — the only one that touches the
rasterizer — spent ~90% of its time waiting on the split queue or idle, actual
drawing received 4-10 ms per second: the GPU sits at 0% because **no work
reaches it**, while the host CPU is also mostly idle (71-204 ms/s busy) —
everyone waits on everyone.

Fix: the split is now **disabled by default** (`EDEN_NX2_SPLIT=0`); the proven
single-thread GPU path (upstream behavior) runs instead. To experiment again:
`EDEN_NX2_SPLIT=1` or `=2` in `env.txt`. The change is logged at boot:
`GPU thread split: disabled by default (EDEN_NX2_SPLIT=1 or 2 to enable)`.

No rebuild needed to test the disabled split: `env.txt` is read at startup
(`/storage/emulated/0/Android/data/<applicationId>/files/env.txt`, one
`KEY=VALUE` per line) — add `EDEN_NX2_SPLIT=0` and restart the app. When the
edit seems to have no effect, see Round 3 below: the folder depends on the
installed build's package, and the loader now prints that exact path in logcat.

Note: round 1's presentation workaround only triggers on the Qualcomm
**proprietary** driver (`VK_DRIVER_ID_QUALCOMM_PROPRIETARY`); sessions using a
Turnip-based custom driver (e.g. PurpleVK) keep the async present thread, so
the vote floor (30 Hz) and this split fix are the parts that apply there.

## Round 3 (2026-11-07): when `EDEN_NX2_SPLIT=0` silently does not apply

Field report: an APK that predates round 2 still shows a 0% GPU after adding
`EDEN_NX2_SPLIT=0` to `env.txt`. The switch loader already existed in that APK
(and logs every line it reads), so the mechanism works — the edit just never
reaches the process. Three causes, all handled now:

1. **Wrong folder.** The loader reads
   `/storage/emulated/0/Android/data/<applicationId>/files/env.txt` where
   `<applicationId>` is the *installed* build's package, not a constant:

   | Installed build | applicationId | env.txt folder |
   |---|---|---|
   | CI release (`-b Release`, no nightly property) | `dev.lichii.android` | `Android/data/dev.lichii.android/files/` |
   | Local default task (`relWithDebInfo`) | `dev.lichii.android.relWithDebInfo` | `Android/data/dev.lichii.android.relWithDebInfo/files/` |
   | `debug` build | `dev.lichii.android.debug` | `Android/data/dev.lichii.android.debug/files/` |
   | Nightly (`-Pnightly=true`) | `dev.lichii.android.nightly` | `Android/data/dev.lichii.android.nightly/files/` |

   Android 13 blocks file managers from `Android/data`; push the file instead:
   `adb push env.txt /storage/emulated/0/Android/data/<applicationId>/files/env.txt`.
   The loader now logs the exact path it scans on every boot, found or not —
   grep logcat for `env.txt:` and it tells you where the file must go.

2. **UTF-8 BOM on the first line.** An `EF BB BF` prefix made the first key
   unmatchable, so a switch placed at the top of the file was ignored while a
   later line (e.g. a copied `nx2_switches.txt` template) re-set it. The BOM is
   now stripped.

3. **A duplicate assignment later in the file wins.** `env.txt` (and a copied
   `nx2_switches.txt` template) used to ship an active `EDEN_NX2_SPLIT=2`; the
   *last* assignment in the file takes effect. Duplicates are now reported in
   logcat, and the shipped template no longer contains an active
   `EDEN_NX2_SPLIT=2`.

### What logcat should show

- `env.txt: <KEY>=<VALUE>` per line read — or `env.txt: no switches file at
  <path>` when absent; that line also prints the exact folder to use;
- `env.txt: N switch(es) loaded from <path>` at the end of the scan, plus a
  warning when the same key is assigned twice;
- `GPU thread split: disabled by default (EDEN_NX2_SPLIT=1 or 2 to enable)`
  once the game boots (round 2 APK or newer);
- `Present thread: async presentation active on this driver` or the Qualcomm
  GPU-thread line — which presentation path is active;
- `Driver: ...` from the renderer — which Vulkan driver actually loaded
  (PurpleVK/Turnip fork vs the Qualcomm proprietary one).

If `env.txt: EDEN_NX2_SPLIT=0` *does* appear and the GPU is still at 0%, the
split is genuinely off and the cause is different again: capture a fresh
on-device profile with `EDEN_NX2_GPU_PROFILE=1` in `env.txt` (10 s window) and
compare against the round 2 numbers.

### Custom drivers (Turnip / PurpleVK)

With a Turnip-class custom driver the split-off fix, the 30 Hz display-vote
floor and HangWatch are the active changes; the round 1 present workaround
does not trigger. With the Qualcomm proprietary driver this device additionally
spent hours in pipeline compilation before some titles started (field report) —
which is why the custom driver remains the practical choice on this device even
before considering raw performance.

## Diagnosis

### What it is NOT

- **The GPU boost works.** "Force maximum clocks" (`force_max_clock`) enables
  `TurboMode`, which on Android calls `adrenotools_set_turbo(true)`: GPU clocks
  do ramp up on the KGSL side. The boost is not the problem.
- **The rendering backend is Vulkan** (the Android port does not use the Null
  renderer) and renderer creation does not fail (the game is displayed).
- **Resources are not saturated**: this is precisely the key clue. A GPU at 0%
  with an idle CPU does not mean "the GPU is broken", it means "the GPU is
  receiving almost no work": the pipeline is stuck *waiting*, not computing.

### The root cause: a self-locking display refresh-rate vote

The Lichii Android port contains a fork-specific system: `EmuWindow_Android`
votes the display refresh rate via `ANativeWindow_setFrameRate...`
(`src/android/app/src/main/jni/emu_window/emu_window.cpp`) based on the
*observed* presentation speed of the emulator.

Bug sequence on the Thor:

1. At launch, shader compilation drops presentation to 3–10 FPS.
2. The vote follows it down (`ANativeWindow_setFrameRate(surface, ~7)`): the
   Thor's AMOLED panel accepts low modes and **actually drops to ~7–10 Hz**.
3. Presentation runs in FIFO (vsync) mode: every frame waits for the panel's
   vblank. The presentation pace therefore equals the display pace → the
   observed rate stays low → the vote stays low. **Closed loop: the value locks
   in forever**, even after shaders finish compiling and the game is able to
   run fast.
4. Result: permanent slideshow, GPU nearly idle (0% KGSL: only a few ms bursts
   per frame), CPU waiting on vblank, no heat.

On the project's test phone (Galaxy S26, LTPO 120 Hz panel) the effect is
mild; on the Thor it is dramatic.

## Changes

### 1. Floor the display vote at 30 Hz — `emu_window.cpp/.h`

`GetFrameRateHint()` is now a wrapper that enforces a **30 Hz** floor on the
vote sent to the display (`GetRawFrameRateHint()` contains the old logic,
unchanged). The panel can no longer drop below 30 Hz because of the emulator:
the lock-in loop can no longer form, and the vote climbs back to 60/120 Hz as
soon as the game holds speed. A log line was added on every vote change
(`Surface frame rate hint: 60 -> 7.5 Hz`) to make the mechanism visible in
`eden_log.txt`.

### 2. Present on the GPU thread for the proprietary Adreno driver — `vk_present_manager.cpp`

This repository's `docs/DriverBugs.md` lists a known Qualcomm proprietary
driver bug: **"Concurrent fence waits are unsupported"**. With asynchronous
presentation (separate present thread, enabled by default), the GPU thread
waits on fences (`present_done`) while the present thread acquires images and
submits: the driver then serializes the whole device and the GPU sits idle
between frames.

On the **Qualcomm proprietary** driver, presentation now happens on the GPU
thread (a single "waiter"). To force the present thread back on anyway:
`EDEN_FORCE_ASYNC_PRESENT=1` in `env.txt`.

### 3. Active hang watchdog on Android — `vk_master_semaphore.cpp`

`EDEN_NX2_HANG_WATCH` is now **enabled by default on Android** (can be disabled
with `EDEN_NX2_HANG_WATCH=0`). If a GPU tick stops advancing, the wait is
logged every 3 seconds instead of blocking silently — the diagnostic tool for
any residual driver-side stall.

## How to test

1. Install the APK rebuilt from this repository (or
   `./gradlew assembleMainlineRelWithDebInfo`).
2. Launch a game, let shader compilation finish.
3. Check `Android/data/dev.lichii.android/files/log/eden_log.txt`:
   - `Surface frame rate hint: ...`: the value must no longer go below 30;
   - `Qualcomm Adreno: presenting on the GPU thread...`: confirms the driver
     bug workaround;
   - any `Waited ... s for the GPU thread to submit tick ...` or
     `Waiting for GPU tick ... failed` line signals a residual stall worth
     investigating.
4. Watch the GPU (KGSL monitor / AYN dashboard): load should now appear and
   scale with the internal resolution.

## Recommended settings on AYN Thor

- **GPU driver**: install a **recent but not broken Turnip** driver through the
  app's driver manager (community feedback indicates some Turnip T29 builds
  are defective on this model; T28 and the newer fixed builds work). The
  Qualcomm proprietary driver also runs the fix, but field reports on this
  device show hours-long pipeline compilation before some titles start — the
  custom driver is the practical choice regardless of raw performance.
- **Resolution** 1×, **handheld mode**, on-disk shader cache enabled.
- The GPU thread split is **off by default**; re-enable it only for tests
  (`EDEN_NX2_SPLIT=1` or `=2` in `env.txt`), and compare against a fresh
  on-device `Nx2Profile` report.

## Files changed

| File | Change |
|---|---|
| `src/android/app/src/main/jni/emu_window/emu_window.cpp` | 30 Hz floor on the display vote + vote logging |
| `src/android/app/src/main/jni/emu_window/emu_window.h` | `GetRawFrameRateHint()` declaration |
| `src/video_core/renderer_vulkan/vk_present_manager.cpp` | Proprietary Adreno: present on the GPU thread (env-overridable) |
| `src/video_core/renderer_vulkan/vk_master_semaphore.cpp` | HangWatch enabled by default on Android |
| `src/video_core/gpu_split.cpp` | GPU thread split disabled by default (opt-in via `EDEN_NX2_SPLIT`) — round 2 |
| `NX2_GPU_THREAD_SPLIT.md` | Split is now opt-in, with the on-device rationale |
| `src/android/app/src/main/jni/android_env_file.cpp` | env.txt loader: logs the scanned path, strips the UTF-8 BOM, reports duplicate assignments — round 3 |
| `dist/nx2_switches.txt` | Template no longer ships an active `EDEN_NX2_SPLIT=2` — round 3 |
| `src/video_core/renderer_vulkan/vk_present_manager.cpp` | Async-present decision logged on custom drivers too — round 3 |
| `docs/AYN_THOR_GPU_FIX.md` | This document |
| `README.md` | "AYN Thor / Snapdragon devices" section |
