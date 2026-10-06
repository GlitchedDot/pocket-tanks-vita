# Pocket Tanks for PS Vita

An unofficial PS Vita port of **Pocket Tanks** (Android v3.0.0) by BlitWise Productions,
built with the `.so`-loader method: the game's original ARMv7 native library is loaded
on the Vita through **kubridge**, with **FdFix** handling file redirection and **vitaGL**
providing the GLES 1.1 graphics.

## Requirements

- A hacked PS Vita (HENkaku / enso) with the kernel plugins installed:
  - `kubridge.skprx`
  - `fd_fix.skprx`
- **Your own legally obtained copy of the Pocket Tanks Android APK** (v3.0.0,
  package `com.blitwise.ptankshd`). Game assets and binaries are **not** included in
  this repository — you must supply them yourself. See `docs/data_prep.sh`.

## Data setup

```sh
# Needs: unzip, ffmpeg
./docs/data_prep.sh /path/to/pocket-tanks-3.0.0.apk
```

This produces `pockettanks-data/` containing the native libraries, game assets
(music converted from M4A to OGG), and `res/`. Copy the **contents** of that folder
to `ux0:data/pockettanks/` on your Vita (VitaShell USB or FTP).

## Building

You need [VitaSDK](https://vitasdk.org/). **The soft-float SDK is required** — the
game's native library uses the soft-float ABI, and a hard-float build will not link
against it.

```sh
export VITASDK=$HOME/workspace/vitasdk-softfp   # softfp SDK, mandatory
mkdir build && cd build
cmake ..
make
```

This produces `pockettanks.vpk`. Install it with VitaShell.

> Note: the LiveArea PNGs in `extras/livearea/` are 8-bit indexed palette images.
> The Vita firmware rejects truecolor PNGs at install time (error `0x8010113D`), so
> keep them indexed if you ever regenerate the artwork.

## Installing

1. Install `kubridge.skprx` and `fd_fix.skprx` as kernel plugins, reboot.
2. Copy the prepared game data to `ux0:data/pockettanks/`.
3. Install `pockettanks.vpk` with VitaShell.
4. Launch the bubble. If it crashes, a `psp2dmp` crash dump in `ux0:data/` helps debug.

## Status — v1.0

First public build. The game boots through the loader; this is early bring-up:

- Audio is **stubbed** in v1 (the game's FMOD init was proven non-fatal, so the game
  runs silent rather than crashing). Real audio via OpenSL ES is planned.
- Pause/resume lifecycle is not wired yet.
- Expect bugs — crash dumps welcome as issues.

## How it works

`source/` reimplements the game's thin Java driver layer (`CPActivity`/`CPView`/
`CPJNILib`) in C: EGL context setup and frame loop (`egl_graphics.c`), the 19 JNI
entry points the engine expects (`main.c`, `jni_bridge.c`), native→Java callbacks
(paths, device info, billing kill-switch, stubs for ads/sharing/HTTP), touch input
via a fake `MotionEvent` (`fake_motionevent.c`), full gamepad→Android keycode mapping
(`input.c`), Bionic libc shims (`bionic_compat.c`, `bionic_ctype.c`), Android logging
(`android_log.c`), and a 450-entry symbol table (`dynlib.c`) resolving every import
of the game's libraries against Vita equivalents.

Vendored under `lib/`: [FalsoJNI](https://github.com/xerpi/falso_jni) (MIT, © Volodymyr
Atamanenko) and `so_util` (kubridge's ELF loader helper).

## Credits

- Game © BlitWise Productions (Michael P. Welch). All game content belongs to its
  respective owners; this repository contains only the loader code, no game assets.
- Loader code in `source/` is MIT licensed — see LICENSE.
- Built on the shoulders of the Vita homebrew scene: kubridge, FdFix, vitaGL,
  VitaSDK.
