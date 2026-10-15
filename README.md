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

> One vendored file is not mirrored in this git tree: `source/stb_vorbis.c`
> (unmodified upstream **stb_vorbis v1.22**, public domain, ~193 KB) exceeds the
> file-size limit of the tooling used to publish this repo. Fetch it from the
> [stb repository](https://github.com/nothings/stb) and drop it into `source/`
> before building.

> Note: the LiveArea PNGs in `extras/livearea/` are 8-bit indexed palette images.
> The Vita firmware rejects truecolor PNGs at install time (error `0x8010113D`), so
> keep them indexed if you ever regenerate the artwork.

## Installing

1. Install `kubridge.skprx` and `fd_fix.skprx` as kernel plugins, reboot.
2. Copy the prepared game data to `ux0:data/pockettanks/`.
3. Install `pockettanks.vpk` with VitaShell.
4. Launch the bubble. The loader writes a log to
   `ux0:data/pockettanks/bootlog.txt` (the first lines name the exact build), and a
   `psp2dmp` crash dump in `ux0:data/` helps debug if it crashes.

## Status — October 2026 (sources = Build BO)

- **The game plays on real hardware.** Full matches confirmed by the tester on
  2026-10-14 (Build Z and later): menus, shop, weapon selection, and matches all work.
- **Music works** (confirmed by ear, Build AS). The data pack's tracks ship as OGG
  but the engine asks for M4A by name — the loader maps the extension and decodes
  OGG itself (vendored `stb_vorbis`) into a looping `sceAudioOut` port, with track
  switching and mute working.
- **Sound effects are the active frontier.** FMOD is being brought up layer by layer
  (see the problem log below). The current sources are Build BO (2026-10-14),
  delivered for hardware testing; whether FMOD `System::init` finally succeeds —
  and whether SFX are audible — is pending that test.
- All 18 Deluxe weapon packs are unlocked via the purchase stubs, and the sprite
  caches persist between boots.

## Problem log — what broke, and where it stands

Every numbered problem the port hit, with its current status. Full forensic
write-ups for Bugs 29–37 live in `docs/analysis/`.

### Fixed

- **Bugs 1–12 — initial bring-up.** Loader crashes through C++ static init:
  mutex/threading shims, thread-local-storage slot fixes, and a per-initializer
  flight recorder that pinpointed a bad Boost threading initializer. All 220
  initializers now run into graphics init and beyond.
- **Bug 13 — doubled asset paths.** `AAssetManager_open` was handed already-absolute
  paths and the loader prepended the data root again
  (`ux0:data/pockettanks/ux0:data/pockettanks/…`). Absolute paths are now tried as-is.
- **Bugs 15/16/18 — the 0x0C heap scribbler.** Parsed preferences kept corrupting
  dlmalloc chunk headers with runs of `0x0C`. After two wrong theories (a JSON
  serializer indent bug; a fixed 256-byte serializer buffer — the loader still
  defensively enlarges that buffer), the true cause was found: `__aeabi_memset`
  takes its arguments as `(dest, n, c)` — **reversed** versus ANSI `memset` — and
  the symbol table mapped it straight to `sceClibMemset`. A wrapper now restores
  the correct order.
- **Bug 19 — `engine.cfg` writes failed.** The data-dir fallback for asset opens
  was read-only (`"rb"`); it now opens `"r+b"`, with write wrappers that log short
  writes.
- **Bugs 20–22 — `CPJNIParse` "not found".** The game never calls `FindClass` for
  it; classes are resolved through `ClassLoader.findClass` during `JNI_OnLoad`,
  and that whole chain was missing from FalsoJNI. The chain is stubbed, the
  CPJNIParse/CPJNISound/CPJNI bridges are registered, and `android.graphics.Rect`
  fields return the real screen rect instead of garbage.
- **Bug 23 — preference saves were dead.** The engine chooses its I/O path by
  testing `filename[0] == '/'`; Vita paths start with `u` (`ux0:…`), so every save
  took a path that cannot write. The loader now applies a 2-byte **runtime patch**
  to `libengine.so` after loading it (tests for `'u'` instead of `'/'`), only when
  the bytes at the patch site match the pristine APK copy.
- **Bugs 24–26 — the `0x42424242` crash.** FalsoJNI's `GetObjectClass` returned an
  "unimplemented" sentinel that the engine passed as a class name into `%s`
  logging. It now returns a real dummy jobject string.
- **Bug 27 — `ptree_bad_path` aborts.** The engine's property-tree `get_child`
  aborted on missing nodes instead of returning a default. `ptree_fake.cpp`
  supplies a real empty ptree for not-found lookups.
- **Bug 28 — `boost::bad_lexical_cast` on weapon data.** Uncaught lexical-cast
  failure while loading the weapon lists; fixed together with the Bug 27 rework.
- **Bug 29 — exception unwinding died.** A `Find_exidx` lookup landed on an
  address inside the loader itself and killed what became the port's first
  properly *caught* exception. The lookup is now contained to the loaded modules.
- **Bug 30 — pthread TLS.** `pthread_getspecific` misbehaved for the engine's
  keys; fixed with a single binding change in `dynlib.c`.
- **Bug 31 — drawn but never shown.** Frames were rendered and never presented;
  the present/swap path is fixed.
- **Bug 32 — title screen hidden behind the loading art.** The TITLE scene was
  active while the display kept the loading screen; menus now render.
- **Bug 33 — gamepad buttons dead.** The engine builds its controller map from
  Android keycode constants; view registration plus KeyEvent/axis wiring fixed it.
- **Bug 35 — no `/dev/urandom`.** Boost's UUID generator had no entropy source;
  the loader shims it.
- **Bug 36 — Shop aborted on entry** with an uncaught `boost::condition_error`.
  The Shop now opens and stays open.
- **Bug 37 — SHOP→GAME transition never armed.** Root cause was ours: the
  loader's diagnostic init-skip list — a leftover from early crash isolation —
  was still skipping the GAME scene's initializer. Removed in Build Z; matches
  confirmed playing on hardware.
- **Bug 38 — sprite caches didn't persist** between boots. Fixed in Build AA;
  all 9 cache files persist, and Deluxe weapons are usable in-match.
- **Bug 39 — keyboard visibility race** at player setup. Latching
  `setKeyboardVisibility(true)` fixed the flickering active flag.
- **FMOD, layers 1–3.** Error 41 was FMOD's NEON gate reading a nonexistent
  `/proc/cpuinfo` — the loader now serves a truthful Cortex-A9 cpuinfo. Error 54
  (no output) — the loader forces FMOD's OpenSL ES output plugin. Error 28 —
  FMOD's Java callbacks were unregistered; `org.fmod.FMOD` statics now answer
  (sample rate 44100, block size 1024, `checkInit` true). Plugin census confirms
  7 outputs / 16 codecs / 32 DSPs.

### Still open / in progress

- **FMOD sound effects (the current hunt).** FMOD's OpenSL layer calls its host
  with NULL interface IDs, NULL `self` pointers, and out-pointers pointing into
  read-only module memory. The loader now answers interface queries with each
  object's primary interface, survives NULL-self calls through a singleton, and
  — new in Build BO — refuses to write through any out-pointer outside writable
  RAM (`0x81000000–0x83000000`). Awaiting a hardware test of Build BO.
- **Bug 40 — system IME keyboard for the name field.** Sony's whole
  common-dialog layer refuses to initialize inside the loader process:
  `sceImeDialogInit` returns `0x800204c4`, and a plain message dialog fails
  identically. Parked. Any future experiment must be a standalone throwaway test
  app — a boot-time dialog probe once hard-locked the Vita (Build AG, binned).
- **Volume sliders have no effect.** Mute works; the music/SFX sliders don't.
  No volume calls reach the loader in any log yet — to be retested once FMOD
  init lands.
- **Physical-button control for the match HUD** — Cross = Fire, D-pad = Angle /
  Power sliders. Designed, not yet built; front touch works in the meantime.
- **Rear touchpad mapping.** Planned, after the small things above.
- **§4.4 dispatch interposition.** One interposed dispatch entry stays in every
  build until an A/B run (with/without it) clears removing it.
- **Frame dips during heavy explosions.** Believed to be hardware headroom;
  not yet profiled.
- **Cleanup build + release.** A final build will strip the bootlog/diagnostic
  instrumentation, then the v1.0 tag and GitHub Release.

## How it works

`source/` reimplements the game's thin Java driver layer (`CPActivity`/`CPView`/
`CPJNILib`) in C: EGL context setup and frame loop (`egl_graphics.c`), the JNI
entry points the engine expects (`main.c`, `jni_bridge.c`), native→Java callbacks
(paths, device info, billing, stubs for ads/sharing/HTTP), touch input via a fake
`MotionEvent` (`fake_motionevent.c`), gamepad→Android keycode mapping (`input.c`),
Bionic libc shims (`bionic_compat.c`, `bionic_ctype.c`, `bionic_pthread.c`),
Android logging (`android_log.c`), a boot logger (`bootlog.c`) whose first lines
identify the exact build, a 400+-entry symbol table (`dynlib.c`) resolving every
import of the game's libraries against Vita equivalents, a music player
(`music.c` + vendored `stb_vorbis`), and an FMOD call tracer (`fmod_trace.c`).

Vendored under `lib/`: [FalsoJNI](https://github.com/xerpi/falso_jni) (MIT,
© Volodymyr Atamanenko), `so_util` (kubridge's ELF loader helper), and
`stb_vorbis` (public domain, Sean Barrett).

## Credits

- Game © BlitWise Productions (Michael P. Welch). All game content belongs to its
  respective owners; this repository contains only the loader code, no game assets.
- Loader code in `source/` is MIT licensed — see LICENSE.
- Built on the shoulders of the Vita homebrew scene: kubridge, FdFix, vitaGL,
  VitaSDK.
