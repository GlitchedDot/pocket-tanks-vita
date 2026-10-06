#include "fake_motionevent.h"

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

jint motionevent_getAction(jmethodID id, va_list args) {
    (void)id; (void)args;
    return me_state.packedAction;
}

jint motionevent_getActionMasked(jmethodID id, va_list args) {
    (void)id; (void)args;
    return me_state.packedAction & 0xFF;
}

jint motionevent_getActionIndex(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (me_state.packedAction >> 8) & 0xFF;
}

jint motionevent_getPointerCount(jmethodID id, va_list args) {
    (void)id; (void)args;
    return me_state.pointerCount;
}

jint motionevent_getPointerId(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    if (index < 0 || index >= me_state.pointerCount)
        return -1;
    return me_state.ids[index];
}

jfloat motionevent_getX(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    if (index < 0 || index >= me_state.pointerCount)
        return 0.0f;
    return me_state.xs[index];
}

jfloat motionevent_getY(jmethodID id, va_list args) {
    jint index = va_arg(args, jint);
    (void)id;
    if (index < 0 || index >= me_state.pointerCount)
        return 0.0f;
    return me_state.ys[index];
}
