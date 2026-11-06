<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="600">
</p>

# Lichii for Android

An Android port of the [Lichii](https://github.com/zortos293/Eglantine) Nintendo Switch / Switch 2 emulator, maintained by [pavelgajdo](https://github.com/pavelgajdo). It focuses on getting Switch 2 games running on Android phones, starting with Samsung's Exynos phones and their AMD-based Xclipse GPUs.

This is an independent, experimental fork. It is not affiliated with Nintendo or with the upstream projects it builds on.

## Download

- **Releases:** [github.com/pavelgajdo/Lichii-Android/releases](https://github.com/pavelgajdo/Lichii-Android/releases). Download the `.apk` from the latest `android-*` release and install it on your phone. You may need to allow installs from your browser or file manager.
- **Test builds:** the `android-latest` pre-release is rebuilt automatically from every change on `nx2`. It has the newest fixes and the newest bugs.

Requirements: an arm64 phone with Android 13 or newer and Vulkan 1.3. Games, keys and firmware are not included. Only use games and system files you have legally obtained.

## Tested on

| | |
|---|---|
| Phone | Samsung Galaxy S26 (SM-S942B) |
| SoC | Exynos 2600 |
| GPU | Samsung Xclipse 960 (AMD RDNA3, GFX11) |
| RAM | 12 GB |
| OS | Android 17 (One UI) |
| GPU driver | Custom Mesa RADV build for Xclipse ("RADV Xclipse", Mesa 27.0-devel), installed through the app's GPU driver manager |

Samsung's own driver on the S26 is also RADV-based and works, but on this phone it measured about 1.8× slower on the same scene than the custom RADV build.

Other phones may work, but this is the only device the fork is tested on so far.

> 📺 **Game Testing & Video Footage:** Check out gameplay performance tests and video showcases on the [Exynos Gaming YouTube Channel](https://www.youtube.com/@ExynosGaming-t7k).

## Game status

| Game | Status |
|---|---|
| The Legend of Zelda: Ocarina of Time (Switch 2) | Boots. The title screen and the pre-rendered intro movies run at 60 FPS. **It currently hangs while loading the first gameplay scene after the intro** (being worked on). |

Only results tested on the phone above are listed here.

### What's different from upstream

- AV1 movies are decoded with Android's hardware decoder (MediaCodec).
- Ocarina of Time's movies skip the game's movie-enhancement neural network (which needs NVIDIA tensor cores), so they play at full speed instead of about 8 FPS.
- Emulated tensor-core math (HMMA) is about twice as fast on RADV.
- `EDEN_*` diagnostic switches can be set on Android with an `env.txt` file (see below).

## Recommended settings

- **System:** Handheld mode.
- **CPU:** NCE backend.
- **Graphics:** 1× resolution. Install a custom RADV driver if your phone has an Xclipse GPU.

## AYN Thor / Snapdragon devices

On the AYN Thor the game could run as a slideshow with the GPU reported at 0% while the
CPU stayed mostly idle: the emulator's display frame-rate vote could lock the panel at a
low refresh rate during shader compilation, and the proprietary Adreno driver does not
support concurrent fence waits (present thread disabled for it). The display vote is now
floored at 30 Hz and a stall watchdog logs stuck GPU ticks. See
[docs/CORRECTIF_AYN_THOR.md](docs/CORRECTIF_AYN_THOR.md) for the full analysis, the
recommended Turnip driver versions and the `env.txt` switches.

## Reporting problems

[Open an issue](https://github.com/pavelgajdo/Lichii-Android/issues) with:

- your phone model, Android version and GPU driver;
- the game and what happened (screenshots help);
- the log from the affected session: `Android/data/dev.lichii.android/files/log/eden_log.txt` (`dev.lichii.android.relWithDebInfo` for test builds).

Check logs for personal information before uploading. Never upload games, keys, firmware or account data.

### Diagnostic switches

Android can't pass environment variables to an app, so Lichii reads them from `Android/data/<app id>/files/env.txt`, one `KEY=VALUE` per line (`#` starts a comment). They take effect the next time the app starts. The available switches are listed in [AGENTS.md](AGENTS.md#debugging-tools). Leave the file empty for normal play: most switches slow the emulator down.

## Building

```bash
cd src/android
./gradlew assembleMainlineRelWithDebInfo
```

You need the Android SDK, NDK 28.2 and CMake 3.22. The APK is written to `src/android/app/build/outputs/apk/mainline/relWithDebInfo/`. CI builds every push (see [.github/workflows/android.yml](.github/workflows/android.yml)). Pushing a tag named `android-<version>` publishes a release with the APK.

More documentation, inherited from upstream: [build guide](docs/Build.md), [coding guidelines](docs/Coding.md).

## Credits and license

Lichii for Android builds on [Lichii](https://github.com/zortos293/Eglantine) by Zortos and on Eglantine, Eden, yuzu and Sudachi, and their contributors, including Eglantine's original developer Camille LaVey. Lichii artwork by YOB (yobslab). Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for the applicable license terms.
