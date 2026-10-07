#include "asset_mgr.h"
#include "utils.h"
#include "bootlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSET_ROOT "ux0:data/pockettanks/assets/"
#define DATA_PATH "ux0:data/pockettanks/"

struct AAssetManager {
    int dummy;
};

struct AAsset {
    FILE *f;
    long size;
};

static AAssetManager g_mgr;

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager) {
    (void)env;
    (void)assetManager;
    // We ignore the fake jobject; assets are resolved from ASSET_ROOT.
    return &g_mgr;
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    (void)mgr;
    (void)mode;
    if (!filename)
        return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", ASSET_ROOT, filename);

    FILE *f = fopen(path, "rb");
    int is_data_fallback = 0;
    if (!f) {
        // Bug 13/13b: the game's file layer routes some paths (e.g. its
        // preferences file) to AAssetManager_open instead of fopen, so they
        // never hit our traced IO wrappers. Fall back to the writable data
        // root for files that aren't APK assets -- but only when the name is
        // relative. The game sometimes passes an already-absolute Vita path
        // (e.g. "ux0:data/pockettanks/engine.cfg"); prepending the data root
        // to that doubles it ("ux0:data/pockettanks/ux0:data/..."), so try
        // absolute names as-is.
        if (strchr(filename, ':') == NULL && filename[0] != '/') {
            snprintf(path, sizeof(path), "%s%s", DATA_PATH, filename);
        } else {
            snprintf(path, sizeof(path), "%s", filename);
        }
        // Bug 19: the game also routes WRITES (engine.cfg preferences save)
        // through AAssetManager_open. A read-only "rb" handle makes its
        // cp_fwrite fail. Data-dir files aren't APK assets, so open them
        // read/write; the game can only get a FILE* via our AAsset struct,
        // and "r+b" keeps reads working while allowing writes.
        f = fopen(path, "r+b");
        if (f)
            is_data_fallback = 1;
        else
            f = fopen(path, "rb");
    }
    blog("IO: asset_open(\"%s\") -> \"%s\" %p%s caller=%p", filename, path, (void *)f,
         is_data_fallback ? " (data-dir rw)" : "",
         __builtin_return_address(0));
    if (!f) {
        log_warn("AAssetManager_open: not found: %s", filename);
        return NULL;
    }

    AAsset *a = (AAsset *)calloc(1, sizeof(AAsset));
    if (!a) {
        fclose(f);
        return NULL;
    }
    a->f = f;
    // Bug 21: log the AAsset* token too (the game may use it as a cookie),
    // and register the FILE* so the write-path wrappers can flag writes
    // through asset handles.
    blog("IO: asset_open(\"%s\") -> AAsset %p (FILE %p)", filename, (void *)a,
         (void *)f);
    track_asset_file_soloader(f);
    fseek(f, 0, SEEK_END);
    a->size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return a;
}

void AAsset_close(AAsset *asset) {
    if (!asset)
        return;
    if (asset->f) {
        untrack_asset_file_soloader(asset->f);
        fclose(asset->f);
    }
    free(asset);
}

off_t AAsset_getLength(AAsset *asset) {
    return asset ? asset->size : 0;
}

int AAsset_read(AAsset *asset, void *buf, size_t count) {
    if (!asset || !asset->f)
        return -1;
    return (int)fread(buf, 1, count, asset->f);
}

off_t AAsset_seek(AAsset *asset, off_t offset, int whence) {
    if (!asset || !asset->f)
        return -1;
    if (fseek(asset->f, offset, whence) != 0)
        return -1;
    return ftell(asset->f);
}
