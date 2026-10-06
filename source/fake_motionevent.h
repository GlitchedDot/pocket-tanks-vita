#ifndef PT_FAKE_MOTIONEVENT_H
#define PT_FAKE_MOTIONEVENT_H

#include <falso_jni/FalsoJNI.h>

#define ME_MAX_POINTERS 6

// Android MotionEvent action codes
#define ME_ACTION_DOWN 0
#define ME_ACTION_UP 1
#define ME_ACTION_MOVE 2
#define ME_ACTION_CANCEL 3
#define ME_ACTION_POINTER_DOWN 5
#define ME_ACTION_POINTER_UP 6

// Populate the fake MotionEvent state before calling onTouch.
// action: use ME_ACTION_* combined as (actionIndex << 8) | actionMasked,
// exactly like Android packs it.
void motionevent_update(int packedAction, int pointerCount,
                        const int *ids, const float *xs, const float *ys);

// The 7 JNI methods the native code looks up on android/view/MotionEvent.
jint motionevent_getAction(jmethodID id, va_list args);
jint motionevent_getActionMasked(jmethodID id, va_list args);
jint motionevent_getActionIndex(jmethodID id, va_list args);
jint motionevent_getPointerCount(jmethodID id, va_list args);
jint motionevent_getPointerId(jmethodID id, va_list args);
jfloat motionevent_getX(jmethodID id, va_list args);
jfloat motionevent_getY(jmethodID id, va_list args);

#endif
