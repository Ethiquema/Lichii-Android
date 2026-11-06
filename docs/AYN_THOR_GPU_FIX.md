# AYN Thor fix — 0% GPU / slideshow gameplay

This document describes the diagnosis and the changes made to fix the bug
observed on the **AYN Thor** (Snapdragon, Adreno): the GPU is unused (0% in the
KGSL monitor), the device stays cool, the CPU is far from saturated, RAM has
headroom — yet the game advances like a slideshow.

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
  Qualcomm proprietary driver also works with this fix.
- **Resolution** 1×, **handheld mode**, on-disk shader cache enabled.
- If a title stays stuck, try `EDEN_NX2_SPLIT=0` in `env.txt` (disables the
  GPU thread split, an untested feature on Snapdragon).

## Files changed

| File | Change |
|---|---|
| `src/android/app/src/main/jni/emu_window/emu_window.cpp` | 30 Hz floor on the display vote + vote logging |
| `src/android/app/src/main/jni/emu_window/emu_window.h` | `GetRawFrameRateHint()` declaration |
| `src/video_core/renderer_vulkan/vk_present_manager.cpp` | Proprietary Adreno: present on the GPU thread (env-overridable) |
| `src/video_core/renderer_vulkan/vk_master_semaphore.cpp` | HangWatch enabled by default on Android |
| `docs/AYN_THOR_GPU_FIX.md` | This document |
| `README.md` | "AYN Thor / Snapdragon devices" section |
