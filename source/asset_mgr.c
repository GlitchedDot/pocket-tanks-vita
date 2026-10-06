#include "asset_mgr.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSET_ROOT "ux0:data/pockettanks/assets/"

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
    if (!f) {
        log_warn("AAssetManager_open: not found: %s", path);
        return NULL;
    }

    AAsset *a = (AAsset *)calloc(1, sizeof(AAsset));
    if (!a) {
        fclose(f);
        return NULL;
    }
    a->f = f;
    fseek(f, 0, SEEK_END);
    a->size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return a;
}

void AAsset_close(AAsset *asset) {
    if (!asset)
        return;
    if (asset->f)
        fclose(asset->f);
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
