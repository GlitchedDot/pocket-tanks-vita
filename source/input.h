#ifndef PT_INPUT_H
#define PT_INPUT_H

#include <falso_jni/FalsoJNI.h>

// Native entry points (resolved in main.c)
extern void (*PT_onTouch)(void *env, void *clazz, void *motionEvent);
extern void (*PT_onGamepadButton)(void *env, void *clazz, int keyCode, int pressed, int player);
extern void (*PT_onGamepadAxis)(void *env, void *clazz, int axis, float value, int player);
// Bug 34: the engine's character channel — one call is a complete
// key tap carrying a character code (bug34-analysis.md §5). Called
// only by main.c's keyboard_service() to inject IME results.
extern void (*PT_onKeyDown)(void *env, void *clazz, int code);

// Fake MotionEvent jobject passed to onTouch (any non-NULL pointer works;
// the 7 methods read from motionevent state)
extern void *pt_motion_event_obj;

// Bug 33 (Build Q, bug33-analysis.md 32 item 1): loader-side poll
// counters. Pure diagnostics — input.c increments, main.c's
// 300-frame heartbeat prints. All cumulative since boot, so
// per-interval movement is the difference between consecutive
// heartbeat lines. They separate the three ways input can die
// before the ring: the Vita never told the poller (peek errors /
// touch_reports flat), the poller never forwarded (reports move,
// send/btn counters flat), or the poller forwarded fine and the
// break is downstream (counters move, census stays empty).
typedef struct {
    unsigned int touch_reports;  // poll_touch calls with reportNum > 0
    unsigned int send_down;      // send_touch_event, masked action DOWN (0)
    unsigned int send_up;        // send_touch_event, masked action UP (1)
    unsigned int send_move;      // send_touch_event, masked action MOVE (2)
    unsigned int send_ptr;       // send_touch_event, POINTER_DOWN/UP (5/6)
    unsigned int btn_calls;      // PT_onGamepadButton calls, total
    unsigned int btn_press;      // ... with pressed != 0
    unsigned int btn_release;    // ... with pressed == 0
    unsigned int axis_calls;     // PT_onGamepadAxis calls
    unsigned int touch_peek_err; // sceTouchPeek returns < 0, count
    int touch_peek_last;         // last sceTouchPeek return value
    unsigned int ctrl_peek_err;  // sceCtrlPeekBufferPositive < 0, count
    int ctrl_peek_last;          // last sceCtrlPeekBufferPositive value
} input_poll_counters_t;

extern input_poll_counters_t g_input_poll_counters;

void input_init(void);
// Poll touch + gamepad; call every frame before onDrawFrame.
void input_poll(void);

#endif
