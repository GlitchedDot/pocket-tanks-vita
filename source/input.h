#ifndef PT_INPUT_H
#define PT_INPUT_H

#include <falso_jni/FalsoJNI.h>

// Native entry points (resolved in main.c)
extern void (*PT_onTouch)(void *env, void *clazz, void *motionEvent);
extern void (*PT_onGamepadButton)(void *env, void *clazz, int keyCode, int pressed, int player);
extern void (*PT_onGamepadAxis)(void *env, void *clazz, int axis, float value, int player);

// Fake MotionEvent jobject passed to onTouch (any non-NULL pointer works;
// the 7 methods read from motionevent state)
extern void *pt_motion_event_obj;

void input_init(void);
// Poll touch + gamepad; call every frame before onDrawFrame.
void input_poll(void);

#endif
