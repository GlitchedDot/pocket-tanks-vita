#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/audioout.h>
#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>
#include "bootlog.h"
#include "music.h"

/* stb_vorbis public API (implementation TU: stb_vorbis_impl.c).
 * Needed because her installed data pack (WinRT-derived) ships the
 * music as Ogg Vorbis while the engine requests the Android .m4a
 * names — the loader maps the extension and decodes in software. */
typedef struct stb_vorbis stb_vorbis;
typedef struct {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
} stb_vorbis_info;
extern stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len,
                                          int *error, const void *alloc_buffer);
extern void stb_vorbis_close(stb_vorbis *f);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_error(stb_vorbis *f);
extern int stb_vorbis_seek_start(stb_vorbis *f);
extern unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels,
                                                    short *buffer, int num_shorts);

#define MUS_FMT_NONE   0
#define MUS_FMT_AAC    1
#define MUS_FMT_VORBIS 2

/* ---- MP4/M4A demuxer over a whole-file memory image ---------------- */

static unsigned int rd32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

struct track {
    char path[256];
    unsigned char *img;      /* whole .m4a file, owned */
    long img_len;
    unsigned char asc[8];
    int asc_len;
    int sample_rate;
    int channels;
    int frames;
    unsigned int *foff;
    unsigned int *fsize;
    unsigned int max_frame;
    int format;              /* MUS_FMT_* */
    stb_vorbis *vorbis;      /* open handle when format == VORBIS */
    int loaded;
    volatile int playing;
    int volume;              /* raw modVolume value; -1 = never set */
    int frame_idx;           /* pump position */
};

static struct track g_trk;

typedef int (*box_cb)(const char *type, long off, long len, void *ud);
static void walk_boxes(long start, long end, box_cb cb, void *ud) {
    long pos = start;
    while (pos + 8 <= end) {
        unsigned int sz = rd32(g_trk.img + pos);
        if (sz == 1 || sz == 0) return; /* not expected in these files */
        if (sz < 8 || pos + sz > g_trk.img_len) return;
        char type[5];
        memcpy(type, g_trk.img + pos + 4, 4); type[4] = 0;
        if (cb(type, pos + 8, (long)sz - 8, ud)) return;
        pos += sz;
    }
}

struct parse_ctx {
    int have_asc, have_stsz, have_stco, have_stsc;
    unsigned int *sizes, n_sizes;
    unsigned int *chunk_off, n_chunks;
    struct { unsigned int first_chunk, per_chunk; } *stsc;
    unsigned int n_stsc;
    unsigned long dur_samples;
};

static int parse_box(const char *type, long off, long len, void *ud_) {
    struct parse_ctx *ud = (struct parse_ctx *)ud_;
    const unsigned char *base = g_trk.img + off;
    if (!strcmp(type, "moov") || !strcmp(type, "trak") ||
        !strcmp(type, "mdia") || !strcmp(type, "minf") ||
        !strcmp(type, "stbl")) {
        walk_boxes(off, off + len, parse_box, ud);
    } else if (!strcmp(type, "stsd") && len > 8) {
        const unsigned char *e = NULL;
        for (long i = 0; i + 4 <= len; i++)
            if (!memcmp(base + i, "esds", 4)) { e = base + i + 4; break; }
        if (e) {
            const unsigned char *p = e + 4;
            if (p[0] == 0x03) {
                p++;
                int b; unsigned int l = 0;
                do { b = *p++; l = (l << 7) | (b & 0x7f); } while (b & 0x80);
                p += 2;
                unsigned char flags = *p++;
                if (flags & 0x80) p += 2;
                if (flags & 0x40) { int sl = *p++; p += sl; }
                if (flags & 0x20) p += 2;
                if (p[0] == 0x04) {
                    p++;
                    l = 0;
                    do { b = *p++; l = (l << 7) | (b & 0x7f); } while (b & 0x80);
                    p += 13;
                    if (p[0] == 0x05) {
                        p++;
                        l = 0;
                        do { b = *p++; l = (l << 7) | (b & 0x7f); } while (b & 0x80);
                        if (l >= 2 && l <= sizeof(g_trk.asc)) {
                            memcpy(g_trk.asc, p, l);
                            g_trk.asc_len = (int)l;
                            static const int srt[13] = {96000,88200,64000,48000,
                                44100,32000,24000,22050,16000,12000,11025,8000,7350};
                            int sri = ((g_trk.asc[0] & 0x07) << 1) | (g_trk.asc[1] >> 7);
                            g_trk.sample_rate = (sri < 13) ? srt[sri] : 0;
                            g_trk.channels = (g_trk.asc[1] >> 3) & 0x0f;
                            ud->have_asc = 1;
                        }
                    }
                }
            }
        }
    } else if (!strcmp(type, "stsz") && len >= 12) {
        unsigned int fixed = rd32(base + 4);
        unsigned int n = rd32(base + 8);
        if (fixed != 0 || n == 0 || n > 200000) return 0;
        ud->sizes = malloc(n * 4);
        if (!ud->sizes) return 0;
        for (unsigned int i = 0; i < n; i++) ud->sizes[i] = rd32(base + 12 + i * 4);
        ud->n_sizes = n;
        ud->have_stsz = 1;
    } else if (!strcmp(type, "stco") && len >= 8) {
        unsigned int n = rd32(base + 4);
        if (n == 0 || n > 100000) return 0;
        ud->chunk_off = malloc(n * 4);
        if (!ud->chunk_off) return 0;
        for (unsigned int i = 0; i < n; i++) ud->chunk_off[i] = rd32(base + 8 + i * 4);
        ud->n_chunks = n;
        ud->have_stco = 1;
    } else if (!strcmp(type, "stsc") && len >= 8) {
        unsigned int n = rd32(base + 4);
        if (n == 0 || n > 100000) return 0;
        ud->stsc = malloc(n * sizeof(*ud->stsc));
        if (!ud->stsc) return 0;
        for (unsigned int i = 0; i < n; i++) {
            ud->stsc[i].first_chunk = rd32(base + 8 + i * 12);
            ud->stsc[i].per_chunk = rd32(base + 8 + i * 12 + 4);
        }
        ud->n_stsc = n;
        ud->have_stsc = 1;
    } else if (!strcmp(type, "stts") && len >= 8) {
        unsigned int n = rd32(base + 4);
        unsigned long tot = 0;
        for (unsigned int i = 0; i < n; i++)
            tot += (unsigned long)rd32(base + 8 + i * 8) * rd32(base + 8 + i * 8 + 4);
        ud->dur_samples = tot;
    }
    return 0;
}

static void track_clear(void) {
    if (g_trk.vorbis) stb_vorbis_close(g_trk.vorbis);
    free(g_trk.foff); free(g_trk.fsize); free(g_trk.img);
    memset(&g_trk, 0, sizeof(g_trk));
    g_trk.volume = -1;
}

/* ---- playback: sceAudiodec -> sceAudioOut BGM port ------------------ */

#define MUS_BLOCK_SAMPLES 4096   /* frames per output block */
#define MUS_PCM_BYTES (MUS_BLOCK_SAMPLES * 2 * 2 + 4096) /* stereo + slack */

static int g_lib_init = 0;
static SceAudiodecCtrl g_dec;
static SceAudiodecInfoAac g_info;
static int g_dec_ok = 0;
static int g_dec_rate = 0, g_dec_ch = 0;
static unsigned char *g_es = NULL;    /* one AAC frame, aligned */
static short *g_pcm = NULL;           /* block accumulator */
static int g_port = -1;
static int g_port_rate = 0;
static int g_port_ch = 0;
static SceUID g_thread = -1;
static int g_dec_fail_streak = 0;
static int g_loops = 0;

static void apply_volume(void) {
    if (g_port < 0) return;
    int v = g_trk.volume;
    int vol = (v < 0) ? 0x8000 : (v <= 100 ? (v * 0x8000) / 100
                                 : (v > 0x8000 ? 0x8000 : v));
    int vols[2] = { vol, vol };
    sceAudioOutSetVolume(g_port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);
}

static int ensure_port(int rate, int channels) {
    if (g_port >= 0 && g_port_rate == rate && g_port_ch == channels) return 1;
    if (g_port >= 0) { sceAudioOutReleasePort(g_port); g_port = -1; }
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                 MUS_BLOCK_SAMPLES, rate,
                                 channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO
                                               : SCE_AUDIO_OUT_MODE_MONO);
    if (g_port < 0) {
        blog("MUS: open BGM port @%d/%dch failed: 0x%x", rate, channels, g_port);
        return 0;
    }
    g_port_rate = rate;
    g_port_ch = channels;
    apply_volume();
    blog("MUS: BGM port open @%d, %d ch (id %d)", rate, channels, g_port);
    return 1;
}

static void ensure_buffers(void) {
    /* AR bug: these were allocated only inside ensure_decoder(),
     * which the Vorbis path skips — the pump decoded into NULL. */
    if (!g_es) g_es = memalign(64, SCE_AUDIODEC_AAC_MAX_ES_SIZE);
    if (!g_pcm) g_pcm = memalign(64, MUS_PCM_BYTES);
}

static int ensure_decoder(void) {
    if (g_dec_ok && g_dec_rate == g_trk.sample_rate &&
        g_dec_ch == g_trk.channels) {
        sceAudiodecClearContext(&g_dec);
        return 1;
    }
    if (g_dec_ok) { sceAudiodecDeleteDecoder(&g_dec); g_dec_ok = 0; }
    if (!g_lib_init) {
        int r1 = sceSysmoduleLoadModule(SCE_SYSMODULE_INTERNAL_AUDIOCODEC);
        SceAudiodecInitParam ip;
        memset(&ip, 0, sizeof(ip));
        ip.aac.size = sizeof(ip);
        ip.aac.totalStreams = 1;
        int r2 = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
        blog("MUS: sysmodule audiocodec=0x%x, InitLibrary AAC=0x%x", r1, r2);
        g_lib_init = 1;
    }
    memset(&g_info, 0, sizeof(g_info));
    g_info.size = sizeof(g_info);
    g_info.isAdts = 0;
    g_info.ch = g_trk.channels;
    g_info.samplingRate = g_trk.sample_rate;
    g_info.isSbr = 0;
    memset(&g_dec, 0, sizeof(g_dec));
    g_dec.size = sizeof(g_dec);
    g_dec.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    g_dec.pInfo = (SceAudiodecInfo *)&g_info;
    g_dec.maxEsSize = SCE_AUDIODEC_AAC_MAX_ES_SIZE;
    g_dec.maxPcmSize = 4096; /* one frame: <=2048 samples x 2 bytes */
    int r = sceAudiodecCreateDecoder(&g_dec, SCE_AUDIODEC_TYPE_AAC);
    blog("MUS: CreateDecoder(%d Hz, %d ch) = 0x%x", g_trk.sample_rate,
         g_trk.channels, r);
    if (r < 0) return 0;
    g_dec_ok = 1;
    g_dec_rate = g_trk.sample_rate;
    g_dec_ch = g_trk.channels;
    g_dec_fail_streak = 0;
    return 1;
}

static int pump_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int acc_samples = 0;
    int logged_decodes = 0;
    while (1) {
        if (!g_trk.playing || !g_trk.loaded ||
            (g_trk.format == MUS_FMT_AAC && !g_dec_ok) ||
            (g_trk.format == MUS_FMT_VORBIS && !g_trk.vorbis)) {
            acc_samples = 0;
            sceKernelDelayThread(15000);
            continue;
        }
        if (g_trk.format == MUS_FMT_VORBIS) {
            int ch = g_trk.channels;
            int want_frames = MUS_BLOCK_SAMPLES - acc_samples;
            int got = stb_vorbis_get_samples_short_interleaved(
                g_trk.vorbis, ch, g_pcm + acc_samples * ch,
                want_frames * ch);
            if (got == 0) {
                stb_vorbis_seek_start(g_trk.vorbis);
                if (g_loops < 2) blog("MUS: track looped (vorbis)");
                g_loops++;
                continue;
            }
            if (logged_decodes < 3) {
                blog("MUS: vorbis pull #%d -> %d frames", logged_decodes, got);
                logged_decodes++;
            }
            acc_samples += got;
            if (acc_samples >= MUS_BLOCK_SAMPLES) {
                sceAudioOutOutput(g_port, g_pcm);
                acc_samples = 0;
            }
            continue;
        }
        int idx = g_trk.frame_idx;
        int fsz = (int)g_trk.fsize[idx];
        if (fsz <= 0 || fsz > SCE_AUDIODEC_AAC_MAX_ES_SIZE) {
            g_trk.frame_idx = (idx + 1) % g_trk.frames;
            continue;
        }
        memcpy(g_es, g_trk.img + g_trk.foff[idx], fsz);
        g_dec.pEs = g_es;
        g_dec.inputEsSize = fsz;
        g_dec.maxEsSize = fsz;
        g_dec.pPcm = (unsigned char *)g_pcm + acc_samples * 2;
        g_dec.maxPcmSize = 4096;
        g_dec.outputPcmSize = 0;
        int r = sceAudiodecDecode(&g_dec);
        if (r < 0) {
            if (g_dec_fail_streak < 5)
                blog("MUS: decode frame %d failed: 0x%x (internal?)",
                     idx, r);
            if (++g_dec_fail_streak > 60) {
                blog("MUS: too many decode failures, stopping pump");
                g_trk.playing = 0;
            }
            g_trk.frame_idx = (idx + 1) % g_trk.frames;
            continue;
        }
        g_dec_fail_streak = 0;
        int produced = (int)g_dec.outputPcmSize / 2; /* mono samples */
        if (logged_decodes < 3) {
            blog("MUS: decode #%d frame %d -> %d samples", logged_decodes,
                 idx, produced);
            logged_decodes++;
        }
        acc_samples += produced;
        g_trk.frame_idx = idx + 1;
        if (g_trk.frame_idx >= g_trk.frames) {
            g_trk.frame_idx = 0;
            if (g_loops < 2) blog("MUS: track looped");
            g_loops++;
        }
        if (acc_samples >= MUS_BLOCK_SAMPLES) {
            sceAudioOutOutput(g_port, g_pcm);
            acc_samples = 0;
        }
    }
    return 0;
}

static void ensure_thread(void) {
    if (g_thread >= 0) return;
    g_thread = sceKernelCreateThread("music_pump", pump_thread,
                                     112, 0x40000, 0, 0, NULL);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL);
    blog("MUS: pump thread = 0x%x", g_thread);
}

/* ---- engine-facing API ---------------------------------------------- */

static void dump_dir(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) { blog("MUS: list %s -> err 0x%x", path, d); return; }
    SceIoDirent ent;
    int n = 0;
    blog("MUS: list %s:", path);
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0 && n < 48) {
        blog("MUS:   [%s]%s", ent.d_name,
             SCE_S_ISDIR(ent.d_stat.st_mode) ? "/" : "");
        memset(&ent, 0, sizeof(ent));
        n++;
    }
    sceIoDclose(d);
}

/* Try the four candidate bases for one asset name. On success the
 * winning path is copied into g_trk.path HERE (the candidate buffer
 * is local to this frame — returning a pointer to it was AO's bug).
 * sceIo, not stdio: the loader's proven file path. */
static SceUID try_open(const char *name) {
    char cand[4][256];
    snprintf(cand[0], sizeof(cand[0]), "ux0:data/pockettanks/assets/%s", name);
    snprintf(cand[1], sizeof(cand[1]), "ux0:data/pockettanks/%s", name);
    snprintf(cand[2], sizeof(cand[2]), "%s", name);
    snprintf(cand[3], sizeof(cand[3]), "app0:assets/%s", name);
    for (int i = 0; i < 4; i++) {
        SceUID fd = sceIoOpen(cand[i], SCE_O_RDONLY, 0);
        blog("MUS: open '%s' -> 0x%x", cand[i], fd);
        if (fd >= 0) {
            strncpy(g_trk.path, cand[i], sizeof(g_trk.path) - 1);
            g_trk.path[sizeof(g_trk.path) - 1] = 0;
            return fd;
        }
    }
    return -1;
}

static int ci_stem_eq(const char *entry, const char *stem, int stem_len) {
    for (int i = 0; i < stem_len; i++) {
        char a = entry[i], b = stem[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b || a == 0) return 0;
    }
    return entry[stem_len] == '.';
}

/* Last resort: find the track by scanning the music directory and
 * opening the entry's name VERBATIM as the filesystem reports it —
 * immune to any case/encoding mismatch in constructed paths. */
static SceUID try_open_by_listing(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    const char *dot = strrchr(base, '.');
    int stem_len = dot ? (int)(dot - base) : (int)strlen(base);
    char dir[2][256];
    if (slash) {
        snprintf(dir[0], sizeof(dir[0]), "ux0:data/pockettanks/assets/%.*s",
                 (int)(slash - name), name);
        snprintf(dir[1], sizeof(dir[1]), "ux0:data/pockettanks/%.*s",
                 (int)(slash - name), name);
    } else {
        snprintf(dir[0], sizeof(dir[0]), "ux0:data/pockettanks/assets/");
        snprintf(dir[1], sizeof(dir[1]), "ux0:data/pockettanks/");
    }
    for (int d = 0; d < 2; d++) {
        SceUID dh = sceIoDopen(dir[d]);
        if (dh < 0) continue;
        SceIoDirent ent;
        memset(&ent, 0, sizeof(ent));
        SceUID found = -1;
        while (sceIoDread(dh, &ent) > 0) {
            if (ci_stem_eq(ent.d_name, base, stem_len)) {
                char full[320];
                snprintf(full, sizeof(full), "%s/%s", dir[d], ent.d_name);
                found = sceIoOpen(full, SCE_O_RDONLY, 0);
                blog("MUS: listing-resolved '%s' -> open 0x%x", full, found);
                if (found >= 0) {
                    strncpy(g_trk.path, full, sizeof(g_trk.path) - 1);
                    g_trk.path[sizeof(g_trk.path) - 1] = 0;
                }
                break;
            }
            memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(dh);
        if (found >= 0) return found;
    }
    return -1;
}

int music_load(const char *name) {
    blog("SND: loadMod enter '%s'", name ? name : "(null)");
    g_trk.playing = 0;
    track_clear();
    if (!name) { blog("SND: loadMod name=NULL"); return 0; }

    SceUID fd = try_open(name);
    if (fd < 0) {
        /* The installed data pack may carry the same track under the
         * other container's extension (.m4a <-> .ogg). */
        const char *dot = strrchr(name, '.');
        if (dot && (!strcmp(dot, ".m4a") || !strcmp(dot, ".ogg"))) {
            char alt[128];
            /* three conversions, three args: precision, stem, new ext.
             * AO/AP/AQ passed only two (name was missing) — the final
             * %s dereferenced stack garbage: NOT FOUND in AP, the
             * loading-bar crash in AQ. */
            snprintf(alt, sizeof(alt), "%.*s%s", (int)(dot - name), name,
                     !strcmp(dot, ".m4a") ? ".ogg" : ".m4a");
            blog("MUS: alt name: '%s'", alt);
            fd = try_open(alt);
            if (fd >= 0)
                blog("SND: loadMod '%s' -> found as '%s'", name, alt);
        }
    }
    if (fd < 0) fd = try_open_by_listing(name);
    if (fd < 0) {
        blog("SND: loadMod '%s' -> NOT FOUND (any name, any base)", name);
        dump_dir("ux0:data/pockettanks/assets/");
        dump_dir("ux0:data/pockettanks/assets/music/");
        return 0;
    }
    const char *used = g_trk.path;
    SceOff fsize_off = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    long fsize = (long)fsize_off;
    g_trk.img = malloc(fsize);
    int rd = g_trk.img ? sceIoRead(fd, g_trk.img, fsize) : -1;
    sceIoClose(fd);
    if (!g_trk.img || rd != fsize) {
        blog("SND: loadMod '%s' -> read failed (%d/%ld)", name, rd, fsize);
        track_clear();
        return 0;
    }
    g_trk.img_len = fsize;
    blog("SND: loadMod '%s' -> '%s' (%ld bytes)", name, used, fsize);

    /* Ogg Vorbis track: no MP4 demux, stb_vorbis streams from the
     * image directly. */
    if (fsize > 4 && !memcmp(g_trk.img, "OggS", 4)) {
        int err = 0;
        g_trk.vorbis = stb_vorbis_open_memory(g_trk.img, (int)fsize,
                                              &err, NULL);
        if (!g_trk.vorbis) {
            blog("SND: loadMod vorbis open failed (err %d)", err);
            track_clear();
            return 0;
        }
        stb_vorbis_info vi = stb_vorbis_get_info(g_trk.vorbis);
        g_trk.format = MUS_FMT_VORBIS;
        g_trk.sample_rate = (int)vi.sample_rate;
        g_trk.channels = vi.channels;
        g_trk.loaded = 1;
        g_trk.frame_idx = 0;
        blog("SND: loadMod OK (Vorbis): %d Hz, %d ch, dur=%.2fs",
             g_trk.sample_rate, g_trk.channels,
             vi.sample_rate ? (double)stb_vorbis_stream_length_in_samples(
                                  g_trk.vorbis) / vi.sample_rate : 0.0);
        return 1;
    }
    g_trk.format = MUS_FMT_AAC;

    struct parse_ctx ud;
    memset(&ud, 0, sizeof(ud));
    walk_boxes(0, fsize, parse_box, &ud);

    if (!ud.have_asc || !ud.have_stsz || !ud.have_stco || !ud.have_stsc) {
        blog("SND: loadMod parse INCOMPLETE asc=%d stsz=%d stco=%d stsc=%d",
             ud.have_asc, ud.have_stsz, ud.have_stco, ud.have_stsc);
        free(ud.sizes); free(ud.chunk_off); free(ud.stsc);
        track_clear();
        return 0;
    }

    g_trk.frames = (int)ud.n_sizes;
    g_trk.foff = malloc(ud.n_sizes * 4);
    g_trk.fsize = malloc(ud.n_sizes * 4);
    if (!g_trk.foff || !g_trk.fsize) {
        free(ud.sizes); free(ud.chunk_off); free(ud.stsc);
        track_clear();
        return 0;
    }
    unsigned int sidx = 0, sci = 0, maxf = 0;
    for (unsigned int c = 1; c <= ud.n_chunks; c++) {
        while (sci + 1 < ud.n_stsc && ud.stsc[sci + 1].first_chunk <= c) sci++;
        unsigned int per = ud.stsc[sci].per_chunk;
        unsigned int off = ud.chunk_off[c - 1];
        for (unsigned int k = 0; k < per && sidx < ud.n_sizes; k++) {
            g_trk.foff[sidx] = off;
            g_trk.fsize[sidx] = ud.sizes[sidx];
            if (ud.sizes[sidx] > maxf) maxf = ud.sizes[sidx];
            off += ud.sizes[sidx];
            sidx++;
        }
    }
    g_trk.max_frame = maxf;
    g_trk.loaded = 1;
    g_trk.frame_idx = 0;
    blog("SND: loadMod OK: %d Hz, %d ch, %d frames, max frame %u B, dur=%.2fs",
         g_trk.sample_rate, g_trk.channels, g_trk.frames, maxf,
         g_trk.sample_rate ? (double)ud.dur_samples / g_trk.sample_rate : 0.0);
    free(ud.sizes); free(ud.chunk_off); free(ud.stsc);
    return 1;
}

void music_play(void) {
    blog("SND: playMod track='%s' loaded=%d", g_trk.path, g_trk.loaded);
    if (!g_trk.loaded) return;
    if (!ensure_port(g_trk.sample_rate, g_trk.channels)) return;
    ensure_buffers();
    if (g_trk.format == MUS_FMT_AAC && !ensure_decoder()) return;
    ensure_thread();
    g_trk.playing = 1;
}

void music_stop(void) {
    blog("SND: stopMod track='%s'", g_trk.path);
    g_trk.playing = 0;
}

void music_free(void) {
    blog("SND: freeMod track='%s'", g_trk.path);
    g_trk.playing = 0;
    /* keep the parsed track: the engine re-loads before replaying;
     * frame table + image are dropped on the next loadMod anyway. */
}

void music_volume(int v) {
    if (v != g_trk.volume) {
        blog("SND: modVolume(%d)", v);
        g_trk.volume = v;
        apply_volume();
    }
}

int music_frame_count(void) { return g_trk.frames; }

long music_frame_offset(int idx, int *size_out) {
    if (!g_trk.loaded || idx < 0 || idx >= g_trk.frames) return -1;
    if (size_out) *size_out = (int)g_trk.fsize[idx];
    return (long)g_trk.foff[idx];
}
