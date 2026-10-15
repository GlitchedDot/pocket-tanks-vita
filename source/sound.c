#include "sound.h"
#include "utils.h"
#include "bootlog.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Build AH: a real OpenSL ES output device for FMOD, backed by       */
/* sceAudioOut.                                                       */
/*                                                                    */
/* History: through Build AG this file was a deliberate stub —        */
/* slCreateEngine failed and dlsym reported every OpenSL symbol       */
/* missing, so FMOD initialized with no output device and mixed       */
/* into the void ("no audio in v1"). AH implements the surface FMOD   */
/* actually uses: engine -> output mix -> buffer-queue audio player,  */
/* with a pump thread feeding the Vita's main audio port. FMOD        */
/* mixes; we carry PCM to the hardware.                               */
/*                                                                    */
/* IID matching is by POINTER: libfmod.so imports the SL_IID_* data   */
/* symbols from the loader's dynlib table, so every interface id it   */
/* passes back to us is one of the four arrays defined below. An      */
/* unrecognized id is logged (first 16 bytes) and refused as an       */
/* unsupported feature, which FMOD tolerates for optional interfaces. */
/* ------------------------------------------------------------------ */

const unsigned char SL_IID_ENGINE[16] = {0};
const unsigned char SL_IID_PLAY[16] = {0};
const unsigned char SL_IID_BUFFERQUEUE[16] = {0};
const unsigned char SL_IID_VOLUME[16] = {0};
/* Build BB: FMOD's OpenSL output asks dlsym for THIS one — the
 * Android simple buffer queue — not the generic BUFFERQUEUE. */
const unsigned char SL_IID_ANDROIDSIMPLEBUFFERQUEUE[16] = {0};
/* Build BC: the rest of FMOD's dlsym list, extracted from
 * libfmod.so's string table: ANDROIDCONFIGURATION + RECORD. */
const unsigned char SL_IID_ANDROIDCONFIGURATION[16] = {0};
const unsigned char SL_IID_RECORD[16] = {0};

typedef int SLint32;
typedef unsigned short SLuint16;
typedef int SLmillisecond;
typedef int SLmillibel;

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3
#define SL_TIME_UNKNOWN      0xFFFFFFFFu
#define SL_OBJECT_STATE_REALIZED 2

/* An interface handle FMOD holds is the address of a word whose
 * value is the interface's vtable; every interface struct here also
 * carries its owner so a method can find the object state. */
typedef struct SlObj SlObj;
typedef struct {
    const void *vtbl;
    SlObj *owner;
} SlIface;

#define SL_QCAP 32

struct SlObj {
    const void *objVtbl; /* first word: the SLObjectItf handle is &obj */
    int kind;            /* 1 = engine, 2 = output mix, 3 = player */
    SlIface if_engine, if_mix, if_play, if_bq, if_vol, if_abq, if_acfg;

    /* player state */
    int rate, channels, bits;
    int port;            /* sceAudioOut port, -1 = not open */
    int portLen;         /* frames per hardware buffer */
    unsigned char *stage;/* staging buffer for size-mismatched chunks */
    int playState;
    int nullIfaceCount;   /* Build BK: FMOD's iid arrives as NULL for
                           * every interface; dispatch by call order */
    void *bqCb, *bqCtx;  /* buffer-queue callback + context */
    void *bqIface;       /* the iface the callback registered through */
    void *playCb, *playCtx;
    SLuint32 playCbMask;
    SLint32 volLevel;
    int muted;
    void *qBuf[SL_QCAP];
    SLuint32 qSize[SL_QCAP];
    int qHead, qTail, qCount;
    SceUID mutex;
    SceUID thread;
    unsigned long framesOut;
    unsigned long buffersOut;
    int loggedVolume;
};

static SlObj g_engine, g_mix, g_player;
static int g_player_created = 0;

/* ---------------- generic stubs ---------------- */

static SLresult sl_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

/* ---------------- object interface ---------------- */

static SLresult obj_realize(void *self, SLboolean async) {
    SlObj *o = (SlObj *)self;
    blog("SL: Realize kind=%d async=%d", o ? o->kind : -1, (int)async);
    return SL_RESULT_SUCCESS;
}
static SLresult obj_resume(void *self, SLboolean async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_get_state(void *self, SLuint32 *state) {
    SlObj *o = (SlObj *)self;
    blog("SL: GetState kind=%d", o ? o->kind : -1);
    if (state) *state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static void sl_log_unknown_iid(const void *iid) {
    const unsigned char *b = (const unsigned char *)iid;
    blog("SL: GetInterface unknown IID %02x%02x%02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x",
         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
         b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static SLresult obj_get_interface(void *self, const void *iid, void *pInterface);
static SLresult obj_destroy(void *self) {
    SlObj *o = (SlObj *)self;
    blog("SL: Destroy kind=%d", o ? o->kind : -1);
    if (o && o->kind == 3)
        o->playState = SL_PLAYSTATE_STOPPED;
    return SL_RESULT_SUCCESS;
}
/* Build BF: the full SLObjectItf has THIRTEEN entries; ours had
 * eight, so any FMOD call to AddInterface/RegisterCallback/
 * AbortAsyncOperation (slots 4/7/8) landed on the wrong function or
 * past the table entirely — the boot crash right after
 * slCreateEngine. Fill the real layout with safe stubs. */
static SLresult obj_add_interface(void *self, const void *iid, int dynamic) {
    (void)self; (void)dynamic;
    blog("SL: AddInterface called");
    if (iid) sl_log_unknown_iid(iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult obj_remove_interface(void *self, const void *iid) {
    (void)self; (void)iid;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_resume_interface(void *self, const void *iid, int async) {
    (void)self; (void)iid; (void)async;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_register_cb(void *self, void *cb, void *ctx) {
    (void)self; (void)cb; (void)ctx;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_abort_async(void *self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_set_priority(void *self, SLint32 prio, SLboolean preempt) {
    (void)self; (void)prio; (void)preempt;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_get_priority(void *self, SLint32 *prio, SLboolean *preempt) {
    (void)self;
    if (prio) *prio = 0;
    if (preempt) *preempt = 1;
    return SL_RESULT_SUCCESS;
}
static SLresult obj_set_loci(void *self, SLuint32 n, void *ids, SLboolean enabled) {
    (void)self; (void)n; (void)ids; (void)enabled;
    return SL_RESULT_SUCCESS;
}

typedef struct {
    void *Realize, *Resume, *GetState, *GetInterface, *AddInterface,
         *RemoveInterface, *ResumeInterface, *RegisterCallback,
         *AbortAsyncOperation, *Destroy, *SetPriority, *GetPriority,
         *SetLossOfControlInterfaces;
} SlObjVtbl;

static const SlObjVtbl g_obj_vtbl = {
    (void *)obj_realize, (void *)obj_resume, (void *)obj_get_state,
    (void *)obj_get_interface, (void *)obj_add_interface,
    (void *)obj_remove_interface, (void *)obj_resume_interface,
    (void *)obj_register_cb, (void *)obj_abort_async,
    (void *)obj_destroy, (void *)obj_set_priority,
    (void *)obj_get_priority, (void *)obj_set_loci
};

/* ---------------- player internals ---------------- */
/* Build BO: an out-pointer is only written when it lands in this
 * process's writable RAM. The BN dump faulted on a store through
 * 0x982a0e5d (inside libfmod's text) that passed a >=0x10000 test. */
#define SL_WRITABLE(p) ((uintptr_t)(p) >= 0x81000000u && \
                        (uintptr_t)(p) < 0x83000000u)


static void sl_lock(SlObj *o)   { if (o->mutex >= 0) sceKernelLockMutex(o->mutex, 1, NULL); }
static void sl_unlock(SlObj *o) { if (o->mutex >= 0) sceKernelUnlockMutex(o->mutex, 1); }

static int sl_open_port(SlObj *o, int frames) {
    if (o->port >= 0)
        return 1;
    if (frames <= 0 || o->rate <= 0 || o->channels <= 0)
        return 0;
    /* sceAudioOut lengths must be a multiple of 64 samples and the
     * API takes the frame count per buffer for the main port. */
    int len = (frames + 63) & ~63;
    SceAudioOutMode mode = (o->channels == 2) ? SCE_AUDIO_OUT_MODE_STEREO
                                              : SCE_AUDIO_OUT_MODE_MONO;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, len,
                                   o->rate, mode);
    if (port < 0) {
        blog("SL: sceAudioOutOpenPort(len=%d rate=%d ch=%d) -> 0x%x",
             len, o->rate, o->channels, port);
        return 0;
    }
    int vol[2] = { 0x8000, 0x8000 }; /* SCE_AUDIO_VOLUME_1 both channels */
    sceAudioOutSetVolume(port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         vol);
    o->port = port;
    o->portLen = len;
    o->stage = (unsigned char *)malloc((size_t)len * (size_t)o->channels * 2u);
    blog("SL: audio port open: id=%d len=%d rate=%d ch=%d (FMOD buf=%d frames)",
         port, len, o->rate, o->channels, frames);
    return 1;
}

static void sl_output_frames(SlObj *o, const unsigned char *data, int frames) {
    int frameBytes = o->channels * 2; /* 16-bit samples throughout */
    if (frames == o->portLen && data) {
        sceAudioOutOutput(o->port, data);
        o->framesOut += (unsigned long)frames;
        return;
    }
    /* Size mismatch: move through the staging buffer in portLen
     * chunks, zero-filling a short tail. */
    int done = 0;
    while (done < frames || done == 0) {
        int chunk = frames - done;
        if (chunk > o->portLen) chunk = o->portLen;
        if (chunk < 0) chunk = 0;
        memset(o->stage, 0, (size_t)o->portLen * (size_t)frameBytes);
        if (data && chunk > 0)
            memcpy(o->stage, data + (size_t)done * (size_t)frameBytes,
                   (size_t)chunk * (size_t)frameBytes);
        sceAudioOutOutput(o->port, o->stage);
        o->framesOut += (unsigned long)o->portLen;
        done += chunk;
        if (frames == 0)
            break;
    }
}

static int sl_pump_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SlObj *o = &g_player;
    blog("SL: pump thread running");
    for (;;) {
        void *buf = NULL;
        SLuint32 size = 0;
        sl_lock(o);
        if (o->playState == SL_PLAYSTATE_PLAYING && o->qCount > 0) {
            buf = o->qBuf[o->qHead];
            size = o->qSize[o->qHead];
            o->qHead = (o->qHead + 1) % SL_QCAP;
            o->qCount--;
        }
        sl_unlock(o);
        if (!buf) {
            sceKernelDelayThread(3000);
            continue;
        }
        int frameBytes = o->channels * 2;
        int frames = frameBytes > 0 ? (int)(size / (SLuint32)frameBytes) : 0;
        if (o->port >= 0)
            sl_output_frames(o, (const unsigned char *)buf, frames);
        o->buffersOut++;
        if (o->buffersOut == 1)
            blog("SL: first buffer played (%u bytes)", size);
        /* Return the buffer to FMOD: its callback refills and
         * re-enqueues from here, on this thread. The Android simple
         * buffer queue callback takes (caller, context, eventFlags);
         * the extra arg is harmless if a client expects only two. */
        if (o->bqCb) {
            typedef void (*BqCb)(void *, void *, unsigned int);
            void *iface = o->bqIface ? o->bqIface : (void *)&o->if_bq;
            ((BqCb)o->bqCb)(iface, o->bqCtx, 0);
        }
    }
    return 0;
}

static void sl_ensure_pump(SlObj *o) {
    if (o->thread >= 0)
        return;
    o->mutex = sceKernelCreateMutex("pt_sl_bq", 0, 1, NULL);
    o->thread = sceKernelCreateThread("pt_sl_pump", sl_pump_thread, 96,
                                      0x10000, 0, 0, NULL);
    if (o->thread >= 0)
        sceKernelStartThread(o->thread, 0, NULL);
    else
        blog("SL: pump thread create failed: 0x%x", o->thread);
}

/* ---------------- buffer queue interface ---------------- */

static SLresult bq_enqueue(void *self, const void *buffer, SLuint32 size) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    /* Build BL: raw-arg tracing on the queue boundary. BK's run had
     * a first call whose "size" was a memory address — FMOD's call
     * shape does not match the assumed slot layout yet. */
    blog("SL: Enqueue buf=%p size=%u", buffer, size);
    if (!o || !buffer || size == 0 || size > 4194304u)
        return SL_RESULT_SUCCESS;
    /* The first buffer fixes the hardware buffer length. */
    if (o->port < 0) {
        int frameBytes = o->channels * (o->bits / 8);
        int frames = frameBytes > 0 ? (int)(size / (SLuint32)frameBytes) : 0;
        sl_open_port(o, frames);
        sl_ensure_pump(o);
    }
    sl_lock(o);
    if (o->qCount < SL_QCAP) {
        o->qBuf[o->qTail] = (void *)buffer;
        o->qSize[o->qTail] = size;
        o->qTail = (o->qTail + 1) % SL_QCAP;
        o->qCount++;
    }
    sl_unlock(o);
    return SL_RESULT_SUCCESS;
}
static SLresult bq_clear(void *self) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    blog("SL: BqClear");
    if (o) {
        sl_lock(o);
        o->qHead = o->qTail = o->qCount = 0;
        sl_unlock(o);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_get_state(void *self, void *state) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    blog("SL: BqGetState state=%p count=%d", state, o ? o->qCount : -1);
    if (state && SL_WRITABLE(state) && o) {
        SLuint32 *s = (SLuint32 *)state; /* {count, playIndex} */
        s[0] = (SLuint32)o->qCount;
        s[1] = (SLuint32)(o->buffersOut & 3u);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult bq_register_callback(void *self, void *cb, void *ctx) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) { o->bqCb = cb; o->bqCtx = ctx; o->bqIface = self; }
    blog("SL: buffer-queue callback registered cb=%p ctx=%p", cb, ctx);
    return SL_RESULT_SUCCESS;
}

/* Build BM: FMOD calls the queue's RegisterCallback at slot 0 with
 * (callback-into-libfmod, context) — its queue vtable layout is not
 * the textbook order. All four queue slots point at this dispatcher,
 * which classifies each call by argument shape:
 *   RegisterCallback: arg2 inside libfmod's image (0x98200000..0x98400000)
 *   Enqueue:          arg3 is a plausible byte count (16..1 MB)
 * Anything else is logged and no-opped rather than guessed at. */
static SLresult abq_dispatch(void *self, void *a2, void *a3) {
    uintptr_t p2 = (uintptr_t)a2, p3 = (uintptr_t)a3;
    if (p2 >= 0x98200000u && p2 < 0x98400000u)
        return bq_register_callback(self, a2, a3);
    if (a2 && p3 >= 16u && p3 <= 1048576u)
        return bq_enqueue(self, a2, (SLuint32)p3);
    blog("SL: ABQ dispatch unclassified a2=%p a3=%p (no-op)", a2, a3);
    return SL_RESULT_SUCCESS;
}

typedef struct { void *Enqueue, *Clear, *GetState, *RegisterCallback; } SlBqVtbl;
static const SlBqVtbl g_bq_vtbl = {
    (void *)bq_enqueue, (void *)bq_clear, (void *)bq_get_state,
    (void *)bq_register_callback
};
/* The Android simple buffer queue has the same 4-method shape and
 * state layout ({count, index}); it shares the implementations.
 * Build BM: FMOD's slot order differs — all slots go through the
 * argument-shape dispatcher (see abq_dispatch). */
static const SlBqVtbl g_abq_vtbl = {
    (void *)abq_dispatch, (void *)abq_dispatch, (void *)abq_dispatch,
    (void *)abq_dispatch
};

/* ---------------- play interface ---------------- */

static SLresult play_set_state(void *self, SLuint32 state) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) {
        blog("SL: SetPlayState %u", state);
        o->playState = (int)state;
        if (state == SL_PLAYSTATE_PLAYING)
            sl_ensure_pump(o);
    }
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_state(void *self, SLuint32 *state) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    /* Build BN: the BM crash dump faulted HERE with self == NULL and
     * a junk state pointer (0xac44) — FMOD calls this with arguments
     * that are not a real interface/output pair. Log it, never write
     * through an implausible pointer. */
    blog("SL: GetPlayState self=%p state=%p", self, (void *)state);
    if (state && SL_WRITABLE(state))
        *state = o ? (SLuint32)o->playState : SL_PLAYSTATE_STOPPED;
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_duration(void *self, SLmillisecond *dur) {
    (void)self;
    if (dur && SL_WRITABLE(dur))
        *dur = (SLmillisecond)SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_position(void *self, SLmillisecond *pos) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    blog("SL: GetPosition self=%p pos=%p", self, (void *)pos);
    if (pos && SL_WRITABLE(pos)) {
        *pos = (o && o->rate > 0)
                   ? (SLmillisecond)((o->framesOut * 1000ul) / (unsigned long)o->rate)
                   : 0;
    }
    return SL_RESULT_SUCCESS;
}
static SLresult play_register_callback(void *self, void *cb, void *ctx) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) { o->playCb = cb; o->playCtx = ctx; }
    return SL_RESULT_SUCCESS;
}
static SLresult play_set_callback_mask(void *self, SLuint32 mask) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) o->playCbMask = mask;
    return SL_RESULT_SUCCESS;
}
static SLresult play_generic_u32(void *self, SLuint32 v) {
    (void)self; (void)v;
    return SL_RESULT_SUCCESS;
}
static SLresult play_get_u32(void *self, SLuint32 *v) {
    (void)self;
    if (v) *v = 0;
    return SL_RESULT_SUCCESS;
}

typedef struct {
    void *SetPlayState, *GetPlayState, *GetDuration, *GetPosition,
         *RegisterCallback, *SetCallbackMask, *SetMarkerPosition,
         *ClearMarkerPosition, *GetMarkerPosition,
         *SetPositionUpdatePeriod, *GetPositionUpdatePeriod;
} SlPlayVtbl;
static const SlPlayVtbl g_play_vtbl = {
    (void *)play_set_state, (void *)play_get_state,
    (void *)play_get_duration, (void *)play_get_position,
    (void *)play_register_callback, (void *)play_set_callback_mask,
    (void *)play_generic_u32, (void *)play_generic_u32,
    (void *)play_get_u32, (void *)play_generic_u32, (void *)play_get_u32
};

/* ---------------- volume interface ---------------- */

static SLresult vol_set_level(void *self, SLmillibel level) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) {
        o->volLevel = level;
        if (!o->loggedVolume) {
            o->loggedVolume = 1;
            blog("SL: SetVolumeLevel %d (port stays at max for v1)", level);
        }
    }
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_level(void *self, SLmillibel *level) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (level && SL_WRITABLE(level)) *level = o ? o->volLevel : 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_set_mute(void *self, SLboolean mute) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (o) o->muted = mute;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_get_mute(void *self, SLboolean *mute) {
    SlObj *o = self ? ((SlIface *)self)->owner : &g_player;
    if (mute) *mute = o ? o->muted : 0;
    return SL_RESULT_SUCCESS;
}
static SLresult vol_generic(void *self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}

typedef struct {
    void *SetVolumeLevel, *GetVolumeLevel, *SetMute, *GetMute,
         *EnableStereoPosition, *IsStereoPositionEnabled,
         *SetStereoPosition, *GetStereoPosition;
} SlVolVtbl;
static const SlVolVtbl g_vol_vtbl = {
    (void *)vol_set_level, (void *)vol_get_level, (void *)vol_set_mute,
    (void *)vol_get_mute, (void *)vol_generic, (void *)vol_generic,
    (void *)vol_generic, (void *)vol_generic
};

/* ------------- android configuration interface ------------- */

static SLresult acfg_set(void *self, const char *key, const void *value,
                         SLuint32 valueSize) {
    (void)self;
    blog("SL: AndroidConfiguration Set '%s' size=%u val=%d",
         key ? key : "(null)", valueSize,
         (value && valueSize >= 4) ? *(const int *)value : -1);
    return SL_RESULT_SUCCESS;
}
static SLresult acfg_get(void *self, const char *key, SLuint32 *valueSize,
                         void *value) {
    (void)self; (void)key;
    if (value && valueSize && *valueSize)
        memset(value, 0, *valueSize);
    return SL_RESULT_SUCCESS;
}

typedef struct { void *SetConfiguration, *GetConfiguration; } SlAcfgVtbl;
static const SlAcfgVtbl g_acfg_vtbl = {
    (void *)acfg_set, (void *)acfg_get
};

/* ---------------- output mix interface ---------------- */

typedef struct { void *GetDestinationOutputDeviceIDs, *ReRoute; } SlMixVtbl;
static const SlMixVtbl g_mix_vtbl = {
    (void *)sl_unsupported, (void *)sl_unsupported
};

/* ---------------- engine interface ---------------- */

typedef struct {
    SLuint32 locatorType;
    SLuint32 numBuffers;
} SlLocatorBq;
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec, bitsPerSample,
             containerSize, channelMask, endian;
} SlFormatPcm;
typedef struct { void *pLocator, *pFormat; } SlDataSrcSnk;

static SLresult eng_create_output_mix(void *self, void *pMix,
                                      SLuint32 numInterfaces,
                                      const void *ids, const SLboolean *req) {
    (void)self; (void)numInterfaces; (void)ids; (void)req;
    if (pMix) *(void **)pMix = (void *)&g_mix;
    blog("SL: CreateOutputMix");
    return SL_RESULT_SUCCESS;
}

static SLresult eng_create_audio_player(void *self, void *pPlayer,
                                        void *pSrc, void *pSnk,
                                        SLuint32 numInterfaces,
                                        const void *ids,
                                        const SLboolean *req) {
    (void)self; (void)pSnk; (void)ids; (void)req;
    SlObj *o = &g_player;
    memset(o, 0, sizeof(*o));
    o->objVtbl = (const void *)&g_obj_vtbl;
    o->kind = 3;
    o->port = -1;
    o->thread = -1;
    o->mutex = -1;
    o->playState = SL_PLAYSTATE_STOPPED;
    o->if_play.vtbl = (const void *)&g_play_vtbl; o->if_play.owner = o;
    o->if_bq.vtbl = (const void *)&g_bq_vtbl;     o->if_bq.owner = o;
    o->if_abq.vtbl = (const void *)&g_abq_vtbl;   o->if_abq.owner = o;
    o->if_acfg.vtbl = (const void *)&g_acfg_vtbl; o->if_acfg.owner = o;
    o->if_vol.vtbl = (const void *)&g_vol_vtbl;   o->if_vol.owner = o;

    SlDataSrcSnk *src = (SlDataSrcSnk *)pSrc;
    int numBuffers = 0;
    if (src && src->pLocator) {
        SlLocatorBq *loc = (SlLocatorBq *)src->pLocator;
        blog("SL: src locator type=%u numBuffers=%u", loc->locatorType,
             loc->numBuffers);
        numBuffers = (int)loc->numBuffers;
    }
    if (src && src->pFormat) {
        SlFormatPcm *f = (SlFormatPcm *)src->pFormat;
        o->channels = (int)f->numChannels;
        o->rate = (int)(f->samplesPerSec / 1000u); /* OpenSL uses mHz */
        o->bits = (int)f->bitsPerSample;
        blog("SL: CreateAudioPlayer fmt type=%u ch=%d rate=%d bits=%d "
             "container=%u mask=0x%x endian=%u numIf=%u bufs=%d",
             f->formatType, o->channels, o->rate, o->bits,
             f->containerSize, f->channelMask, f->endian,
             numInterfaces, numBuffers);
    }
    if (pPlayer) *(void **)pPlayer = (void *)o;
    g_player_created = 1;
    return SL_RESULT_SUCCESS;
}

typedef struct {
    void *CreateLEDDevice, *CreateVibraDevice, *CreateAudioPlayer,
         *CreateAudioRecorder, *CreateMidiPlayer, *CreateListener,
         *Create3DGroup, *CreateOutputMix, *CreateMetadataExtractor,
         *CreateExtensionObject, *QueryNumSupportedInterfaces,
         *QuerySupportedInterfaces, *QueryNumSupportedExtensions,
         *QuerySupportedExtension, *IsExtensionSupported;
} SlEngVtbl;
static const SlEngVtbl g_eng_vtbl = {
    (void *)sl_unsupported, (void *)sl_unsupported,
    (void *)eng_create_audio_player,
    (void *)sl_unsupported, (void *)sl_unsupported, (void *)sl_unsupported,
    (void *)sl_unsupported, (void *)eng_create_output_mix,
    (void *)sl_unsupported, (void *)sl_unsupported, (void *)sl_unsupported,
    (void *)sl_unsupported, (void *)sl_unsupported, (void *)sl_unsupported,
    (void *)sl_unsupported
};

/* ---------------- GetInterface (all objects) ---------------- */

static SLresult obj_get_interface(void *self, const void *iid, void *pInterface) {
    SlObj *o = (SlObj *)self;
    blog("SL: GetInterface kind=%d iid=%p", o ? o->kind : -1, iid);
    if (!o || !pInterface)
        return SL_RESULT_FEATURE_UNSUPPORTED;
    *(void **)pInterface = NULL;
    if (iid == (const void *)SL_IID_ENGINE && o->kind == 1) {
        *(void **)pInterface = (void *)&o->if_engine;
        blog("SL: GetInterface -> ENGINE");
        return SL_RESULT_SUCCESS;
    }
    if (o->kind == 3) {
        if (iid == (const void *)SL_IID_PLAY) {
            *(void **)pInterface = (void *)&o->if_play;
            return SL_RESULT_SUCCESS;
        }
        if (iid == (const void *)SL_IID_BUFFERQUEUE) {
            *(void **)pInterface = (void *)&o->if_bq;
            return SL_RESULT_SUCCESS;
        }
        if (iid == (const void *)SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
            *(void **)pInterface = (void *)&o->if_abq;
            blog("SL: GetInterface -> AndroidSimpleBufferQueue");
            return SL_RESULT_SUCCESS;
        }
        if (iid == (const void *)SL_IID_ANDROIDCONFIGURATION) {
            *(void **)pInterface = (void *)&o->if_acfg;
            blog("SL: GetInterface -> AndroidConfiguration");
            return SL_RESULT_SUCCESS;
        }
        if (iid == (const void *)SL_IID_VOLUME) {
            *(void **)pInterface = (void *)&o->if_vol;
            return SL_RESULT_SUCCESS;
        }
    }
    /* Build BI: FMOD calls GetInterface(engine, NULL) right after
     * Realize. Refusing it (BH) makes FMOD abort init with 28;
     * answering with the object handle (BE) crashed FMOD, because
     * it immediately calls CreateOutputMix through the returned
     * pointer at ENGINE-interface index 7 — i.e. this call IS
     * FMOD reaching for the engine interface, with the ID arriving
     * as 0 in this environment. Give it the engine interface. */
    if (iid == NULL) {
        /* Build BJ: FMOD asks EVERY object for its primary interface
         * with a NULL id (engine -> ENGINE in BI worked; the player
         * is asked next). Answer per object kind. */
        if (o->kind == 1) {
            *(void **)pInterface = (void *)&o->if_engine;
            blog("SL: GetInterface NULL iid on engine -> ENGINE");
            return SL_RESULT_SUCCESS;
        }
        if (o->kind == 2) {
            *(void **)pInterface = (void *)&o->if_mix;
            blog("SL: GetInterface NULL iid on mix -> OUTPUTMIX");
            return SL_RESULT_SUCCESS;
        }
        if (o->kind == 3) {
            /* Build BK: BJ's log showed FMOD's first slot-0 call on
             * the returned interface passing a BUFFER POINTER (into
             * FMOD's own image) — an Enqueue, not a SetPlayState.
             * FMOD asks the player for the buffer queue FIRST.
             * First NULL request -> ABQ, later ones -> PLAY. */
            o->nullIfaceCount++;
            if (o->nullIfaceCount == 1) {
                *(void **)pInterface = (void *)&o->if_abq;
                blog("SL: GetInterface NULL iid on player #1 -> ABQ");
                return SL_RESULT_SUCCESS;
            }
            *(void **)pInterface = (void *)&o->if_play;
            blog("SL: GetInterface NULL iid on player #%d -> PLAY",
                 o->nullIfaceCount);
            return SL_RESULT_SUCCESS;
        }
        blog("SL: GetInterface NULL iid -> FEATURE_UNSUPPORTED");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    sl_log_unknown_iid(iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* ---------------- entry point ---------------- */

static void sl_init_obj(SlObj *o, int kind) {
    memset(o, 0, sizeof(*o));
    o->objVtbl = (const void *)&g_obj_vtbl;
    o->kind = kind;
    o->port = -1;
    o->thread = -1;
    o->mutex = -1;
    o->if_engine.vtbl = (const void *)&g_eng_vtbl; o->if_engine.owner = o;
    o->if_mix.vtbl = (const void *)&g_mix_vtbl;    o->if_mix.owner = o;
}

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions,
                        const SLEngineOption *pEngineOptions, SLuint32 numInterfaces,
                        const void *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    blog("SL: slCreateEngine enter numOptions=%u numInterfaces=%u",
         numOptions, numInterfaces);
    (void)pEngineOptions; (void)pInterfaceIds; (void)pInterfaceRequired;
    sl_init_obj(&g_engine, 1);
    sl_init_obj(&g_mix, 2);
    if (pEngine)
        *pEngine = (SLObjectItf)&g_engine;
    blog("SL: slCreateEngine -> real OpenSL device (Build AH)");
    return SL_RESULT_SUCCESS;
}
