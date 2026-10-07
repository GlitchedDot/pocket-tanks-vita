// Bionic (Android libc) compatibility shims for symbols the game imports
// that don't exist in the VitaSDK.
#include "utils.h"
#include "bootlog.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

// ---------------------------------------------------------------------------
// Generic stub return values
// ---------------------------------------------------------------------------
int ret0(void) { return 0; }
int ret1(void) { return 1; }
int ret_neg1(void) { return -1; }
void ret_void(void) {}

// Forward: game-heap extent tracking for the bug-17 throw hook's scan.
static void heap_track(void *p, size_t n);

// ---------------------------------------------------------------------------
// Stack protector (game was built with -fstack-protector)
// ---------------------------------------------------------------------------
uintptr_t __stack_chk_guard = 0x5a5a5a5a;

void __stack_chk_fail_soloader(void) {
    log_error("__stack_chk_fail: stack smashing detected!");
    blog("EXIT: __stack_chk_fail - stack smashing detected");
    bootlog_close();
    sceKernelExitProcess(1);
}

// ---------------------------------------------------------------------------
// __sF: Bionic's stdin/stdout/stderr FILE array (data symbol)
// ---------------------------------------------------------------------------
FILE __sF[3];

void bionic_sF_init(void) {
    __sF[0] = *stdin;
    __sF[1] = *stdout;
    __sF[2] = *stderr;
}

// ---------------------------------------------------------------------------
// __errno: Bionic errno accessor
// ---------------------------------------------------------------------------
int *__errno_soloader(void) {
    static int e = 0;
    return &e;
}

// ---------------------------------------------------------------------------
// _FORTIFY _chk wrappers (drop the extra size argument)
// ---------------------------------------------------------------------------
void *__memcpy_chk(void *dest, const void *src, size_t n, size_t destlen) {
    (void)destlen;
    return sceClibMemcpy(dest, src, n);
}

void *__memset_chk(void *s, int c, size_t n, size_t destlen) {
    (void)destlen;
    return sceClibMemset(s, c, n);
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcpy(dest, src);
}

char *__strncpy_chk(char *dest, const char *src, size_t n, size_t destlen) {
    (void)destlen;
    return strncpy(dest, src, n);
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcat(dest, src);
}

char *__strchr_chk(const char *s, int c, size_t destlen) {
    (void)destlen;
    return (char *)strchr(s, c);
}

size_t __strlen_chk(const char *s, size_t destlen) {
    (void)destlen;
    return strlen(s);
}

char *__strncpy_chk2(char *dest, const char *src, size_t n, size_t d1, size_t d2) {
    (void)d1; (void)d2;
    return strncpy(dest, src, n);
}

int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap) {
    (void)flag; (void)slen;
    return vsnprintf(s, n, fmt, ap);
}

int __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap) {
    (void)flag; (void)slen;
    return vsprintf(s, fmt, ap);
}

size_t __read_chk(int fd, void *buf, size_t n, size_t buflen) {
    (void)buflen;
    return read(fd, buf, n);
}

// Forward declaration (defined below with the other IO wrappers).
static void normalize_path_soloader(const char *in, char *out, size_t outsz);

int __open_2(const char *path, int flags) {
    // NDK _FORTIFY_SOURCE redirects the game's open() calls here. Route
    // through the same path normalization + tracing as open_soloader.
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    if (path && strcmp(path, norm) != 0)
        blog("IO: __open_2 path normalized: \"%s\" -> \"%s\"", path, norm);
    errno = 0;
    int fd = open(norm, flags);
    int e = errno;
    blog("IO: __open_2(\"%s\", 0x%x) -> %d errno=%d", path ? path : "(null)",
         flags, fd, fd >= 0 ? 0 : e);
    return fd;
}

// ---------------------------------------------------------------------------
// pthread_create: Bionic attr layout differs; use sane defaults
// ---------------------------------------------------------------------------
int pthread_create_soloader(pthread_t *thread, const void *attr,
                            void *(*start)(void *), void *param) {
    (void)attr;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 512 * 1024);
    int ret = pthread_create(thread, &a, start, param);
    pthread_attr_destroy(&a);
    return ret;
}

// ---------------------------------------------------------------------------
// dlsym: FMOD lazy-loads AAudio/OpenSL symbols; report them missing so it
// falls back gracefully.
// ---------------------------------------------------------------------------
void *dlsym_soloader(void *handle, const char *symbol) {
    (void)handle;
    log_info("dlsym(%s) -> NULL (stubbed)", symbol ? symbol : "(null)");
    return NULL;
}

// ---------------------------------------------------------------------------
// mmap64/munmap/mremap: malloc-backed (game is unlikely to need real mmap;
// FMOD doesn't mmap)
// ---------------------------------------------------------------------------
void *mmap64_soloader(void *addr, size_t len, int prot, int flags, int fd, long long offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    log_warn("mmap64(%u) -> malloc fallback", (unsigned)len);
    return malloc(len ? len : 1);
}

// ---------------------------------------------------------------------------
// __assert2: Bionic assert
// ---------------------------------------------------------------------------
void __assert2_soloader(const char *file, int line, const char *func, const char *expr) {
    log_error("ASSERT FAILED: %s:%d (%s): %s", file, line, func, expr);
    blog("EXIT: bionic assert failed: %s:%d (%s): %s", file, line, func, expr);
    bootlog_close();
    sceKernelExitProcess(1);
}

// ---------------------------------------------------------------------------
// Misc wrappers
// ---------------------------------------------------------------------------
void *__memclr_wrapper(void *s, size_t n) {
    return sceClibMemset(s, 0, n);
}

// ---------------------------------------------------------------------------
// Bug 18: __aeabi_memset argument order (THE 0x0C heap scribbler).
// ARM EABI: void __aeabi_memset(void *dest, size_t n, int c) — the 2nd and
// 3rd args are REVERSED vs ANSI memset(dest, c, n). The loader previously
// mapped __aeabi_memset directly to sceClibMemset (ANSI order), swapping n
// and c. A game call __aeabi_memset(buf, 12, 32) ("fill 12 bytes with 0x20")
// therefore executed as memset(buf, 12, 32) = 32 bytes of 0x0C, overflowing
// the 12-byte destination and smashing the adjacent dlmalloc chunk header
// (fd/bk = 0x0c0c0c0c), which crashed later in the smallbin unlink during
// free()/malloc(). This is why the memset_soloader 0x0C trap never fired:
// the game never calls ANSI memset with 0x0C — it uses __aeabi_memset.
// ---------------------------------------------------------------------------
void *__aeabi_memset_wrapper(void *dest, size_t n, int c) {
    static int log_count = 0;
    if ((c & 0xff) == 0x0c && log_count < 8) {
        void *caller = __builtin_return_address(0);
        blog("HEAP: __aeabi_memset(dst=0x%x, n=%u, c=0x0c) caller=0x%x",
             (unsigned)(uintptr_t)dest, (unsigned)n,
             (unsigned)(uintptr_t)caller);
        log_count++;
    }
    return memset(dest, c, n);
}

void *memalign_soloader(size_t alignment, size_t size) {
    // posix_memalign isn't in Vita newlib; align manually.
    // NOTE: free() on the result leaks; acceptable for game use.
    void *raw = malloc(size + alignment + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~(alignment - 1);
    ((void **)aligned)[-1] = raw;
    heap_track(raw, size + alignment + sizeof(void *));
    return (void *)aligned;
}

void exit_soloader(int status) {
    log_info("exit(%d) called by game", status);
    blog("EXIT: game called bionic exit(%d) - silent exit to LiveArea", status);
    bootlog_close();
    sceKernelExitProcess(status);
}

void abort_soloader(void) {
    log_error("abort() called by game");
    blog("EXIT: game called abort()");
    bootlog_close();
    sceKernelExitProcess(1);
}

void *memmem_soloader(const void *haystack, size_t haystacklen,
                      const void *needle, size_t needlelen) {
    if (needlelen == 0)
        return (void *)haystack;
    if (haystacklen < needlelen)
        return NULL;
    const unsigned char *h = haystack, *n = needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

void sincos_soloader(double x, double *s, double *c) {
    *s = sin(x);
    *c = cos(x);
}

void sincosf_soloader(float x, float *s, float *c) {
    *s = sinf(x);
    *c = cosf(x);
}

int vasprintf_soloader(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0)
        return -1;
    *strp = malloc(len + 1);
    if (!*strp)
        return -1;
    return vsnprintf(*strp, len + 1, fmt, ap);
}

// ---------------------------------------------------------------------------
// C++ runtime shims provided by loader's libgcc/libstdc++ (declared extern,
// resolved at link time)
// ---------------------------------------------------------------------------
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dsub;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_fadd;
extern void *__aeabi_fdiv;
extern void *__aeabi_fmul;
extern void *__aeabi_fsub;
extern void *__aeabi_i2d;
extern void *__aeabi_l2d;
extern void *__aeabi_ui2d;
extern void *__aeabi_ul2d;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__dynamic_cast;
extern void *__gxx_personality_v0;
extern void *__gnu_Unwind_Find_exidx;

// ---------------------------------------------------------------------------
// Instrumented stdio wrappers (diagnostic): log every open/fopen/stat, and
// trace the engine.cfg stream's fread/fseek/ftell/fclose specifically.
// Also normalizes duplicate slashes ("//" -> "/") — Vita's io layer is
// picky about them and the game builds paths like
// "ux0:data/pockettanks//engine.cfg".
// ---------------------------------------------------------------------------
static void normalize_path_soloader(const char *in, char *out, size_t outsz) {
    // Relative paths (no drive ':' and not '/'-rooted): the game assumes the
    // process CWD is its files dir (true on Android, not on Vita). Resolve
    // them against the data dir before anything else.
    // (Bug 13: the game's preferences filename "blitengine.cfg" is relative;
    // without this it would resolve against the Vita process CWD.)
    char tmp[512];
    if (in && in[0] && !strchr(in, ':') && in[0] != '/') {
        snprintf(tmp, sizeof(tmp), "ux0:data/pockettanks/%s", in);
        in = tmp;
    }
    size_t j = 0;
    int last_slash = 0;
    if (!in)
        in = "";
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        int is_slash = (in[i] == '/');
        if (is_slash && last_slash)
            continue;
        out[j++] = in[i];
        last_slash = is_slash;
    }
    out[j] = '\0';
}

#define TRACKED_MAX 8
static FILE *tracked_streams[TRACKED_MAX];
static int tracked_count = 0;

static void track_stream_soloader(FILE *f, const char *path) {
    if (f && path && (strstr(path, "engine.cfg") || strstr(path, "blitengine.cfg")) && tracked_count < TRACKED_MAX)
        tracked_streams[tracked_count++] = f;
}

static int is_tracked_soloader(FILE *f) {
    for (int i = 0; i < tracked_count; i++)
        if (tracked_streams[i] == f)
            return 1;
    return 0;
}

FILE *fopen_soloader(const char *path, const char *mode) {
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    if (path && strcmp(path, norm) != 0)
        blog("IO: fopen path normalized: \"%s\" -> \"%s\"", path, norm);
    errno = 0;
    FILE *f = fopen(norm, mode);
    int e = errno;
    blog("IO: fopen(\"%s\", \"%s\") -> %p errno=%d", path ? path : "(null)",
         mode ? mode : "(null)", (void *)f, f ? 0 : e);
    track_stream_soloader(f, path);
    return f;
}

int open_soloader(const char *path, int flags, ...) {
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    if (path && strcmp(path, norm) != 0)
        blog("IO: open path normalized: \"%s\" -> \"%s\"", path, norm);
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    errno = 0;
    int fd = open(norm, flags, mode);
    int e = errno;
    blog("IO: open(\"%s\", 0x%x) -> %d errno=%d", path ? path : "(null)",
         flags, fd, fd >= 0 ? 0 : e);
    return fd;
}

int stat_soloader(const char *path, struct stat *st) {
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    errno = 0;
    int r = stat(norm, st);
    int e = errno;
    blog("IO: stat(\"%s\") -> %d errno=%d", path ? path : "(null)", r, r == 0 ? 0 : e);
    return r;
}

int access_soloader(const char *path, int mode) {
    // The game's config loader checks file existence with access() using the
    // double-slashed path ("ux0:data/pockettanks//engine.cfg"); raw access()
    // fails on that, so normalize first like the other IO wrappers.
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    if (path && strcmp(path, norm) != 0)
        blog("IO: access path normalized: \"%s\" -> \"%s\"", path, norm);
    errno = 0;
    int r = access(norm, mode);
    int e = errno;
    blog("IO: access(\"%s\", %d) -> %d errno=%d", path ? path : "(null)", mode,
         r, r == 0 ? 0 : e);
    return r;
}

// ---------------------------------------------------------------------------
// time/gettimeofday: implement via the RTC so the game sees a real, advancing
// clock. (The game's log timestamps were observed frozen across runs.)
// sceRtcGetCurrentTick returns microseconds since 0001-01-01 00:00:00 UTC, so
// the 62135596800-second offset to the 1970 epoch must be subtracted in 64-bit
// BEFORE narrowing to time_t (narrowing the raw ~6.4e10 value first yields a
// garbage 1954 date).
// ---------------------------------------------------------------------------
time_t time_soloader(time_t *t) {
    SceRtcTick tick = { 0 };
    sceRtcGetCurrentTick(&tick);
    time_t s = (time_t)(tick.tick / 1000000ULL - 62135596800ULL);
    if (t)
        *t = s;
    return s;
}

int gettimeofday_soloader(struct timeval *tv, void *tz) {
    (void)tz;
    SceRtcTick tick = { 0 };
    int r = sceRtcGetCurrentTick(&tick);
    if (tv) {
        tv->tv_sec = (time_t)(tick.tick / 1000000ULL - 62135596800ULL);
        tv->tv_usec = (suseconds_t)(tick.tick % 1000000ULL);
    }
    return r;
}

// ---------------------------------------------------------------------------
// gmtime_r/localtime_r/mktime: newlib's localtime_r/gmtime_r return NULL on
// Vita (no usable TZ data), and the game passes the result straight into
// asctime() -> null deref at DFAR=0x10. Implement them directly.
// Civil-date conversion uses Howard Hinnant's public-domain algorithm.
// ---------------------------------------------------------------------------
static int64_t days_from_civil_soloader(int y, unsigned m, unsigned d) {
    y -= (int)(m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days_soloader(int64_t z, int *yp, int *mp, int *dp) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mpv = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mpv + 2) / 5 + 1;
    unsigned m = mpv + (mpv < 10 ? 3 : -9);
    y += (m <= 2);
    *yp = y; *mp = (int)m; *dp = (int)d;
}

struct tm *gmtime_r_soloader(const time_t *t, struct tm *tm) {
    if (!t || !tm)
        return NULL;
    int64_t secs = (int64_t)*t;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int y, m, d;
    civil_from_days_soloader(days, &y, &m, &d);
    int wday = (int)((days + 4) % 7); // 1970-01-01 was a Thursday
    if (wday < 0)
        wday += 7;
    tm->tm_sec = (int)(rem % 60);
    tm->tm_min = (int)((rem / 60) % 60);
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_mday = d;
    tm->tm_mon = m - 1;
    tm->tm_year = y - 1900;
    tm->tm_wday = wday;
    tm->tm_yday = (int)(days - days_from_civil_soloader(y, 1, 1));
    tm->tm_isdst = 0;
    return tm;
}

struct tm *localtime_r_soloader(const time_t *t, struct tm *tm) {
    if (!t || !tm)
        return NULL;
    // Derive the UTC->local offset from the RTC (handles the user's timezone
    // and DST as configured on the Vita).
    SceRtcTick utc_tick = { 0 }, local_tick = { 0 };
    int64_t offset = 0;
    if (sceRtcGetCurrentTick(&utc_tick) == 0 &&
        sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick) == 0) {
        offset = ((int64_t)local_tick.tick - (int64_t)utc_tick.tick) / 1000000;
    }
    time_t local_t = *t + (time_t)offset;
    return gmtime_r_soloader(&local_t, tm);
}

static struct tm gmtime_buf_soloader;
static struct tm localtime_buf_soloader;

struct tm *gmtime_soloader(const time_t *t) {
    return gmtime_r_soloader(t, &gmtime_buf_soloader);
}

struct tm *localtime_soloader(const time_t *t) {
    return localtime_r_soloader(t, &localtime_buf_soloader);
}

time_t mktime_soloader(struct tm *tm) {
    if (!tm)
        return (time_t)-1;
    int64_t days = days_from_civil_soloader(tm->tm_year + 1900,
                                            (unsigned)tm->tm_mon + 1,
                                            (unsigned)tm->tm_mday);
    int64_t secs = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 +
                   tm->tm_sec;
    // mktime interprets tm as local time; convert using the RTC offset.
    SceRtcTick utc_tick = { 0 }, local_tick = { 0 };
    int64_t offset = 0;
    if (sceRtcGetCurrentTick(&utc_tick) == 0 &&
        sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick) == 0) {
        offset = ((int64_t)local_tick.tick - (int64_t)utc_tick.tick) / 1000000;
    }
    return (time_t)(secs - offset);
}

// NULL-safe asctime/ctime wrappers using our own broken-down time.
// (newlib's asctime dereferences tm unconditionally; the game was passing
// NULL from the broken localtime_r.)
static const char wday_name_soloader[7][4] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char mon_name_soloader[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

char *asctime_r_soloader(const struct tm *tm, char *buf) {
    if (!tm || !buf)
        return NULL;
    int wday = tm->tm_wday < 0 || tm->tm_wday > 6 ? 0 : tm->tm_wday;
    int mon = tm->tm_mon < 0 || tm->tm_mon > 11 ? 0 : tm->tm_mon;
    snprintf(buf, 64, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             wday_name_soloader[wday], mon_name_soloader[mon],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

static char asctime_buf_soloader[64];

char *asctime_soloader(const struct tm *tm) {
    return asctime_r_soloader(tm, asctime_buf_soloader);
}

char *ctime_r_soloader(const time_t *t, char *buf) {
    struct tm tmv;
    if (!localtime_r_soloader(t, &tmv))
        return NULL;
    return asctime_r_soloader(&tmv, buf);
}

static char ctime_buf_soloader[64];

char *ctime_soloader(const time_t *t) {
    return ctime_r_soloader(t, ctime_buf_soloader);
}

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t r = fread(ptr, size, nmemb, stream);
    if (is_tracked_soloader(stream))
        blog("IO: fread(engine.cfg stream, %u x %u) -> %u", (unsigned)size,
             (unsigned)nmemb, (unsigned)r);
    return r;
}

int fseek_soloader(FILE *stream, long offset, int whence) {
    int r = fseek(stream, offset, whence);
    if (is_tracked_soloader(stream))
        blog("IO: fseek(engine.cfg stream, %ld, %d) -> %d", offset, whence, r);
    return r;
}

long ftell_soloader(FILE *stream) {
    long r = ftell(stream);
    if (is_tracked_soloader(stream))
        blog("IO: ftell(engine.cfg stream) -> %ld", r);
    return r;
}

int fclose_soloader(FILE *stream) {
    int was = is_tracked_soloader(stream);
    int r = fclose(stream);
    if (was) {
        blog("IO: fclose(engine.cfg stream) -> %d", r);
        for (int i = 0; i < tracked_count; i++) {
            if (tracked_streams[i] == stream) {
                tracked_streams[i] = NULL;
                break;
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Bug 19: fwrite/freopen wrappers. fwrite was mapped raw; a short write is
// now logged (the game's "cp_fwrite failed" left no trace of which stream
// failed). freopen was also raw -- it gets the same path normalization as
// fopen, since the game builds double-slashed paths.
// ---------------------------------------------------------------------------
size_t fwrite_soloader(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream) {
    size_t r = fwrite(ptr, size, nmemb, stream);
    if (r != nmemb) {
        blog("IO: fwrite(%p, %u x %u) -> %u (SHORT) errno=%d",
             (void *)stream, (unsigned)size, (unsigned)nmemb, (unsigned)r,
             errno);
    } else if (is_tracked_soloader(stream)) {
        blog("IO: fwrite(engine.cfg stream, %u x %u) -> %u",
             (unsigned)size, (unsigned)nmemb, (unsigned)r);
    }
    return r;
}

FILE *freopen_soloader(const char *path, const char *mode, FILE *stream) {
    char norm[512];
    normalize_path_soloader(path, norm, sizeof(norm));
    if (path && strcmp(path, norm) != 0)
        blog("IO: freopen path normalized: \"%s\" -> \"%s\"", path, norm);
    errno = 0;
    FILE *f = freopen(norm, mode, stream);
    int e = errno;
    blog("IO: freopen(\"%s\", \"%s\", %p) -> %p errno=%d",
         path ? path : "(null)", mode ? mode : "(null)", (void *)stream,
         (void *)f, f ? 0 : e);
    return f;
}

// ---------------------------------------------------------------------------
// Bug 21 (mystery 1): write-path tracing. The game's engine.cfg save goes
// through AAssetManager_open (never fopen), and no fwrite() call appears in
// the bootlog -- so it must write via fputc/fputs/fprintf/fflush/write or
// via fdopen+fileno, all of which were mapped raw. These wrappers log the
// handle/fd (first few calls) and flag handles belonging to AAssetManager
// assets so we can see the game's actual write path.
// ---------------------------------------------------------------------------
#define WRITELOG_MAX 8
static int writelog_count = 0;

static int is_asset_file_soloader(FILE *f);  // defined below (asset registry)

static void writelog(const char *what, FILE *f) {
    if (writelog_count >= WRITELOG_MAX)
        return;
    writelog_count++;
    blog("IO: %s(stream=%p%s)", what, (void *)f,
         is_asset_file_soloader(f) ? " [ASSET]" : "");
}

int fputc_soloader(int c, FILE *stream) {
    writelog("fputc", stream);
    return fputc(c, stream);
}

int fputs_soloader(const char *s, FILE *stream) {
    writelog("fputs", stream);
    return fputs(s, stream);
}

int vfprintf_soloader(FILE *stream, const char *format, va_list ap) {
    // Log format pointer first (separate call) so we get it even if format is bad.
    // Then log the format string with a cap; vfprintf will read it anyway.
    blog("IO: vfprintf(stream=%p, fmt=%p)", stream, format);
    if (format) blog("IO: vfprintf fmt=\"%.128s\"", format);
    va_list aq;
    va_copy(aq, ap);
    int r = vfprintf(stream, format, aq);
    va_end(aq);
    return r;
}

int fprintf_soloader(FILE *stream, const char *format, ...) {
    blog("IO: fprintf(stream=%p, fmt=%p)", stream, format);
    if (format) blog("IO: fprintf fmt=\"%.128s\"", format);
    va_list ap;
    va_start(ap, format);
    int r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

// zlib wrappers with logging: the game may write configs via gzopen/gzwrite,
// which would otherwise be invisible in the bootlog.
gzFile gzopen_soloader(const char *path, const char *mode) {
    blog("IO: gzopen(\"%s\", \"%s\")", path ? path : "(null)", mode ? mode : "(null)");
    gzFile g = gzopen(path, mode);
    blog("IO: gzopen -> %p", g);
    return g;
}

int gzwrite_soloader(gzFile file, const void *buf, unsigned len) {
    blog("IO: gzwrite(%p, %u bytes)", file, len);
    int r = gzwrite(file, buf, len);
    blog("IO: gzwrite -> %d", r);
    return r;
}

int gzclose_soloader(gzFile file) {
    blog("IO: gzclose(%p)", file);
    int r = gzclose(file);
    blog("IO: gzclose -> %d", r);
    return r;
}

int fflush_soloader(FILE *stream) {    writelog("fflush", stream);
    return fflush(stream);
}

ssize_t write_soloader(int fd, const void *buf, size_t nbyte) {
    if (writelog_count < WRITELOG_MAX) {
        writelog_count++;
        blog("IO: write(fd=%d, n=%u)", fd, (unsigned)nbyte);
    }
    return write(fd, buf, nbyte);
}

FILE *fdopen_soloader(int fd, const char *mode) {
    FILE *f = fdopen(fd, mode);
    if (writelog_count < WRITELOG_MAX) {
        writelog_count++;
        blog("IO: fdopen(fd=%d, \"%s\") -> %p", fd, mode ? mode : "(null)",
             (void *)f);
    }
    return f;
}

int fileno_soloader(FILE *stream) {
    int r = fileno(stream);
    if (writelog_count < WRITELOG_MAX) {
        writelog_count++;
        blog("IO: fileno(%p%s) -> %d", (void *)stream,
             is_asset_file_soloader(stream) ? " [ASSET]" : "", r);
    }
    return r;
}

// Registry of FILE* handles owned by AAssetManager assets, so the write
// wrappers can flag when the game writes through an asset handle.
#define ASSET_FILES_MAX 32
static FILE *asset_files[ASSET_FILES_MAX];
static int asset_files_count = 0;

void track_asset_file_soloader(FILE *f) {
    if (f && asset_files_count < ASSET_FILES_MAX)
        asset_files[asset_files_count++] = f;
}

static int is_asset_file_soloader(FILE *f) {
    for (int i = 0; i < asset_files_count; i++)
        if (asset_files[i] == f)
            return 1;
    return 0;
}

void untrack_asset_file_soloader(FILE *f) {
    for (int i = 0; i < asset_files_count; i++) {
        if (asset_files[i] == f) {
            asset_files[i] = NULL;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Bug 15: thread-safe malloc + heap-corruption tripwire.
// newlib's default __malloc_lock/__malloc_unlock are empty stubs (single-
// threaded assumption). The game spawns threads (vitaGL GC, display queue,
// and its own workers) that allocate concurrently -> heap corruption.
// Implement them with a recursive kernel mutex. The lock path must not call
// malloc/blog (re-entrancy), so it uses raw kernel syscalls only.
// Also: free() validates the dlmalloc chunk header before freeing; the
// 2026-10-06 coredump showed a chunk header overwritten with 0x0c0c0c0c
// (heap buffer overflow), which crashed in the smallbin unlink.
// ---------------------------------------------------------------------------
static SceUID g_malloc_mutex = 0;
static SceUID g_malloc_owner = 0;
static int g_malloc_depth = 0;
SceUID sceKernelGetThreadId(void);

void __malloc_lock(void) {
    SceUID self = sceKernelGetThreadId();
    if (g_malloc_owner == self) {
        g_malloc_depth++;
        return;
    }
    if (g_malloc_mutex == 0) {
        SceUID id = sceKernelCreateMutex("soloader_malloc", 0, 0, NULL);
        if (id >= 0)
            g_malloc_mutex = id;
    }
    if (g_malloc_mutex != 0)
        sceKernelLockMutex(g_malloc_mutex, 1, NULL);
    g_malloc_owner = self;
    g_malloc_depth = 1;
}

void __malloc_unlock(void) {
    if (g_malloc_depth > 0 && --g_malloc_depth == 0) {
        g_malloc_owner = 0;
        if (g_malloc_mutex != 0)
            sceKernelUnlockMutex(g_malloc_mutex, 1);
    }
}

// free() with a tripwire: dlmalloc stores [prev_size, size|flags] in the
// 8 bytes before the user pointer. If the size field is garbage (e.g. the
// 0x0c0c0c0c overwrite seen in the wild), log it before the inevitable
// crash so the bootlog shows the corruption explicitly.
void free_soloader(void *mem) {
    if (mem) {
        unsigned int head = ((unsigned int *)mem)[-1];  // size|flags
        unsigned int size = head & ~7u;
        if (head == 0x0c0c0c0c || size == 0 || size > 256u * 1024u * 1024u) {
            blog("HEAP: free(%p) chunk header corrupt: head=%#x (possible buffer overflow)",
                 mem, head);
        } else {
            // Bug 22 diagnostic: use-after-free tripwire. Fill the user area
            // with 0xDD before handing it to dlmalloc (which overwrites the
            // first 16 bytes with freelist links). If the game later
            // dereferences a dangling pointer, it reads 0xDDDDDDDD (kernel
            // space -> immediate data abort) instead of stale pixel data,
            // proving UAF vs wild write. Header was validated above, so the
            // size is trustworthy (chunksize includes the 8-byte header).
            if (size > 8)
                memset(mem, 0xdd, size - 8);
        }
    }
    free(mem);
}

// malloc() with serializer-buffer mitigation: the game's custom JSON
// preferences serializer uses a fixed 256-byte heap buffer but emits
// ~600+ bytes when the ptree has depth-3 nesting (the engine.res block),
// overflowing into adjacent heap and crashing later in free(). Enlarge
// 256-byte allocations to 4096 so the serializer output fits. Log the
// first few callers so we can identify the serializer for a precise fix.
void *malloc_soloader(size_t size) {
    static int log_count = 0;
    if (size == 256) {
        if (log_count < 8) {
            void *caller = __builtin_return_address(0);
            blog("HEAP: malloc(256) -> enlarged to 4096, caller=0x%x", (unsigned)caller);
            log_count++;
        }
        size = 4096;
    }
    void *p = malloc(size);
    heap_track(p, size);
    return p;
}

// memset() interceptor to catch the 0x0C heap writer: the game corrupts heap
// with 0x0c bytes (later crash: free() on chunk with fd/bk=0x0c0c0c0c).
// Log every memset that fills with 0x0c so the bootlog identifies the
// writer's call site and size. Limited to first 16 to avoid spam.
void *memset_soloader(void *dst, int c, size_t n) {
    static int log_count = 0;
    if ((c & 0xff) == 0x0c) {
        if (log_count < 16) {
            void *caller = __builtin_return_address(0);
            blog("HEAP: memset(0x0c, size=%u) dst=0x%x caller=0x%x",
                 (unsigned)n, (unsigned)(uintptr_t)dst, (unsigned)(uintptr_t)caller);
            log_count++;
        }
    }
    return memset(dst, c, n);
}

// ---------------------------------------------------------------------------
// Bug 17: __cxa_throw/__cxa_rethrow interposition + heap-corruption timing.
//
// The game dies in a std::string dtor during exception unwinding, on memory
// already clobbered with 0x0c — but we never learn WHAT it was throwing,
// because the unwind crashes first. The throw entry points are interposed
// via GOT patching in main.c (the game binds them from its own
// libc++_shared.so through DT_NEEDED, which wins over our fallback table,
// so the table alone cannot intercept them). Each throw logs:
//   THROW: <mangled typeinfo name> heap-corrupt=<yes|no>
// plus a best-effort what() for known std::exception derivatives.
// The heap scan answers the timing question: does the 0x0C corruption
// predate the throw (writer ran during parse/serialize) or appear mid-unwind?
// ---------------------------------------------------------------------------

// Game-heap extent tracking: the malloc/memalign wrappers record the
// lowest/highest heap addresses handed out so the throw hook can bound its
// corruption scan to actually-allocated memory. (newlib never unmaps sbrk
// memory — munmap is stubbed — so the whole tracked range stays readable.)
static uintptr_t g_heap_lo = (uintptr_t)-1;
static uintptr_t g_heap_hi = 0;

static void heap_track(void *p, size_t n) {
    if (!p || n == 0)
        return;
    uintptr_t a = (uintptr_t)p;
    uintptr_t b = a + n;
    if (a < g_heap_lo)
        g_heap_lo = a;
    if (b > g_heap_hi)
        g_heap_hi = b;
}

// Scan the tracked game-heap range for the 0x0c0c0c0c corruption word.
// Word-aligned reads only; the range is capped as a sanity backstop.
static int heap_has_0c0c0c0c(void) {
    if (g_heap_hi <= g_heap_lo)
        return 0;
    uintptr_t lo = g_heap_lo & ~(uintptr_t)3;
    uintptr_t hi = g_heap_hi & ~(uintptr_t)3;
    if (hi - lo > 8u * 1024u * 1024u)
        hi = lo + 8u * 1024u * 1024u;
    const uint32_t *p = (const uint32_t *)lo;
    const uint32_t *end = (const uint32_t *)hi;
    for (; p < end; p++) {
        if (*p == 0x0c0c0c0c)
            return 1;
    }
    return 0;
}

// Best-effort std::exception::what() for known single-inheritance
// std::exception derivatives at offset 0 (every Boost property_tree
// exception type qualifies: ptree_bad_path -> ptree_error ->
// std::runtime_error -> std::exception). std::exception's vtable is
// [0]=complete dtor, [1]=deleting dtor, [2]=what(); the primary vtable of a
// singly-derived class keeps that layout with the final overrider.
// The mangled-name allowlist keeps this from ever touching a foreign object.
static const char *exception_what_guarded(void *ex, const char *mangled) {
    static const char *const ok[] = {
        "ptree_bad_path", "ptree_bad_data", "ptree_error",
        "json_parser_error", "info_parser_error", "ini_parser_error",
        "xml_parser_error", "St9exception", "St13runtime_error",
        "St11logic_error", "runtime_error", "logic_error",
        "bad_alloc", "bad_cast", "bad_typeid", NULL
    };
    if (!ex || !mangled)
        return NULL;
    int allowed = 0;
    for (int i = 0; ok[i]; i++) {
        if (strstr(mangled, ok[i])) { allowed = 1; break; }
    }
    if (!allowed)
        return NULL;
    void **vtable = *(void ***)ex;
    const char *(*what_fn)(const void *) =
        (const char *(*)(const void *))vtable[2];
    if (!what_fn)
        return NULL;
    return what_fn(ex);
}

static void (*g_real_cxa_throw)(void *, void *, void (*)(void *)) = NULL;
static void (*g_real_cxa_rethrow)(void) = NULL;
static void *(*g_cxa_current_exception_type)(void) = NULL;

void cxa_throw_hook_init(void *real_throw, void *real_rethrow,
                         void *cur_exception_type) {
    g_real_cxa_throw = real_throw;
    g_real_cxa_rethrow = real_rethrow;
    g_cxa_current_exception_type = cur_exception_type;
}

// Itanium ABI: std::type_info is [vtable, __type_name]; [1] is the
// mangled name pointer. Raw read, no virtual call, no allocation.
static const char *tinfo_mangled_name(void *tinfo) {
    if (!tinfo)
        return "(null tinfo)";
    const char *name = ((const char **)tinfo)[1];
    return name ? name : "(null name)";
}

void __cxa_throw_soloader(void *ex, void *tinfo, void (*dest)(void *)) {
    static int log_count = 0;
    if (log_count < 16) {
        const char *mangled = tinfo_mangled_name(tinfo);
        int corrupt = heap_has_0c0c0c0c();
        blog("THROW: %s heap-corrupt=%s", mangled, corrupt ? "yes" : "no");
        const char *w = exception_what_guarded(ex, mangled);
        if (w)
            blog("THROW: what(): %s", w);
        // Bug 27 diagnostic: capture the throw call chain so the next
        // bootlog names the exact get_child("<xmlattr>") site. The game is
        // built with frame pointers (push {r4,r6,r7,lr} / add r7,sp,#8).
        blog("THROW: backtrace ret0=%p ret1=%p ret2=%p ret3=%p ret4=%p ret5=%p",
             __builtin_return_address(0), __builtin_return_address(1),
             __builtin_return_address(2), __builtin_return_address(3),
             __builtin_return_address(4), __builtin_return_address(5));
        log_count++;
    }
    if (g_real_cxa_throw)
        g_real_cxa_throw(ex, tinfo, dest);
    for (;;) { /* noreturn backstop */ }
}

void __cxa_rethrow_soloader(void) {
    static int log_count = 0;
    if (log_count < 16) {
        const char *mangled = "<rethrow>";
        if (g_cxa_current_exception_type) {
            void *tinfo = g_cxa_current_exception_type();
            if (tinfo)
                mangled = tinfo_mangled_name(tinfo);
        }
        int corrupt = heap_has_0c0c0c0c();
        blog("THROW: %s heap-corrupt=%s", mangled, corrupt ? "yes" : "no");
        log_count++;
    }
    if (g_real_cxa_rethrow)
        g_real_cxa_rethrow();
    for (;;) { /* noreturn backstop */ }
}

// ---------------------------------------------------------------------------
// Bug 22 diagnostic: sscanf/vsscanf interceptor. The game crashes inside
// newlib's __ssvfscanf_r during onSurfaceChanged (data abort on a garbage
// input pointer). Log the caller's address, the input pointer, and the
// format so the next bootlog names the bad call. blog() is unbuffered, so
// the pointer line survives even if the byte peek faults.
// ---------------------------------------------------------------------------
static int sscanf_log_count = 0;

static void log_sscanf_call(const char *str, const char *fmt, void *caller) {
    if (sscanf_log_count >= 16)
        return;
    // Pointer + fmt FIRST (always safe); the str byte peek comes after.
    blog("SCAN: sscanf(str=%p, fmt=\"%.64s\") caller=%p",
         (const void *)str, fmt ? fmt : "(null)", caller);
    if (str) {
        unsigned char peek[16];
        __builtin_memcpy(peek, str, sizeof(peek));
        blog("SCAN: str[0..16] = %02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x",
             peek[0], peek[1], peek[2], peek[3], peek[4], peek[5], peek[6],
             peek[7], peek[8], peek[9], peek[10], peek[11], peek[12],
             peek[13], peek[14], peek[15]);
    }
    sscanf_log_count++;
}

int sscanf_soloader(const char *str, const char *fmt, ...) {
    va_list ap;
    void *caller = __builtin_return_address(0);
    log_sscanf_call(str, fmt, caller);
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

int vsscanf_soloader(const char *str, const char *fmt, va_list ap) {
    log_sscanf_call(str, fmt, __builtin_return_address(0));
    return vsscanf(str, fmt, ap);
}
