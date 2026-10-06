# Pocket Tanks — PS Vita Port (v1)

Android → Vita `.so` port of Pocket Tanks v3.0.0 (com.blitwise.ptankshd)
via kubridge + FdFix + vitaGL + FalsoJNI. Built with the softfp VitaSDK
(the game `.so` is soft-float ABI).

## Status: v1 — graphics + input bring-up build (no audio)

Audio is stubbed in v1: FMOD init failure is proven non-fatal in the game's
disassembly, so the game boots silent. OpenSL ES bring-up is the next step.

## Install

### On the Vita (required first)
- `kubridge.skprx` and `fd_fix.skprx` kernel plugins installed and loaded.

### 1. Prepare the game data (on your PC/Mac)
You need the Pocket Tanks Android APK (v3.0.0, package `com.blitwise.ptankshd`).

```sh
./data_prep.sh /path/to/pocket-tanks-3.0.0.apk
```

This creates `pockettanks-data/` with the native libs, assets, and
`res/`, and converts the M4A music to OGG. Requires `unzip` and `ffmpeg`.

### 2. Copy data to the Vita
Copy the **contents** of `pockettanks-data/` to `ux0:data/pockettanks/`
via VitaShell USB or FTP, so it looks like:

```
ux0:data/pockettanks/
  libengine.so
  libfmod.so
  libc++_shared.so
  libsqlcipher.so
  assets/
  res/
  cache/
```

### 3. Install the VPK
Install `pockettanks.vpk` with VitaShell, then launch the bubble.

## Controls
- **Touch**: front touchscreen (menus, aiming)
- **Gamepad**: D-Pad, Cross/Circle/Square/Triangle, L1/R1, Start/Select,
  both analog sticks — mapped to the game's Android keycodes/axes
- Accelerometer: stubbed (this game doesn't use tilt)

## Known v1 limitations
- No audio (FMOD/OpenSL stubbed; init is non-fatal by design)
- No pause/resume lifecycle yet (suspend the Vita at your own risk)
- 2-player (playerIndex 1) not wired up
- Shop is disabled (`isBillingEnabled=false`) — free weapons only

## Troubleshooting
- "Data files not found" on boot: the `ux0:data/pockettanks/` layout above
  is wrong or incomplete.
- Black screen: check that `assets/` was copied (the game reads art via
  the asset manager).
- Crash on boot: make sure `kubridge.skprx` is loaded (the loader needs it
  for NDK shims and unrestricted memory copies).

## Build from source
```sh
export VITASDK=$HOME/workspace/vitasdk-softfp   # softfp MANDATORY
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
Output: `build/pockettanks.vpk`.
