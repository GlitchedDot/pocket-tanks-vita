#include "fake_motionevent.h"
#include "bootlog.h"

#include <stdarg.h>
#include <string.h>

static struct {
    int packedAction;
    int pointerCount;
    int ids[ME_MAX_POINTERS];
    float xs[ME_MAX_POINTERS];
    float ys[ME_MAX_POINTERS];
} me_state;

void motionevent_update(int packedAction, int pointerCount,
                        const int *ids, const float *xs, const float *ys) {
    me_state.packedAction = packedAction;
    me_state.pointerCount = pointerCount > ME_MAX_POINTERS ? ME_MAX_POINTERS : pointerCount;
    for (int i = 0; i < me_state.pointerCount; i++) {
        me_state.ids[i] = ids[i];
        me_state.xs[i] = xs[i];
        me_state.ys[i] = ys[i];
    }
}

/* Bug 33-R (bug33-analysis.md 38 item 1): getter readback tracer.
 * Pass 5 proved every static link of the readback chain correct —
 * send path, fake getters, field tables, FalsoJNI dispatch, varargs
 * thunks, handler dispatch — and the runtime still contradicts the
 * statics, so this logs the values the engine's handlers actually
 * READ: each getter's return value plus the me_state snapshot at
 * that instant. The handlers' dispatch turns on getActionMasked
 * alone (masked 0 enters the DOWN branch, whose push is
 * straight-line unavoidable), so the first calls are the decisive
 * sample: masked = 0 on DOWN sends acquits the readback and points
 * inside onTouch; masked = -1 means the runtime method-ID table
 * diverges from the source; any other value on a DOWN send means
 * me_state is stale or rewritten between update and read, and the
 * logged packedAction names it. Capped at GET_TRACE_CAP logged
 * lines so the TITLE era cannot recreate the logging flood Build M
 * removed; the per-line call index keeps counting past the cap so
 * the cap itself is visible in the log. Getter behavior is
 * unchanged — the result is computed exactly as before, traced,
 * then returned. */
#define GET_TRACE_CAP 80
static unsigned g_get_trace_calls = 0;

static void get_trace_int(const char *name, int idx, jint ret) {
    unsigned n = ++g_get_trace_calls;
    if (n > GET_TRACE_CAP) {
        if (n == GET_TRACE_CAP + 1)
            blog("GET: cap %d reached (further getter calls counted, not logged)",
                 GET_TRACE_CAP);
        return;
    }
    blog("GET: #%u %s idx=%d ret=%d packed=0x%08x masked=%d aidx=%d count=%d",
         n, name, idx, (int)ret, (unsigned)me_state.packedAction,
         me_state.packedAction & 0xFF, (me_state.packedAction >> 8) & 0xFF,
         me_state.pointerCount);
}

static void get_trace_float(const char *name, int idx, jfloat ret) {
    unsigned n = ++g_get_trace_calls;
    if (n > GET_TRACE_CAP) {
        if (n == GET_TRACE_CAP + 1)
            blog("GET: cap %d reached (further getter calls counted, not logged)",
                 GET_TRACE_CAP);
        return;
    }
    float sx = (idx >= 0 && idx < me_state.pointerCount) ? me_state.xs[idx] : 0.0f;
    float sy = (idx >= 0 && idx < me_state.pointerCount) ? me_state.ys[idx] : 0.0f;
    blog("GET: #%u %s idx=%d ret=%f packed=0x%08x masked=%d aidx=%d count=%d x=%f y=%f",
         n, name, idx, (double)ret, (unsigned)me_state.packedAction,
         me_state.packedAction & 0xFF, (me_state.packedAction >> 8) & 0xFF,
         me_state.pointerCount, (double)sx, (double)sy);
}

jint motionevent_getAction(jmethodID id, va_list args) {
    (void)id; (void)args;
    jint ret = me_state.packedAction;
    get_trace_int("getAction", -1, ret);
    return ret;
}

jint motionevent_getActionMasked(jmethodID id, va_list args) {
    (void)id; (void)args;
    jint ret = me_state.packedAction & 0xFF;
    get_trace_int("getActionMasked", -1, ret);
    return ret;
}

jint motionevent_getActionIndex(jmethodID id, va_list args) {
    (void)id; (void)args;
    jint ret = (me_state.packedAction >> 8) & 0xFF;
    get_trace_int("getActionIndex", -1, ret);
    return ret;
}

jint motionevent_getPointerCount(jmethodID id, va_list args) {
    (void)id; (void)args;
    jint ret = me_state.pointerCount;
    get_trace_int("getPointerCount", -1, ret);
    return ret;
}

jint motionevent_getPointerId(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    jint ret = -1;
    if (index >= 0 && index < me_state.pointerCount)
        ret = me_state.ids[index];
    get_trace_int("getPointerId", (int)index, ret);
    return ret;
}

jfloat motionevent_getX(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    jfloat ret = 0.0f;
    if (index >= 0 && index < me_state.pointerCount)
        ret = me_state.xs[index];
    get_trace_float("getX", (int)index, ret);
    return ret;
}

jfloat motionevent_getY(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    jfloat ret = 0.0f;
    if (index >= 0 && index < me_state.pointerCount)
        ret = me_state.ys[index];
    get_trace_float("getY", (int)index, ret);
    return ret;
}
