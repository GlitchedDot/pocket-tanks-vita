#include "input.h"
#include "fake_motionevent.h"
#include "utils.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <string.h>
#include <math.h>

void (*PT_onTouch)(void *env, void *clazz, void *motionEvent);
void (*PT_onGamepadButton)(void *env, void *clazz, int keyCode, int pressed, int player);
void (*PT_onGamepadAxis)(void *env, void *clazz, int axis, float value, int player);

static int dummy_me_obj;
void *pt_motion_event_obj = &dummy_me_obj;

// ---- touch state ----------------------------------------------------------
#define MAX_TOUCH 6
static struct {
    int active;
    int id;
    float x, y;
} touches[MAX_TOUCH];
static int touch_count = 0;

// ---- gamepad state ---------------------------------------------------------
static unsigned int prev_buttons = 0;

// Android keycodes
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_L3 106
#define AKEYCODE_BUTTON_R3 107
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

// Android axis codes
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14

static const struct {
    unsigned int sce;
    int android;
} button_map[] = {
    {SCE_CTRL_UP, AKEYCODE_DPAD_UP},
    {SCE_CTRL_DOWN, AKEYCODE_DPAD_DOWN},
    {SCE_CTRL_LEFT, AKEYCODE_DPAD_LEFT},
    {SCE_CTRL_RIGHT, AKEYCODE_DPAD_RIGHT},
    {SCE_CTRL_CROSS, AKEYCODE_BUTTON_A},
    {SCE_CTRL_CIRCLE, AKEYCODE_BUTTON_B},
    {SCE_CTRL_SQUARE, AKEYCODE_BUTTON_X},
    {SCE_CTRL_TRIANGLE, AKEYCODE_BUTTON_Y},
    {SCE_CTRL_L1, AKEYCODE_BUTTON_L1},
    {SCE_CTRL_R1, AKEYCODE_BUTTON_R1},
    {SCE_CTRL_L3, AKEYCODE_BUTTON_L3},
    {SCE_CTRL_R3, AKEYCODE_BUTTON_R3},
    {SCE_CTRL_START, AKEYCODE_BUTTON_START},
    {SCE_CTRL_SELECT, AKEYCODE_BUTTON_SELECT},
};

void input_init(void) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(touches, 0, sizeof(touches));
    log_info("input initialized");
}

static void send_touch_event(int packedAction) {
    int ids[MAX_TOUCH];
    float xs[MAX_TOUCH], ys[MAX_TOUCH];
    int n = 0;
    for (int i = 0; i < MAX_TOUCH; i++) {
        if (touches[i].active) {
            ids[n] = touches[i].id;
            xs[n] = touches[i].x;
            ys[n] = touches[i].y;
            n++;
        }
    }
    motionevent_update(packedAction, n, ids, xs, ys);
    if (PT_onTouch)
        PT_onTouch(&jni, NULL, pt_motion_event_obj);
}

static void poll_touch(void) {
    SceTouchData td;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0)
        return;

    // Mark all inactive first; reactivate from report
    int seen[MAX_TOUCH] = {0};
    for (unsigned int i = 0; i < td.reportNum && i < MAX_TOUCH; i++) {
        int id = td.report[i].id;
        // Vita touch coords are 0..1919 / 0..1087; game is 960x544
        float x = td.report[i].x / 2.0f;
        float y = td.report[i].y / 2.0f;

        int slot = -1;
        for (int s = 0; s < MAX_TOUCH; s++) {
            if (touches[s].active && touches[s].id == id) {
                slot = s;
                break;
            }
        }
        if (slot < 0) {
            // New touch: find free slot
            for (int s = 0; s < MAX_TOUCH; s++) {
                if (!touches[s].active) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0)
                continue;
            touches[slot].active = 1;
            touches[slot].id = id;
            touches[slot].x = x;
            touches[slot].y = y;
            touch_count++;
            // DOWN for first finger, POINTER_DOWN otherwise
            int action = (touch_count == 1) ? ME_ACTION_DOWN
                                            : ME_ACTION_POINTER_DOWN | (slot << 8);
            send_touch_event(action);
        } else {
            if (touches[slot].x != x || touches[slot].y != y) {
                touches[slot].x = x;
                touches[slot].y = y;
                seen[slot] = 2; // moved
            } else {
                seen[slot] = 1; // held
            }
        }
    }

    // Released touches
    int released_any = 0;
    for (int s = 0; s < MAX_TOUCH; s++) {
        if (touches[s].active && !seen[s]) {
            touches[s].active = 0;
            touch_count--;
            released_any = 1;
        }
    }

    if (touch_count == 0 && released_any) {
        send_touch_event(ME_ACTION_UP);
        return;
    }
    if (released_any) {
        // Find one released slot for the POINTER_UP index
        for (int s = 0; s < MAX_TOUCH; s++) {
            if (!touches[s].active) {
                // need the slot that was just released; approximate with first inactive
                send_touch_event(ME_ACTION_POINTER_UP | (s << 8));
                break;
            }
        }
        return;
    }

    int moved = 0;
    for (int s = 0; s < MAX_TOUCH; s++) {
        if (seen[s] == 2)
            moved = 1;
    }
    if (moved)
        send_touch_event(ME_ACTION_MOVE);
}

static float apply_deadzone(float v) {
    if (fabsf(v) < 0.15f)
        return 0.0f;
    return v;
}

static void poll_gamepad(void) {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;

    unsigned int changed = pad.buttons ^ prev_buttons;
    if (changed && PT_onGamepadButton) {
        for (unsigned int i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
            if (changed & button_map[i].sce) {
                int pressed = (pad.buttons & button_map[i].sce) ? 1 : 0;
                PT_onGamepadButton(&jni, NULL, button_map[i].android, pressed, 0);
            }
        }
    }
    prev_buttons = pad.buttons;

    if (PT_onGamepadAxis) {
        float lx = apply_deadzone(((float)pad.lx - 128.0f) / 128.0f);
        float ly = apply_deadzone(((float)pad.ly - 128.0f) / 128.0f);
        float rx = apply_deadzone(((float)pad.rx - 128.0f) / 128.0f);
        float ry = apply_deadzone(((float)pad.ry - 128.0f) / 128.0f);
        PT_onGamepadAxis(&jni, NULL, AXIS_X, lx, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_Y, ly, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_Z, rx, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_RZ, ry, 0);
    }
}

void input_poll(void) {
    poll_touch();
    poll_gamepad();
}
