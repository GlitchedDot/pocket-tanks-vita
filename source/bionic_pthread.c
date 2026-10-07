/*
 * bionic_pthread.c — Bionic-ABI pthread implementations for Android .so ports.
 *
 * Why this exists: the game .sos were built against Android's bionic libc,
 * where pthread_mutex_t / pthread_cond_t are 4-byte VALUE types (a zeroed
 * struct is a valid unlocked mutex) and pthread_key_t is an int. The
 * VitaSDK's newlib instead defines pthread_mutex_t etc. as POINTERS, where
 * only (void*)-1 is a valid static initializer. Mapping the .so's pthread
 * calls straight to newlib's made libc++abi's __cxa_guard_acquire abort
 * ("failed to acquire mutex") during static init, because newlib rejected
 * the zero-initialized guard mutex.
 *
 * These functions use Bionic's type layouts (the .so only ever passes us
 * pointers; the 4-byte slots are ours to interpret) and back them with real
 * Vita kernel primitives. They are exported to the .so via dynlib.c under
 * the plain pthread_* / __cxa_guard_* names.
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bootlog.h"

#include <falso_jni/FalsoJNI.h> /* for &jni (fake JNIEnv*) in the key-0 fixup below */

/* Bionic (NDK r21, 32-bit ARM) layouts. Sizes must match; the .so allocates
 * these on its own stack/globals, we just read/write the slots. */
typedef volatile int32_t b_mutex_t;          /* 4 bytes; 0 = statically init'd */
typedef volatile int32_t b_cond_t;           /* 4 bytes; 0 = statically init'd */
typedef int32_t          b_mutexattr_t;      /* 4 bytes; holds type */
typedef int32_t          b_condattr_t;       /* 4 bytes; holds clock id */
typedef int32_t          b_key_t;            /* 4 bytes */
typedef struct { int32_t state; int32_t spare; } b_once_t; /* 8 bytes */

/* Bionic mutex types (bionic pthread.h) */
#define B_MUTEX_NORMAL    0
#define B_MUTEX_RECURSIVE 1
#define B_MUTEX_ERRORCHECK 2

static int g_mutex_seq = 0;

static int mutex_ensure(b_mutex_t *v, int recursive) {
    int32_t cur = __atomic_load_n(v, __ATOMIC_ACQUIRE);
    if (cur != 0)
        return (int)cur;
    char name[32];
    int seq = __atomic_fetch_add(&g_mutex_seq, 1, __ATOMIC_RELAXED);
    sceClibSnprintf(name, sizeof(name), "bm_%d", seq);
    SceUInt attr = recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0;
    SceUID id = sceKernelCreateMutex(name, attr, 0, NULL);
    if (id < 0) {
        blog("bionic_pthread: sceKernelCreateMutex failed: 0x%08x", id);
        return 0;
    }
    int32_t expected = 0;
    if (!__atomic_compare_exchange_n(v, &expected, (int32_t)id, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* Lost the race; use the winner's mutex. */
        sceKernelDeleteMutex(id);
        return (int)expected;
    }
    return (int)id;
}

/* --- mutex attributes --- */
int bionic_pthread_mutexattr_init(void *attr) {
    *(b_mutexattr_t *)attr = B_MUTEX_NORMAL;
    return 0;
}
int bionic_pthread_mutexattr_destroy(void *attr) {
    (void)attr;
    return 0;
}
int bionic_pthread_mutexattr_settype(void *attr, int type) {
    if (type < B_MUTEX_NORMAL || type > B_MUTEX_ERRORCHECK)
        return EINVAL;
    *(b_mutexattr_t *)attr = (int32_t)type;
    return 0;
}
int bionic_pthread_mutexattr_gettype(const void *attr, int *type) {
    *type = (int)*(const b_mutexattr_t *)attr;
    return 0;
}

/* --- mutexes --- */
int bionic_pthread_mutex_init(void *m, const void *attr) {
    int type = attr ? (int)*(const b_mutexattr_t *)attr : B_MUTEX_NORMAL;
    int32_t *v = (int32_t *)m;
    *v = 0;
    /* Eagerly create so init failures surface here, not at first lock. */
    if (!mutex_ensure((b_mutex_t *)m, type == B_MUTEX_RECURSIVE))
        return ENOMEM;
    return 0;
}
int bionic_pthread_mutex_destroy(void *m) {
    int32_t v = __atomic_exchange_n((int32_t *)m, 0, __ATOMIC_ACQ_REL);
    if (v != 0)
        sceKernelDeleteMutex((SceUID)v);
    return 0;
}
int bionic_pthread_mutex_lock(void *m) {
    int32_t v = __atomic_load_n((int32_t *)m, __ATOMIC_ACQUIRE);
    blog("API: pthread_mutex_lock(m=%p, *m=%#x)", m, (unsigned)v);
    int id = mutex_ensure((b_mutex_t *)m, 0);
    if (!id)
        return EINVAL;
    return sceKernelLockMutex((SceUID)id, 1, NULL) < 0 ? EINVAL : 0;
}
int bionic_pthread_mutex_trylock(void *m) {
    int id = mutex_ensure((b_mutex_t *)m, 0);
    if (!id)
        return EINVAL;
    return sceKernelTryLockMutex((SceUID)id, 1) < 0 ? EBUSY : 0;
}
int bionic_pthread_mutex_unlock(void *m) {
    int32_t v = __atomic_load_n((int32_t *)m, __ATOMIC_ACQUIRE);
    blog("API: pthread_mutex_unlock(m=%p, *m=%#x)", m, (unsigned)v);
    if (v == 0)
        return EINVAL;
    return sceKernelUnlockMutex((SceUID)v, 1) < 0 ? EPERM : 0;
}

/* --- condition variables --- */
typedef struct {
    SceUID guard;   /* protects waiters */
    SceUID sema;    /* waiters block here */
    int    waiters;
} bcond_internal_t;

static int g_cond_seq = 0;

static bcond_internal_t *cond_ensure(b_cond_t *c) {
    int32_t cur = __atomic_load_n((int32_t *)c, __ATOMIC_ACQUIRE);
    if (cur != 0)
        return (bcond_internal_t *)(uintptr_t)cur;
    bcond_internal_t *nc = calloc(1, sizeof(*nc));
    if (!nc)
        return NULL;
    char name[32];
    int seq = __atomic_fetch_add(&g_cond_seq, 1, __ATOMIC_RELAXED);
    sceClibSnprintf(name, sizeof(name), "bcg_%d", seq);
    nc->guard = sceKernelCreateMutex(name, 0, 0, NULL);
    sceClibSnprintf(name, sizeof(name), "bcs_%d", seq);
    nc->sema = sceKernelCreateSema(name, 0, 0, 0x7fffffff, NULL);
    if (nc->guard < 0 || nc->sema < 0) {
        blog("bionic_pthread: cond primitive creation failed");
        if (nc->guard >= 0) sceKernelDeleteMutex(nc->guard);
        if (nc->sema >= 0) sceKernelDeleteSema(nc->sema);
        free(nc);
        return NULL;
    }
    int32_t expected = 0;
    if (!__atomic_compare_exchange_n((int32_t *)c, &expected,
                                     (int32_t)(uintptr_t)nc, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sceKernelDeleteMutex(nc->guard);
        sceKernelDeleteSema(nc->sema);
        free(nc);
        return (bcond_internal_t *)(uintptr_t)expected;
    }
    return nc;
}

int bionic_pthread_condattr_init(void *attr) {
    *(b_condattr_t *)attr = 0; /* CLOCK_REALTIME */
    return 0;
}
int bionic_pthread_condattr_destroy(void *attr) {
    (void)attr;
    return 0;
}
int bionic_pthread_condattr_setclock(void *attr, int clockid) {
    *(b_condattr_t *)attr = (int32_t)clockid;
    return 0;
}

int bionic_pthread_cond_init(void *c, const void *attr) {
    (void)attr;
    *(int32_t *)c = 0;
    return cond_ensure((b_cond_t *)c) ? 0 : ENOMEM;
}
int bionic_pthread_cond_destroy(void *c) {
    bcond_internal_t *ic = (bcond_internal_t *)(uintptr_t)
        __atomic_exchange_n((int32_t *)c, 0, __ATOMIC_ACQ_REL);
    if (ic) {
        sceKernelDeleteMutex(ic->guard);
        sceKernelDeleteSema(ic->sema);
        free(ic);
    }
    return 0;
}
int bionic_pthread_cond_wait(void *c, void *m) {
    bcond_internal_t *ic = cond_ensure((b_cond_t *)c);
    if (!ic)
        return EINVAL;
    sceKernelLockMutex(ic->guard, 1, NULL);
    ic->waiters++;
    sceKernelUnlockMutex(ic->guard, 1);
    bionic_pthread_mutex_unlock(m);
    sceKernelWaitSema(ic->sema, 1, NULL);
    sceKernelLockMutex(ic->guard, 1, NULL);
    ic->waiters--;
    sceKernelUnlockMutex(ic->guard, 1);
    return bionic_pthread_mutex_lock(m);
}
int bionic_pthread_cond_timedwait(void *c, void *m, const struct timespec *abstime) {
    bcond_internal_t *ic = cond_ensure((b_cond_t *)c);
    if (!ic)
        return EINVAL;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t target_us = (int64_t)abstime->tv_sec * 1000000LL + abstime->tv_nsec / 1000;
    int64_t now_us = (int64_t)now.tv_sec * 1000000LL + now.tv_nsec / 1000;
    int64_t delta_us = target_us - now_us;
    if (delta_us <= 0)
        return ETIMEDOUT;
    sceKernelLockMutex(ic->guard, 1, NULL);
    ic->waiters++;
    sceKernelUnlockMutex(ic->guard, 1);
    bionic_pthread_mutex_unlock(m);
    SceUInt to = delta_us > 0xFFFFFFFE ? 0xFFFFFFFE : (SceUInt)delta_us;
    int r = sceKernelWaitSema(ic->sema, 1, &to);
    sceKernelLockMutex(ic->guard, 1, NULL);
    ic->waiters--;
    sceKernelUnlockMutex(ic->guard, 1);
    bionic_pthread_mutex_lock(m);
    return r < 0 ? ETIMEDOUT : 0;
}
int bionic_pthread_cond_signal(void *c) {
    bcond_internal_t *ic = cond_ensure((b_cond_t *)c);
    if (!ic)
        return EINVAL;
    sceKernelLockMutex(ic->guard, 1, NULL);
    if (ic->waiters > 0)
        sceKernelSignalSema(ic->sema, 1);
    sceKernelUnlockMutex(ic->guard, 1);
    return 0;
}
int bionic_pthread_cond_broadcast(void *c) {
    bcond_internal_t *ic = cond_ensure((b_cond_t *)c);
    if (!ic)
        return EINVAL;
    sceKernelLockMutex(ic->guard, 1, NULL);
    if (ic->waiters > 0)
        sceKernelSignalSema(ic->sema, ic->waiters);
    sceKernelUnlockMutex(ic->guard, 1);
    return 0;
}

/* --- pthread_once --- */
int bionic_pthread_once(void *once, void (*init_routine)(void)) {
    int32_t *o = (int32_t *)once;
    if (__atomic_load_n(&o[0], __ATOMIC_ACQUIRE) == 2)
        return 0;
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&o[0], &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init_routine();
        __atomic_store_n(&o[0], 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&o[0], __ATOMIC_ACQUIRE) != 2)
            sceKernelDelayThread(1000);
    }
    return 0;
}

/* --- thread-specific data (bionic pthread_key_t is an int) --- */
#define B_PTHREAD_KEYS_MAX 128
static int      g_key_used[B_PTHREAD_KEYS_MAX];
static SceUID   g_key_lock = 0;
/* Low key slots are reserved, never handed out by pthread_key_create.
 * Why: libengine.so uses pthread key 0 directly without ever calling
 * pthread_key_create (verified in the binary: its TLS getters read a
 * zero-initialized key global, and the .so's only key_create call is
 * Boost.Asio's). On bionic this accidentally works because key 0 is a
 * usable slot. If we gave slot 0 to a legitimate key_create caller, the
 * two would alias the same (tid,key) TLS slot: observed crash was the
 * game's thread-local singleton cell holding 1 instead of an object
 * pointer, faulting inside onCreate's init path. */
#define B_PTHREAD_KEYS_RESERVED 8
static int g_keys_reserved = 0;
typedef struct tls_entry {
    SceUID           tid;
    int              key;
    const void      *value;
    struct tls_entry *next;
} tls_entry_t;
static tls_entry_t *g_tls_head = NULL;

static SceUID key_lock_ensure(void) {
    if (g_key_lock)
        return g_key_lock;
    SceUID id = sceKernelCreateMutex("bionic_keylock", 0, 0, NULL);
    SceUID expected = 0;
    /* Single-threaded during key creation in practice; keep it simple. */
    if (g_key_lock == 0 && id >= 0)
        g_key_lock = id;
    (void)expected;
    if (!g_keys_reserved) {
        for (int i = 0; i < B_PTHREAD_KEYS_RESERVED; i++)
            g_key_used[i] = 1;
        g_keys_reserved = 1;
    }
    return g_key_lock;
}

int bionic_pthread_key_create(int *key, void (*dtor)(void *)) {
    (void)dtor; /* thread-exit destructors not supported; game doesn't rely on them */
    SceUID lk = key_lock_ensure();
    if (lk) sceKernelLockMutex(lk, 1, NULL);
    /* Defense in depth: never hand out a slot that already has TLS entries
     * (someone used it via setspecific without creating it). */
    for (tls_entry_t *e = g_tls_head; e; e = e->next) {
        if (e->key >= 0 && e->key < B_PTHREAD_KEYS_MAX)
            g_key_used[e->key] = 1;
    }
    int slot = -1;
    for (int i = 0; i < B_PTHREAD_KEYS_MAX; i++) {
        if (!g_key_used[i]) { g_key_used[i] = 1; slot = i; break; }
    }
    if (lk) sceKernelUnlockMutex(lk, 1);
    if (slot < 0)
        return ENOMEM;
    *key = slot;
    blog("API: pthread_key_create -> key=%d", slot);
    return 0;
}
int bionic_pthread_key_delete(int key) {
    SceUID lk = key_lock_ensure();
    if (lk) sceKernelLockMutex(lk, 1, NULL);
    if (key >= 0 && key < B_PTHREAD_KEYS_MAX) {
        g_key_used[key] = 0;
        tls_entry_t **pp = &g_tls_head;
        while (*pp) {
            if ((*pp)->key == key) {
                tls_entry_t *dead = *pp;
                *pp = dead->next;
                free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    if (lk) sceKernelUnlockMutex(lk, 1);
    return 0;
}
int bionic_pthread_setspecific(int key, const void *value) {
    blog("API: pthread_setspecific(key=%d, value=%p)", key, value);
    /* Key-0 fixup: libengine.so has two thread-local singletons sharing
     * hardcoded pthread key 0 (never via pthread_key_create):
     *  - a simple getter that calloc(2049,1)s a thread struct, and
     *  - a JNIEnv* getter that expects [cell] == JNIEnv* (via GetEnv).
     * If the simple getter claims key 0 first, the JNIEnv* getter finds
     * *cell == 0 and crashes dereferencing it as a JNIEnv* in onCreate.
     * Seed [cell] with our fake JNIEnv* whenever a fresh zeroed cell is
     * stored under key 0. The simple getter's caller never inspects [0],
     * and the JNIEnv* getter requires it — so this is safe on all threads.
     * (The main thread's cell is also pre-seeded from main.c, but game
     * worker threads take this path.) */
    if (key == 0 && value && *(void *const *)value == NULL && jni != NULL) {
        *(void **)value = (void *)&jni;
        blog("API: pthread_setspecific: key-0 cell %p seeded with JNIEnv* %p",
             value, (void *)&jni);
    }
    SceUID tid = sceKernelGetThreadId();
    SceUID lk = key_lock_ensure();
    if (lk) sceKernelLockMutex(lk, 1, NULL);
    tls_entry_t *e = g_tls_head;
    while (e) {
        if (e->tid == tid && e->key == key) { e->value = value; break; }
        e = e->next;
    }
    if (!e) {
        e = malloc(sizeof(*e));
        if (e) {
            e->tid = tid; e->key = key; e->value = value;
            e->next = g_tls_head; g_tls_head = e;
        }
    }
    if (lk) sceKernelUnlockMutex(lk, 1);
    return e ? 0 : ENOMEM;
}
void *bionic_pthread_getspecific(int key) {
    SceUID tid = sceKernelGetThreadId();
    SceUID lk = key_lock_ensure();
    void *ret = NULL;
    if (lk) sceKernelLockMutex(lk, 1, NULL);
    for (tls_entry_t *e = g_tls_head; e; e = e->next) {
        if (e->tid == tid && e->key == key) { ret = (void *)e->value; break; }
    }
    if (lk) sceKernelUnlockMutex(lk, 1);
    blog("API: pthread_getspecific(key=%d) -> %p", key, ret);
    return ret;
}

/* --- Itanium C++ static-init guards (belt and braces: normally satisfied by
 * libc++_shared's own definitions, but libengine.so imports these as
 * undefined, so ours must have correct semantics if selected). --- */
int __cxa_guard_acquire_soloader(uint64_t *guard_object) {
    uint8_t *g = (uint8_t *)guard_object;
    uint8_t expected = 0;
    if (__atomic_compare_exchange_n(g, &expected, (uint8_t)1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 1; /* this thread runs the initializer */
    while (__atomic_load_n(g, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
    return 0;
}
void __cxa_guard_release_soloader(uint64_t *guard_object) {
    __atomic_store_n((uint8_t *)guard_object, (uint8_t)2, __ATOMIC_RELEASE);
}
void __cxa_guard_abort_soloader(uint64_t *guard_object) {
    __atomic_store_n((uint8_t *)guard_object, (uint8_t)0, __ATOMIC_RELEASE);
}
