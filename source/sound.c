#include "sound.h"
#include "utils.h"

// Dummy IIDs — never dereferenced since slCreateEngine fails.
const unsigned char SL_IID_ENGINE[16] = {0};
const unsigned char SL_IID_PLAY[16] = {0};
const unsigned char SL_IID_BUFFERQUEUE[16] = {0};
const unsigned char SL_IID_VOLUME[16] = {0};

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions,
                        const SLEngineOption *pEngineOptions, SLuint32 numInterfaces,
                        const void *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    (void)pEngine;
    (void)numOptions;
    (void)pEngineOptions;
    (void)numInterfaces;
    (void)pInterfaceIds;
    (void)pInterfaceRequired;
    // v1: no audio. The engine's init checks every FMOD_RESULT / SLresult
    // and bails gracefully (proven in disassembly), so this is safe.
    log_info("slCreateEngine: stubbed (no audio in v1)");
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
