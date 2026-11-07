# Eden SX2A: developer notes

Android port of Switch 2 emulation on top of
[PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork) 2.0.0 (a fork of Eglantine, which builds
on Takoroka's eden-nx2). This branch is PokeDinner's history plus this fork's commits; the user-facing overview is in
[README.md](README.md). v0.2.1 and v0.2.2 were based on Eden-NS2, older versions on Takoroka's eden-nx2.

## Following PokeDinner

```sh
git remote add pokedinner https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork.git
git fetch pokedinner
git log --oneline HEAD..pokedinner/master      # read each new commit first
git merge pokedinner/master
```

- Merge, never rebase onto PokeDinner, so the next merge stays easy.
- `README.md`: this fork's section sits on top of PokeDinner's README. On a conflict keep ours at the top and take
  PokeDinner's text below the divider.
- Likely conflict spots: `src/core/hle/service/nx2_gsv.h` (present crop, vi2 107),
  `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h` (register HSETP2, `0x234`),
  `deconstructed_rom_directory.cpp` (loose DLC), and `src/core/arm/nce/visitor_base.h`, which must match every entry in
  dynarmic's A64 `a64.inc` / `impl.h`.
- Android builds with clang and `-Werror`; PokeDinner targets desktop compilers, so new code can break the Android
  build. Prefer a warning flag in `src/CMakeLists.txt` over editing PokeDinner's files.

## What this fork changes

Android app:
- 10GB / 12GB memory layouts in the UI; Switch 2 defaults.
- Loose ExeFS launch by real path ("Launch ExeFS game folder", `exefs_path` intent extra), loose games listed from game
  folders, a chooser when a folder holds several dumps; loose DLC from `dlc/<title id>/<version>/romfs`.
- Settings -> Switch 2 readiness check.
- Bundled Mr. Purple Turnip T30, selected once on Adreno devices that have no driver chosen.
- Swiping the app from recents ends the process; presenting after the surface is gone drops the frame.
- Release builds don't read `files/nx2_env.txt` (`-DEDEN_NX2_NO_ENV_FILE`); debug builds turn its `KEY=VALUE` lines
  into environment variables at app start.

Core and GPU:
- NCE visitor stubs for the Switch 2 A64 decoder entries (Android build).
- arm64 (oaknut) backend for the Turing macro JIT, with `tools/nx2/jit_fuzz` (0 mismatches against the interpreter).
- Presents are cropped to the visible size the game passes with vi2 229 (word 7); vi2 107 succeeds quietly.
- Register HSETP2 (`0x234`) is lowered, so a game's post-processing runs and its upscaler stand-in can
  replace the tensor-core pass.
- A game's QMD layouts; drawing and syncpoint ids out of range are dropped.
- No host occlusion queries for Switch 2 programs (Adreno crash in `vkCmdBeginQuery`); Qualcomm/ARM storage buffer cap
  uses the plain limit; storage CAS write helper defined whenever used.
- Texel views of unmapped texture buffers get a format (a game); render targets no host can create are left unbound.
- Dynarmic arm64 per-block info in vectors; fiber stacks left uninitialized.

## Build

```sh
tools/nx2/android/build_install.sh     # RelWithDebInfo, installs with adb --user 0
```

Release APKs are built with `-ffile-prefix-map` so no build paths end up in the binary.
