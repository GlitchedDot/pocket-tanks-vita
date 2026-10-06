#ifndef PT_SOUND_H
#define PT_SOUND_H

// OpenSL ES stub surface: slCreateEngine fails gracefully so the engine's
// audio init bails out non-fatally (proven in disassembly). The SL_IID_*
// objects are exported so libengine.so's NEEDED imports resolve.

typedef unsigned int SLresult;
typedef unsigned int SLuint32;
typedef int SLboolean;
typedef void *SLObjectItf;

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_FEATURE_UNSUPPORTED 12

// Mirrors SLEngineOption from OpenSLES.h
typedef struct {
    SLuint32 feature;
    SLuint32 data;
} SLEngineOption;

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions,
                        const SLEngineOption *pEngineOptions, SLuint32 numInterfaces,
                        const void *pInterfaceIds, const SLboolean *pInterfaceRequired);

// IID objects (data symbols the game imports)
extern const unsigned char SL_IID_ENGINE[16];
extern const unsigned char SL_IID_PLAY[16];
extern const unsigned char SL_IID_BUFFERQUEUE[16];
extern const unsigned char SL_IID_VOLUME[16];

#endif
