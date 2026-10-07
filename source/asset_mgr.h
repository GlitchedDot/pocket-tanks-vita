#ifndef PT_ASSET_MGR_H
#define PT_ASSET_MGR_H

#include <stddef.h>
#include <sys/types.h>

// Opaque types matching NDK <android/asset_manager.h>
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;

enum {
    AASSET_MODE_UNKNOWN = 0,
    AASSET_MODE_RANDOM = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER = 3
};

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager);
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
void AAsset_close(AAsset *asset);
off_t AAsset_getLength(AAsset *asset);
int AAsset_read(AAsset *asset, void *buf, size_t count);
off_t AAsset_seek(AAsset *asset, off_t offset, int whence);

// bionic_compat.c: asset FILE* registry for write-path tracing (bug 21).
#include <stdio.h>
void track_asset_file_soloader(FILE *f);
void untrack_asset_file_soloader(FILE *f);

#endif
