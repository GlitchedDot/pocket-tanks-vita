// Bionic (Android libc) compatibility shims for symbols the game imports
// that don't exist in the VitaSDK.
#include "utils.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Generic stub return values
// ---------------------------------------------------------------------------
int ret0(void) { return 0; }
int ret1(void) { return 1; }
int ret_neg1(void) { return -1; }
void ret_void(void) {}

// ---------------------------------------------------------------------------
// Stack protector (game was built with -fstack-protector)
// ---------------------------------------------------------------------------
uintptr_t __stack_chk_guard = 0x5a5a5a5a;

void __stack_chk_fail_soloader(void) {
    log_error("__stack_chk_fail: stack smashing detected!");
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

int __open_2(const char *path, int flags) {
    return open(path, flags);
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
    sceKernelExitProcess(1);
}

// ---------------------------------------------------------------------------
// Misc wrappers
// ---------------------------------------------------------------------------
void *__memclr_wrapper(void *s, size_t n) {
    return sceClibMemset(s, 0, n);
}

void *memalign_soloader(size_t alignment, size_t size) {
    // posix_memalign isn't in Vita newlib; align manually.
    // NOTE: free() on the result leaks; acceptable for game use.
    void *raw = malloc(size + alignment + sizeof(void *));
    if (!raw)
        return NULL;
    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~(alignment - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void exit_soloader(int status) {
    log_info("exit(%d) called by game", status);
    sceKernelExitProcess(status);
}

void abort_soloader(void) {
    log_error("abort() called by game");
    sceKernelExitProcess(1);
}

// libc++ __ndk1::string is ABI-identical to std::string; forward the
// __ndk1 constructors the game imports to the loader's libstdc++ versions.
extern void _ZNSt11logic_errorC2ERKS_(void *t, const void *s);
extern void _ZNSt13runtime_errorC1ERKS_(void *t, const void *s);
extern void _ZNSt13runtime_errorC2ERKS_(void *t, const void *s);

void _ZNSt11logic_errorC2ERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE(void *t, const void *s) {
    _ZNSt11logic_errorC2ERKS_(t, s);
}
void _ZNSt13runtime_errorC1ERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE(void *t, const void *s) {
    _ZNSt13runtime_errorC1ERKS_(t, s);
}
void _ZNSt13runtime_errorC2ERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE(void *t, const void *s) {
    _ZNSt13runtime_errorC2ERKS_(t, s);
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
