#include <stdint.h>
#include <string.h>
#include "bootlog.h"
#include "fmod_trace.h"

/* GOT slot VAs in libengine (R_ARM_JUMP_SLOT relocations). */
#define GOT_SETSOFTWARE_FORMAT 0x703fc0
#define GOT_SYSTEM_INIT        0x703fc4
#define GOT_SYSTEM_CREATE      0x703fc8
#define GOT_CREATE_CHANGROUP   0x703fcc
#define GOT_CREATE_SOUND       0x703fd0
#define GOT_PLAY_SOUND         0x703fec

typedef int (*create_fn)(void **out, unsigned int ver);
typedef int (*init_fn)(void *self, int maxch, unsigned int flags, void *extra);
typedef int (*swfmt_fn)(void *self, int rate, int speakermode, int raw);
typedef int (*cgroup_fn)(void *self, const char *name, void **out);
typedef int (*csound_fn)(void *self, const char *name, unsigned int mode,
                         void *exinfo, void **out);
typedef int (*play_fn)(void *self, void *sound, void *group, int paused,
                       void **chan);

static uintptr_t g_fmod_base;

static create_fn real_create;
static init_fn real_init;
static swfmt_fn real_swfmt;
static cgroup_fn real_cgroup;
static csound_fn real_csound;
static play_fn real_play;

static int wrap_create(void **out, unsigned int ver) {
    int r = real_create(out, ver);
    blog("FMOD: System_Create(ver=0x%08x) -> %d, system=%p",
         ver, r, out ? *out : (void *)0);
    if (g_fmod_base) {
        /* Build AX: FMOD's cached CPU-feature state in libfmod .bss:
         * flag bytes at +0x12ecc8/+0x12eccc, 64-bit mask at +0x12ece0.
         * The error-41 gate (fn 0x9eaac) tests mask bit 4 (0x10). */
        const volatile unsigned char *fb =
            (const volatile unsigned char *)g_fmod_base;
        unsigned int mlo = *(const volatile unsigned int *)(fb + 0x12ece0);
        unsigned int mhi = *(const volatile unsigned int *)(fb + 0x12ece4);
        blog("FMOD: cpu mask=0x%08x:%08x flags=%u,%u",
             mhi, mlo, fb[0x12ecc8], fb[0x12eccc]);
    }
    if (r == 0 && out && *out && g_fmod_base) {
        /* Build AZ: enumerate FMOD's plugin registry on the fresh
         * system (which outputs/codecs/DSPs actually registered?),
         * then force the OpenSL output by handle — bypassing the
         * AUTODETECT selection that fails with error 54. libfmod
         * VAs (thumb): GetNumPlugins 0x1ad69, GetPluginHandle
         * 0x1ad73, GetPluginInfo 0x1ad7d, SetOutputByPlugin 0x1ad87. */
        typedef int (*nplug_fn)(void *, int, int *);
        typedef int (*handle_fn)(void *, int, int, unsigned int *);
        typedef int (*info_fn)(void *, unsigned int, int *, char *,
                               unsigned int, unsigned int *);
        typedef int (*setout_fn)(void *, unsigned int);
        nplug_fn nplug = (nplug_fn)(g_fmod_base + 0x1ad69);
        handle_fn phandle = (handle_fn)(g_fmod_base + 0x1ad73);
        info_fn pinfo = (info_fn)(g_fmod_base + 0x1ad7d);
        setout_fn setout = (setout_fn)(g_fmod_base + 0x1ad87);
        unsigned int opensl_handle = 0, first_out_handle = 0;
        for (int type = 0; type <= 2; type++) {
            int n = -1;
            int rn = nplug(*out, type, &n);
            blog("FMODPLUGIN: type=%d GetNumPlugins -> %d (n=%d)",
                 type, rn, n);
            if (rn != 0 || n <= 0 || n > 64)
                continue;
            for (int i = 0; i < n; i++) {
                unsigned int h = 0;
                if (phandle(*out, type, i, &h) != 0)
                    continue;
                char nm[64];
                int t2 = -1;
                unsigned int ver = 0;
                nm[0] = '\0';
                int ri = pinfo(*out, h, &t2, nm, sizeof(nm), &ver);
                nm[sizeof(nm) - 1] = '\0';
                blog("FMODPLUGIN: type=%d idx=%d handle=0x%x name='%s' "
                     "ver=0x%x (info->%d)", type, i, h, nm, ver, ri);
                if (type == 0) {
                    if (!first_out_handle)
                        first_out_handle = h;
                    if (strstr(nm, "OpenSL") || strstr(nm, "openSL") ||
                        strstr(nm, "OPENSL"))
                        opensl_handle = h;
                }
            }
        }
        unsigned int use = opensl_handle ? opensl_handle : first_out_handle;
        if (use) {
            int rs = setout(*out, use);
            blog("FMOD: SetOutputByPlugin(0x%x) -> %d", use, rs);
        } else {
            blog("FMOD: no output plugin registered at all");
        }
    }
    return r;
}

static int wrap_swfmt(void *self, int rate, int speakermode, int raw) {
    int r = real_swfmt(self, rate, speakermode, raw);
    blog("FMOD: setSoftwareFormat(rate=%d, speakermode=%d, raw=%d) -> %d",
         rate, speakermode, raw, r);
    return r;
}

static int wrap_init(void *self, int maxch, unsigned int flags, void *extra) {
    int r = real_init(self, maxch, flags, extra);
    blog("FMOD: System::init(maxch=%d, flags=0x%x, extra=%p) -> %d",
         maxch, flags, extra, r);
    if (r != 0 && g_fmod_base) {
        /* Build AY: interrogate the half-initialized system — is an
         * output registered at all? (libfmod VAs, thumb.) */
        typedef int (*sys1_fn)(void *sys, int *out);
        sys1_fn get_num_drivers = (sys1_fn)(g_fmod_base + 0x1aca1);
        sys1_fn get_output = (sys1_fn)(g_fmod_base + 0x1ac97);
        int ndrv = -12345, otype = -12345;
        int r1 = get_num_drivers(self, &ndrv);
        int r2 = get_output(self, &otype);
        blog("FMOD: post-init probe: GetNumDrivers -> %d (n=%d), "
             "GetOutput -> %d (type=%d)", r1, ndrv, r2, otype);
    }
    return r;
}

int pt_fmod_debug_cb(unsigned int flags, const char *file, int line,
                     const char *func, const char *message) {
    (void)flags; (void)file; (void)line; (void)func;
    blog("FMODDBG: %s", message ? message : "");
    return 0;
}

static int wrap_cgroup(void *self, const char *name, void **out) {
    int r = real_cgroup(self, name, out);
    blog("FMOD: createChannelGroup('%s') -> %d, group=%p",
         name ? name : "(null)", r, out ? *out : (void *)0);
    return r;
}

static int wrap_csound(void *self, const char *name, unsigned int mode,
                       void *exinfo, void **out) {
    int r = real_csound(self, name, mode, exinfo, out);
    /* name may be a path or a raw data pointer (FMOD_OPENMEMORY);
     * print it as a string only when it looks like one. */
    char printable = 0;
    if (name) {
        printable = 1;
        for (int i = 0; i < 24 && name[i]; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c < 0x20 || c > 0x7e) { printable = 0; break; }
        }
    }
    if (printable)
        blog("FMOD: createSound('%s', mode=0x%x) -> %d, sound=%p",
             name, mode, r, out ? *out : (void *)0);
    else
        blog("FMOD: createSound(ptr=%p, mode=0x%x) -> %d, sound=%p",
             name, mode, r, out ? *out : (void *)0);
    return r;
}

static int wrap_play(void *self, void *sound, void *group, int paused,
                     void **chan) {
    int r = real_play(self, sound, group, paused, chan);
    blog("FMOD: playSound(sound=%p, group=%p, paused=%d) -> %d, chan=%p",
         sound, group, paused, r, chan ? *chan : (void *)0);
    return r;
}

static void patch(uintptr_t base, uint32_t va, void *wrap, void **real,
                  const char *label) {
    volatile uint32_t *slot = (volatile uint32_t *)(base + va);
    uint32_t old = *slot;
    *real = (void *)old;
    *slot = (uint32_t)(uintptr_t)wrap;
    blog("FMODTRACE: %s GOT 0x%x: 0x%08x -> wrapper", label, va, old);
}

void fmod_trace_install(uintptr_t engine_base, uintptr_t fmod_base) {
    g_fmod_base = fmod_base;
    patch(engine_base, GOT_SYSTEM_CREATE, (void *)wrap_create,
          (void **)&real_create, "System_Create");
    patch(engine_base, GOT_SETSOFTWARE_FORMAT, (void *)wrap_swfmt,
          (void **)&real_swfmt, "setSoftwareFormat");
    patch(engine_base, GOT_SYSTEM_INIT, (void *)wrap_init,
          (void **)&real_init, "System::init");
    patch(engine_base, GOT_CREATE_CHANGROUP, (void *)wrap_cgroup,
          (void **)&real_cgroup, "createChannelGroup");
    patch(engine_base, GOT_CREATE_SOUND, (void *)wrap_csound,
          (void **)&real_csound, "createSound");
    patch(engine_base, GOT_PLAY_SOUND, (void *)wrap_play,
          (void **)&real_play, "playSound");
}
