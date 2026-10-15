# Spritecache rebuild fix — ready to apply (NOT applied)

**Prepared:** 2026-10-13 evening, during the Build I test window.
**Status:** draft only. Loader source untouched — apply with the GAME-scene
work, after Bug 32 closes. Harmless for TITLE (verified in bug32-analysis.md §5).

## The defect (re-verified against bootlog-10, build H)

Every boot, the engine deletes its three sprite caches and tries to rebuild
them:

- `sprite_1024_960x544.spritecache`
- `hdsprite_1024_960x544.spritecache`
- `iconssprite_1024_960x544.spritecache`

The rebuild opens `<name>.spritecache.tmp` through `AAssetManager_open`
(caller 0x993d8c47) with the absolute path
`ux0:data/pockettanks/cache//cache/<name>.spritecache.tmp` — and gets NULL.

Root cause, in `pockettanks-vita/source/asset_mgr.c`: `AAssetManager_open`
ignores its `mode` argument; the data-root fallback tries `fopen(path, "r+b")`
then `fopen(path, "rb")`. Neither can create a missing file, so a brand-new
cache can never be born through this shim.

## What is NOT the problem (checked in bootlog-10, so don't chase these)

- **The directory exists and is writable.** The same `cache//cache/` folder
  takes plain `fopen(..., "wb")` writes fine — the `*.bbk_960x544_id` art-pack
  stamp files are written there successfully in the same run. No mkdir needed.
- **TITLE doesn't need the caches.** Its art is per-file PNGs under
  `zoom/title/`, all loading fine. This fix matters at the GAME scene, whose
  rendering uses the sprite/icon atlases.

## The fix (one scoped change in asset_mgr.c)

In `AAssetManager_open`, in the data-fallback branch, after the `"r+b"` /
`"rb"` attempts both fail, allow creation **only** for `.tmp` targets
(the engine's cache-rebuild temp files — the only creation-through-asset_open
case observed in any bootlog):

```c
        f = fopen(path, "r+b");
        if (f)
            is_data_fallback = 1;
        else {
            f = fopen(path, "rb");
            if (!f) {
                // Spritecache rebuild: the engine creates
                // "<name>.spritecache.tmp" through AAssetManager_open;
                // "r+b"/"rb" can never create it. Scope creation to
                // .tmp targets so no read path can truncate a file.
                size_t plen = strlen(path);
                if (plen > 4 && strcmp(path + plen - 4, ".tmp") == 0) {
                    f = fopen(path, "w+b");
                    if (f)
                        is_data_fallback = 1;
                }
            }
        }
```

Notes for whoever applies it:

- `"w+b"` truncation is safe here precisely because of the `.tmp` scope:
  a `.tmp` that already exists at open time is a stale rebuild artifact.
- The existing tail of the function handles the rest: the new empty file
  gets `a->size = 0` via the SEEK_END ftell, and the FILE* is registered
  with `track_asset_file_soloader`, so the engine's writes through the
  asset handle are already traced/supported (same path as the Bug 19
  engine.cfg write fix).
- After writing, the engine renames/deletes through its own file layer
  (plain fopen wrappers — the "Deleting file: ...spritecache" lines in the
  game log), which already works; no loader change needed there.

## How a test run proves it worked

In the first bootlog with the fix in:

1. `asset_open("ux0:data/pockettanks/cache//cache/sprite_1024_960x544.spritecache.tmp")`
   returns non-NULL with the `(data-dir rw)` tag — for all three caches.
2. `ux0:data/pockettanks/cache/cache/` gains `.spritecache` files
   (visible in VitaShell, or via the engine's own fopen traffic).
3. No regression check needed for TITLE — but as ever, what's on screen
   is Lola's report, not the log's.
