#include "input.h"
#include "fake_motionevent.h"
#include "keyboard.h"
#include "utils.h"
#include "bootlog.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <string.h>
#include <math.h>

void (*PT_onTouch)(void *env, void *clazz, void *motionEvent);
void (*PT_onGamepadButton)(void *env, void *clazz, int keyCode, int pressed, int player);
void (*PT_onGamepadAxis)(void *env, void *clazz, int axis, float value, int player);
void (*PT_onKeyDown)(void *env, void *clazz, int code);

static int dummy_me_obj;
void *pt_motion_event_obj = &dummy_me_obj;

// Bug 33 (Build Q): poll counters (declared in input.h, printed by
// main.c's heartbeat). Counting only — no behavior change.
input_poll_counters_t g_input_poll_counters;

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
    // Build Q: count every forwarding attempt by masked action.
    // poll_touch only ever packs DOWN/UP/MOVE/POINTER_DOWN/
    // POINTER_UP, so the default bucket is exactly the pointer
    // pair.
    switch (packedAction & 0xff) {
    case ME_ACTION_DOWN: g_input_poll_counters.send_down++; break;
    case ME_ACTION_UP:   g_input_poll_counters.send_up++;   break;
    case ME_ACTION_MOVE: g_input_poll_counters.send_move++; break;
    default:             g_input_poll_counters.send_ptr++;  break;
    }
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
    /* Build AD touch probe: log the packed record stream so the
     * match HUD's gesture language (slider widgets, reticle, fire)
     * can be measured from real play before the physical-button
     * synthesis is designed. DOWN/UP/POINTER always; MOVE thinned
     * to >=6px steps in game coords; hard cap so a long match
     * cannot flood the log. Strip with the other diagnostics. */
    {
        static int tp_lines = 0;
        static float tp_lx = -1000.0f, tp_ly = -1000.0f;
        int act = packedAction & 0xff;
        float fx = n > 0 ? xs[0] : -1.0f;
        float fy = n > 0 ? ys[0] : -1.0f;
        int want = (act != ME_ACTION_MOVE);
        if (!want && n > 0) {
            float dx = fx - tp_lx, dy = fy - tp_ly;
            want = (dx * dx + dy * dy) >= 36.0f;
        }
        if (want && tp_lines < 1200) {
            tp_lines++;
            tp_lx = fx; tp_ly = fy;
            blog("TP: act=%d n=%d p0=(%.0f,%.0f)", packedAction, n,
                 (double)fx, (double)fy);
        }
    }
    motionevent_update(packedAction, n, ids, xs, ys);
    if (PT_onTouch) {
        /* Build M: the Bug 31 INPUT trace line is removed (input
         * delivery was proven in bootlog-8); the forward is unchanged. */
        PT_onTouch(&jni, NULL, pt_motion_event_obj);
    }
}

static void poll_touch(void) {
    SceTouchData td;
    int peek = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    g_input_poll_counters.touch_peek_last = peek;
    if (peek < 0) {
        g_input_poll_counters.touch_peek_err++;
        return;
    }
    if (td.reportNum > 0)
        g_input_poll_counters.touch_reports++;

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
            seen[slot] = 1; /* Bug 33 fix (build S): registered this call — the
                               release sweep below must not reap it and emit an
                               immediate ACTION_UP in the same poll. */
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

    // Released touches. Bug 33 (build T): Android delivers
    // ACTION_UP / ACTION_POINTER_UP with the departing pointer
    // STILL PRESENT in the event, at its final coordinates — the
    // engine reads getX(0)/getY(0) off the UP event, and menus
    // activate on the release position. The pre-T sweep cleared
    // the slot before sending, so send_touch_event (which packs
    // only active touches) emitted pointerCount=0 and every UP
    // record carried (0,0): presses registered, taps never
    // activated (bootlog-21). Now: collect the departing slots,
    // send each one's UP event while it is still active (final
    // x/y intact, action index = its index in the packed pointer
    // list), and clear the slot only after the handler call.
    int rel_slot[MAX_TOUCH];
    int n_rel = 0;
    for (int s = 0; s < MAX_TOUCH; s++) {
        if (touches[s].active && !seen[s])
            rel_slot[n_rel++] = s;
    }
    if (n_rel > 0) {
        for (int r = 0; r < n_rel; r++) {
            int s = rel_slot[r];
            if (touch_count == 1) {
                // Last pointer departing: plain ACTION_UP (index 0),
                // pointer still packed at its release position.
                send_touch_event(ME_ACTION_UP);
            } else {
                // Index of the departing pointer within the pointer
                // list send_touch_event is about to pack (active
                // touches in slot order, this one included).
                int idx = 0;
                for (int t = 0; t < s; t++) {
                    if (touches[t].active)
                        idx++;
                }
                send_touch_event(ME_ACTION_POINTER_UP | (idx << 8));
            }
            touches[s].active = 0;
            touch_count--;
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
    int peek = sceCtrlPeekBufferPositive(0, &pad, 1);
    g_input_poll_counters.ctrl_peek_last = peek;
    if (peek < 0) {
        g_input_poll_counters.ctrl_peek_err++;
        return;
    }

    unsigned int changed = pad.buttons ^ prev_buttons;
    if (changed && PT_onGamepadButton) {
        for (unsigned int i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
            if (changed & button_map[i].sce) {
                int pressed = (pad.buttons & button_map[i].sce) ? 1 : 0;
                g_input_poll_counters.btn_calls++;
                if (pressed)
                    g_input_poll_counters.btn_press++;
                else
                    g_input_poll_counters.btn_release++;
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
        g_input_poll_counters.axis_calls += 4; // the four calls below
        PT_onGamepadAxis(&jni, NULL, AXIS_X, lx, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_Y, ly, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_Z, rx, 0);
        PT_onGamepadAxis(&jni, NULL, AXIS_RZ, ry, 0);
    }
}

void input_poll(void) {
    /* Bug 34 (Build U): while the Vita IME dialog is open it owns
     * the screen and the pad — do not forward touch or buttons to
     * the engine underneath (bug34-analysis.md §7.3). The dialog is
     * serviced by keyboard_service() in main.c; finished text is
     * injected via PT_onKeyDown directly, never through here. */
    if (kb_dialog_open())
        return;
    poll_touch();
    poll_gamepad();
}
