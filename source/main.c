// Pocket Tanks (com.blitwise.ptankshd) PS Vita port — loader entry point.
#include "utils.h"
#include "bootlog.h"
#include "egl_graphics.h"
#include "input.h"
#include "keyboard.h"
#include "fmod_trace.h"

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <kubridge.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_PATH "ux0:data/pockettanks/"

/* from bionic_pthread.c (no header; defined there) */
int bionic_pthread_setspecific(int key, const void *value);
/* from so_util (not in header; defined in lib/so_util/so_util.c) */
uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);
#define LOAD_ADDR_CPP    0x98000000
#define LOAD_ADDR_FMOD   0x98200000
#define LOAD_ADDR_ENGINE 0x99000000

/* Bug 27: encode a Thumb-2 b.w (unconditional wide branch). */
static void encode_bw(uint16_t *out, uintptr_t from, uintptr_t to) {
    int32_t offset = (int32_t)to - (int32_t)(from + 4);
    uint32_t u = (uint32_t)offset;
    int S = (u >> 24) & 1;
    int I1 = (u >> 23) & 1;
    int I2 = (u >> 22) & 1;
    int imm10 = (u >> 12) & 0x3FF;
    int imm11 = (u >> 1) & 0x7FF;
    int J1 = !(I1 ^ S);
    int J2 = !(I2 ^ S);
    out[0] = (uint16_t)(0xF000 | (S << 10) | imm10);
    out[1] = (uint16_t)(0x9000 | (J1 << 13) | (1 << 12) | (J2 << 11) | imm11);
}

/* Bug 28: encode 16-bit Thumb conditional branch (T1).
 * cond: 0=eq, 1=ne, 2=cs/hs, 3=cc/lo. from/to are byte addresses.
 * Verified against arm-vita-eabi-as: bcs.n -> 0xD2xx, bcc/blo.n -> 0xD3xx,
 * bne.n -> 0xD1xx, beq.n -> 0xD0xx, with imm8 = (to-(from+4))>>1
 * (halfword-scaled; e.g. bcs.n from 8 to 0x10 -> 0xD202). */
static void encode_b_cond(uint16_t *out, uintptr_t from, uintptr_t to, int cond) {
    int32_t imm8 = ((int32_t)to - (int32_t)(from + 4)) >> 1;
    out[0] = (uint16_t)(0xD000 | ((cond & 0xF) << 8) | (imm8 & 0xFF));
}

/* Bug 28: encode 16-bit Thumb unconditional branch (T1, b.n).
 * Verified: b.n +2 from 0 -> 0xE001 (imm11 = (to-(from+4))>>1). */
static void encode_b_n(uint16_t *out, uintptr_t from, uintptr_t to) {
    int32_t imm11 = ((int32_t)to - (int32_t)(from + 4)) >> 1;
    out[0] = (uint16_t)(0xE000 | (imm11 & 0x7FF));
}

/* Bug 28: fixed 16-bit Thumb data-processing encodings used by the
 * get_child path-check trampoline. Every halfword verified against
 * arm-vita-eabi-as ground truth (see /tmp/enctest/t.s disassembly):
 *   mov r3, r8        -> 0x4643
 *   ldrb r2, [r3]     -> 0x781A
 *   mov r1, r2        -> 0x4611
 *   lsls r2, r2, #1   -> 0x0052
 *   lsrs r2, r1, #1   -> 0x084A
 *   adds r1, r3, #1   -> 0x1C59
 *   ldr r1, [r3, #8]  -> 0x6899
 *   ldr r2, [r3, #4]  -> 0x685A
 *   cmp r2, #9        -> 0x2A09
 *   ldr r0, [r1]      -> 0x6808
 *   cmp r0, r3        -> 0x4298
 *   push {r3}         -> 0xB408
 *   ldr r0, [r1, #4]  -> 0x6848
 *   pop {r3}          -> 0xBC08
 *   ldrb r0, [r1, #8] -> 0x7A08
 *   cmp r0, #0x3E     -> 0x283E
 *   adds r1, r1, #1   -> 0x3101
 *   subs r2, r2, #1   -> 0x3A01
 *   mov r5, r0        -> 0x4605
 */

/* Bug 27: encode a Thumb-2 cbz/cbnz (T1). from/to are byte addresses,
 * rn is 0-7, is_cbnz selects cbnz (1) vs cbz (0).
 * Verified against arm-vita-eabi-as: cbnz r0,+10 from 0 -> 0xB928, etc. */
static void encode_cbnz(uint16_t *out, uintptr_t from, uintptr_t to, int rn, int is_cbnz) {
    int32_t imm32 = (int32_t)to - (int32_t)(from + 4);
    int i = (imm32 >> 6) & 1;
    int imm5 = (imm32 >> 1) & 0x1F;
    out[0] = (uint16_t)(0xB100 | (is_cbnz << 11) | (i << 9) | (imm5 << 3) | (rn & 7));
}

/* Bug 27: encode Thumb-2 movw (T3). rd 0-15, imm16 0-0xFFFF.
 * Verified: movw r5,#0x1234 -> f241 2534; movw r5,#0x1800 -> f641 0500. */
static void encode_movw(uint16_t *out, int rd, uint32_t imm16) {
    int i = (imm16 >> 11) & 1;
    int imm4 = (imm16 >> 12) & 0xF;
    int imm3 = (imm16 >> 8) & 0x7;
    int imm8 = imm16 & 0xFF;
    out[0] = (uint16_t)(0xF240 | (i << 10) | imm4);
    out[1] = (uint16_t)((imm3 << 12) | ((rd & 0xF) << 8) | imm8);
}

/* Bug 27: encode Thumb-2 movt (T3). rd 0-15, imm16 0-0xFFFF.
 * Verified: movt r5,#0x98ff -> f6c9 05ff. */
static void encode_movt(uint16_t *out, int rd, uint32_t imm16) {
    int i = (imm16 >> 11) & 1;
    int imm4 = (imm16 >> 12) & 0xF;
    int imm3 = (imm16 >> 8) & 0x7;
    int imm8 = imm16 & 0xFF;
    out[0] = (uint16_t)(0xF2C0 | (i << 10) | imm4);
    out[1] = (uint16_t)((imm3 << 12) | ((rd & 0xF) << 8) | imm8);
}

/* Bug 27: real empty ptree builder (ptree_fake.cpp). */
void *ptree_fake_init(void);

/* Bug 29 DIAGNOSTIC (logging only, no policy change): called exclusively
 * from the Bug 27/28 trampoline's fake branch via blx r12, with:
 *   path_obj = the queried path object (r8 at the patch site; the game's
 *              12-byte string layout starts at offset 0 — byte[0] even ->
 *              short (len = byte[0]>>1, data at +1); odd -> long (len at
 *              +4, data ptr at +8). Mirrors the trampoline's own parse.
 *   saved_lr = get_child's saved LR ([r7,#4] at the patch site) = the
 *              direct caller's return address, Thumb bit set.
 *   text_base = libengine text base, for text_base+offset reporting.
 * Build M (cleanup): the FAKE-DIAG logging is removed — this is now a
 * no-op. The trampoline still calls it on the fake branch exactly as
 * before (its bytes are unchanged); it touches no game state, and the
 * trampoline sets r5 = fake ptree itself after this returns. */
static void ptree_fake_diag_log(const void *path_obj, uintptr_t saved_lr,
                                uintptr_t text_base) {
    (void)path_obj;
    (void)saved_lr;
    (void)text_base;
}

/* Bug 32: total graphics_swap() calls made by the frame loop so far.
 * Incremented in main's loop after each graphics_swap(); read by
 * scene_trace_log so every scene-switch request is stamped with the
 * swap count at the moment it was requested (a swap count that stays
 * frozen while other activity continues proves the loader loop never
 * reached the swap again). */
static long long g_swap_count = 0;

/* Bug 32-I: stuck-frame tracer arming flag. Set by scene_trace_log the
 * moment a "TITLE" switch is requested; never cleared afterwards.
 * Read by the dispatch bracket bookkeeping below and tail_mode_check
 * (its one-time constant-mode print). Build M removed its other two
 * readers (the getspecific SPIN capture and the CPJNIHTTP glue log). */
volatile int g_title_trace_armed = 0;

/* Bug 32-J: the frame loop's current frame number, mirrored here so
 * tracer helpers (tail_mode_check) can stamp their lines with it; the
 * loop's own local counter stays the source of truth and is copied in
 * at the top of every iteration. */
static long long g_frame_count = 0;

/* Bug 32-J: frame-tail mode sampling — defined after the mod_engine
 * declaration (it reads engine memory); forward-declared here for
 * dispatch_log_enter. */
static void tail_mode_check(void);

/* Bug 32-L: the conditional-swap fix (bug32-analysis.md 5.3) and its
 * verification counters. The loader surface has exactly 2 display
 * buffers (vglUseTripleBuffering(GL_FALSE) in egl_graphics.c). In the
 * TITLE era the engine presents once per frame from inside
 * onDrawFrame (frame tail -> present wrapper at libengine file offset
 * 0x3d6628 -> eglSwapBuffers), and up to build J main's loop then
 * called graphics_swap() AGAIN, unconditionally: two swaps per
 * iteration pin the draw target to one buffer forever, while the
 * other buffer — still holding frame 0's finished loading screen —
 * is re-queued every iteration and holds the panel between the
 * engine's brief menu presents. Build L counts the engine's
 * presents with an entry hook on the wrapper (see the patch block in
 * main) and the frame loop swaps itself ONLY when the engine
 * presented zero times during that onDrawFrame call.
 *
 * g_conditional_swap is set to 1 ONLY by the present hook's APPLIED
 * branch: if the hook is SKIPPED (site bytes mismatch / no arena),
 * the loop falls back to the old unconditional swap, so the fix can
 * never silently not apply — the patch-time SKIPPED line says so. */
static volatile unsigned g_engine_present_count = 0;
static unsigned g_loader_swap_count = 0;
static unsigned g_skipped_swap_count = 0;
static int g_conditional_swap = 0;

/* Bug 32-L: called exclusively from the present-wrapper trampoline.
 * Counts entries of libengine's present wrapper 0x3d6628 — in the
 * mode-3 era every call reaches eglSwapBuffers (the wrapper early-
 * returns without swapping only when the tail mode is not 3, and it
 * is only ever called from mode-3 contexts: the frame tail and
 * drawBumper, itself mode-gated). Bumper presents during a load
 * inside onDrawFrame count too, which is exactly what the loop's
 * delta check wants: any engine present in the call suppresses the
 * redundant loader swap for that iteration. */
static void present_count_inc(void) {
    g_engine_present_count++;
}

/* Bug 33-R (bug33-analysis.md 38 item 3): push-entry tracer helper,
 * called exclusively from the ring-push trampoline in main with the
 * record pointer (the push's incoming r0) and the caller's return
 * address. Pass 5's fingerprint — 215 type-10 records carrying a
 * tracker sequence (f04 = 0..214) but zero type-8s, on code where a
 * DOWN branch that stamps the sequence pushes its type-8
 * straight-line, unavoidably — is impossible as read, so this
 * watches the push itself: every non-axis record at the ring's only
 * entrance, with the LR that names its producer (0x3d48ea =
 * onTouch DOWN, 0x3d4a40 = onTouch MOVE, 0x3d494a = onTouch UP,
 * 0x3d56fa = onGamepadButton, 0x170708 = the INPUTQ pump copy
 * path). Pushes seen here but missing from the census would break
 * the ring theory; pushes never seen close the case against the
 * handlers. Type 14 (the loader's own axis flood, ~4/poll) is the
 * census's story and is not logged here. Capped at
 * PUSH_TRACE_CAP lines. */
#define PUSH_TRACE_CAP 300
static volatile unsigned g_push_trace_logged = 0;

static void push_trace_log(const void *rec, uint32_t lr) {
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t type, f04, f08;
    uint16_t x, y;
    memcpy(&type, p + 0x00, sizeof(type));
    if (type == 14)
        return;
    if (g_push_trace_logged >= PUSH_TRACE_CAP)
        return;
    g_push_trace_logged++;
    memcpy(&f04, p + 0x04, sizeof(f04));
    memcpy(&f08, p + 0x08, sizeof(f08));
    memcpy(&x, p + 0x0C, sizeof(x));
    memcpy(&y, p + 0x0E, sizeof(y));
    blog("PUSH: type=%d f04=%08x f08=%08x x=%u y=%u lr=0x%08x",
         (int)type, f04, f08, (unsigned)x, (unsigned)y, lr);
}

/* Bug 32-L (section 5.5 item 2, counts-only form): enter/exit
 * counters for the frame tail's render fn (libengine file offset
 * 0x220b80), bracketed by the dispatch-style LR-interception
 * trampoline in main. The render uses r7 as a frame pointer, so the
 * trampoline must execute the displaced prologue at the ORIGINAL
 * stack pointer; the incoming LR therefore cannot be stack-saved
 * across the call and is held on this small C-side stack instead
 * (enter pushes, exit pops and returns it to the exit stub). The
 * render is called once per mode-3 frame from the tail and does not
 * recurse (call census in 5.2), so depth stays 1 in practice; the
 * stack saturates rather than corrupting if that ever changes. */
static volatile unsigned g_render_enter_count = 0;
static volatile unsigned g_render_exit_count = 0;
#define RENDER_LR_STACK_DEPTH 16
static uint32_t g_render_lr_stack[RENDER_LR_STACK_DEPTH];
static int g_render_lr_depth = 0;

static void render_enter_inc(uint32_t lr) {
    g_render_enter_count++;
    if (g_render_lr_depth < RENDER_LR_STACK_DEPTH)
        g_render_lr_stack[g_render_lr_depth] = lr;
    g_render_lr_depth++;
}

static uint32_t render_exit_inc(void) {
    g_render_exit_count++;
    if (g_render_lr_depth > 0)
        g_render_lr_depth--;
    /* Reachable only after a matching enter planted the exit stub
     * in the render's saved-LR slot, so depth >= 1 here by
     * construction; the saturate arm is pure belt-and-braces. */
    if (g_render_lr_depth < RENDER_LR_STACK_DEPTH)
        return g_render_lr_stack[g_render_lr_depth];
    return g_render_lr_stack[RENDER_LR_STACK_DEPTH - 1];
}

/* Bug 33 (Build O; write reverted in Build P): the view-singleton
 * readback, defined after the Build N input-state sampler below
 * (it needs mod_engine). Called from scene_trace_log on EVERY
 * scene arm. */
static void viewstate_arm_log(const char *scene_name);

/* Bug 31 trace: called exclusively from the scene-switch trampoline
 * (see the patch block in main) with the requested scene name — arg1 of
 * libengine's switch function at file offset 0x2a8c3c, a plain C string
 * (the same pointer the game log's "Switching to scene %s" prints).
 * Logs every switch REQUEST, including names the registry search
 * rejects silently; the game log only shows successful switches.
 * Bug 32: the line also carries the loader's swap count at request
 * time (see g_swap_count above). */
static void scene_trace_log(const char *name) {
    /* Bug 32-I: arm the stuck-frame tracer on the TITLE request. The
     * request pends during the frame's update phase, before the frame
     * tail loads TITLE and makes its first dispatch — so the flag is
     * already set when the never-returning dispatch happens. */
    if (name && strcmp(name, "TITLE") == 0)
        g_title_trace_armed = 1;
    blog("SCENE: switch requested -> \"%s\" swaps=%lld",
         name ? name : "(null)", g_swap_count);
    /* Bug 33 (Build O; write reverted in Build P): sample and log
     * the input view singleton on every scene arm — read-only
     * since Build P (bug33-analysis.md 25, 27 item 1). */
    viewstate_arm_log(name);
}

/* Bug 32-I: scene-dispatch bracket helpers, called exclusively from
 * the dispatch trampoline (see the patch block in main).
 *
 * dispatch_log_enter receives the forwarder's incoming r0: a pointer
 * to the scene-pointer cell. It replicates the forwarder's resolution
 * (scene = *cell; target = scene_vtable[+0x18]) and returns the target
 * for the trampoline to call, or 0 when the scene pointer is NULL
 * (the forwarder's cbz no-op case). The exit helper runs after the
 * target returns, via the trampoline's exit stub, and pops the target
 * recorded here so enter/exit lines pair up even under nesting.
 * Bookkeeping is gated on the TITLE arming flag. Build M (cleanup):
 * the DISPATCH enter/exit log lines are removed; the interposition's
 * code path — this helper performing the forwarder's contract, the
 * depth/target bookkeeping, and the exit stub — is byte-for-byte the
 * build L behavior otherwise (bug32-analysis.md 4.4 preserve rule). */
#define DISPATCH_LINE_CAP 200
#define DISPATCH_MAX_DEPTH 16
static uint32_t g_dispatch_targets[DISPATCH_MAX_DEPTH];
static int g_dispatch_depth = 0;
static int g_dispatch_lines = 0;

static uint32_t dispatch_log_enter(uint32_t cell) {
    /* Bug 32-J: both of the tail's dispatch sites flow through this
     * forwarder, so sampling the tail's mode here reads the value
     * that gated this very dispatch. Change-logged; see
     * tail_mode_check. */
    tail_mode_check();
    uint32_t scene = *(const uint32_t *)cell;
    if (!scene)
        return 0;
    uint32_t vtable = *(const uint32_t *)scene;
    uint32_t target = *(const uint32_t *)(vtable + 0x18);
    if (g_title_trace_armed) {
        if (g_dispatch_depth < DISPATCH_MAX_DEPTH)
            g_dispatch_targets[g_dispatch_depth] = target;
        g_dispatch_depth++;
        if (g_dispatch_lines < DISPATCH_LINE_CAP)
            g_dispatch_lines++;
    }
    return target;
}

static void dispatch_log_exit(void) {
    if (!g_title_trace_armed)
        return;
    uint32_t target = 0;
    if (g_dispatch_depth > 0) {
        g_dispatch_depth--;
        if (g_dispatch_depth < DISPATCH_MAX_DEPTH)
            target = g_dispatch_targets[g_dispatch_depth];
    }
    (void)target;
    if (g_dispatch_lines < DISPATCH_LINE_CAP)
        g_dispatch_lines++;
}

int _newlib_heap_size_user = 256 * 1024 * 1024;

static so_module mod_cpp, mod_fmod, mod_engine;

/* Bug 32-J: frame-tail mode sampling. The tail's dispatch block is
 * gated on the engine-state dword at [globals+0x28] — mode 2 is the
 * steady-state arm, mode 3 the switch/loading arm
 * (bug32-analysis.md 4.2). The globals base is derived exactly as
 * the engine's own mode getter (file offset 0x3d64f4) derives it.
 * Build P address audit (bug33-analysis.md 20): for the getter's
 * `ldr r0,[pc,#4]; add r0,pc` pair the PIC base is the RAW
 * (add address + 4) + literal — the add is at 0x3d64f6 (a site
 * congruent to 2 mod 4), the literal (0x52e262) sits at 0x3d64fc,
 * so the base is 0x90475c, NOT the Align(PC,4) result 0x90475a the
 * earlier builds used. The getter then loads [base + 0x28], so
 * the mode cell is 0x904784 (the old derivation read 0x904782,
 * two bytes low — a straddled read of this very dword, which is
 * why bootlog-14 printed 196608 instead of 3). The literal is
 * still read live from the loaded image so the base cannot drift
 * from the binary.
 * tail_mode_check logs every CHANGE of the value and is called once
 * per loader frame (frame boundary) and again inside
 * dispatch_log_enter (the in-frame value that gated the dispatch),
 * so no transition is missed. */
static uintptr_t g_tail_mode_addr = 0;
static int g_tail_mode_last = -1;
static int g_tail_mode_const_printed = 0;

static void tail_mode_check(void) {
    if (!g_tail_mode_addr) {
        uint32_t lit = 0;
        kuKernelCpuUnrestrictedMemcpy(&lit,
            (const void *)(mod_engine.text_base + 0x3d64fc), sizeof(lit));
        g_tail_mode_addr = mod_engine.text_base + 0x3d64f6 + 4 + lit + 0x28;
    }
    uint32_t mode = 0;
    kuKernelCpuUnrestrictedMemcpy(&mode, (const void *)g_tail_mode_addr, sizeof(mode));
    /* Bug 32-L (section 5.5 item 1): print the mode value outright,
     * once, as soon as the TITLE tracer arms — the derivation above
     * lands on [globals+0x28] with globals base at file offset
     * 0x90475c (raw add-address+4 + live literal; cell 0x904784).
     * Build J proved the value constant by the absence of change
     * lines; this closes the mode cell's unknown-writer question
     * by measurement. */
    if (g_title_trace_armed && !g_tail_mode_const_printed) {
        g_tail_mode_const_printed = 1;
        blog("TAIL: mode=%d (constant check)", (int)mode);
    }
    if (g_tail_mode_last < 0) {
        g_tail_mode_last = (int)mode;
        return;
    }
    if ((int)mode != g_tail_mode_last) {
        blog("TAIL: mode %d -> %d at frame %lld",
             g_tail_mode_last, (int)mode, g_frame_count);
        g_tail_mode_last = (int)mode;
    }
}

/* Bug 33 (Build N, v2 in Build P): input-state tracer — read-only
 * sampling of the engine's input machinery, loader-side only
 * (bug33-analysis.md 12, corrected cells per pass 3, sections
 * 19-27). The loader already reads engine BSS directly (the
 * tail-mode print above is the precedent): ELF VA == file offset
 * in libengine, so the runtime address is mod_engine.text_base +
 * VA. Pass 3 proved the pass-1/2 derivations used Align(PC,4)
 * where this binary's PIC pairs use raw (add address + 4) +
 * literal; every cell below is the corrected value, each one
 * re-verified firsthand against the ELF during the Build P audit:
 *   ring state struct 0x904854: count +0x40 (0x904894), read idx
 *     +0x44 (0x904898), write idx +0x48 (0x90489C) — all accessed
 *     with 32-bit ldr/str in push 0x3d4c8c / pop 0x3da134
 *     (unchanged from Build N; runtime-proven coherent).
 *   [0x8D7E40] — the load/transition context pointer (LOADCTX
 *     0x8D7E28 + 0x18); while it is non-null, the tail preprocessor
 *     0x2220a8's mode-3 early-exit returns every touch/key record
 *     "consumed" before the global touch state is updated
 *     (bug33-analysis.md 6). Correctly derived in builds N/O and
 *     measured 0 in every sample — the trap door stands acquitted.
 *   STATE struct 0x8D7D30 (was misderived 0x8D7D2E): Loop B's
 *     drain-gate byte at +0x11 (0x8D7D41) and the preprocessor
 *     sub-handler enable byte at +0x12 (0x8D7D42). Builds N/O
 *     sampled +0x0F/+0x10 (0x8D7D3F/0x8D7D40) by mistake — the
 *     byte that flipped 0->1 during load is STATE+0x0F, not
 *     either gate; the true pair has never been measured before.
 *   input struct 0x8D7EA8 (was misderived 0x8D7EA6 — a non-4-aligned
 *     base for a struct full of doubles): raw record-coordinate
 *     halfwords at +0x2C/+0x2E (0x8D7ED4/0x8D7ED6), the poll pair
 *     at +0x30/+0x34 (0x8D7ED8/0x8D7EDC — builds N/O read two
 *     bytes low, at 0x8D7ED6/0x8D7EDA, and the resulting
 *     "-1.49M / 65535 garbage" was a misaligned-read artifact;
 *     decoded in section 22, the true pair held the letterbox
 *     transform of (0,0) all along), and the double-based position
 *     interface at +0x40/+0x48/+0x50 (0x8D7EE8/0x8D7EF0/0x8D7EF8),
 *     which pass 3 section 24 hypothesis (B) says the widgets may
 *     actually poll (via the core side) instead of the int pair.
 * Sampling NEVER writes engine memory. Lines are emitted at each
 * heartbeat and whenever a sampled value changes, with change
 * lines rate-limited to one per second and capped — the 60 fps
 * era must not recreate the logging flood Build M removed.
 * Section 27 item 3 adds one exception: during the first 600
 * frames the poll pair logs on ANY change with no rate limit, so
 * a tap store that survives only a few frames is still caught. */
#define INPUTSTATE_CHANGE_CAP 300
#define INPUTSTATE_MIN_INTERVAL_US 1000000ULL

typedef struct {
    int32_t   queue_count;
    uint32_t  rd;
    uint32_t  wr;
    uintptr_t gate_ptr;
    uint8_t   gate;      /* STATE+0x11 (0x8D7D41): Loop B drain gate */
    uint8_t   en;        /* STATE+0x12 (0x8D7D42): sub-handler enable */
    uint16_t  raw_x;     /* input +0x2C (0x8D7ED4): raw coord halfword */
    uint16_t  raw_y;     /* input +0x2E (0x8D7ED6): raw coord halfword */
    int32_t   touch_x;   /* input +0x30 (0x8D7ED8): poll pair */
    int32_t   touch_y;   /* input +0x34 (0x8D7EDC): poll pair */
    double    d40;       /* input +0x40 (0x8D7EE8): position doubles */
    double    d48;       /* input +0x48 (0x8D7EF0) */
    double    d50;       /* input +0x50 (0x8D7EF8) */
} inputstate_t;

static void inputstate_sample(inputstate_t *s) {
    uintptr_t base = mod_engine.text_base;
    kuKernelCpuUnrestrictedMemcpy(&s->queue_count,
        (const void *)(base + 0x904894), sizeof(s->queue_count));
    kuKernelCpuUnrestrictedMemcpy(&s->rd,
        (const void *)(base + 0x904898), sizeof(s->rd));
    kuKernelCpuUnrestrictedMemcpy(&s->wr,
        (const void *)(base + 0x90489c), sizeof(s->wr));
    kuKernelCpuUnrestrictedMemcpy(&s->gate_ptr,
        (const void *)(base + 0x8d7e40), sizeof(s->gate_ptr));
    kuKernelCpuUnrestrictedMemcpy(&s->gate,
        (const void *)(base + 0x8d7d41), sizeof(s->gate));
    kuKernelCpuUnrestrictedMemcpy(&s->en,
        (const void *)(base + 0x8d7d42), sizeof(s->en));
    kuKernelCpuUnrestrictedMemcpy(&s->raw_x,
        (const void *)(base + 0x8d7ed4), sizeof(s->raw_x));
    kuKernelCpuUnrestrictedMemcpy(&s->raw_y,
        (const void *)(base + 0x8d7ed6), sizeof(s->raw_y));
    kuKernelCpuUnrestrictedMemcpy(&s->touch_x,
        (const void *)(base + 0x8d7ed8), sizeof(s->touch_x));
    kuKernelCpuUnrestrictedMemcpy(&s->touch_y,
        (const void *)(base + 0x8d7edc), sizeof(s->touch_y));
    kuKernelCpuUnrestrictedMemcpy(&s->d40,
        (const void *)(base + 0x8d7ee8), sizeof(s->d40));
    kuKernelCpuUnrestrictedMemcpy(&s->d48,
        (const void *)(base + 0x8d7ef0), sizeof(s->d48));
    kuKernelCpuUnrestrictedMemcpy(&s->d50,
        (const void *)(base + 0x8d7ef8), sizeof(s->d50));
}

static void inputstate_log(const inputstate_t *s) {
    blog("INPUTSTATE: queue_count=%d rd=%u wr=%u gate_ptr=%p gate=%u en=%u raw_x=%u raw_y=%u touch_x=%d touch_y=%d d40=%f d48=%f d50=%f frame=%lld",
         s->queue_count, s->rd, s->wr, (void *)s->gate_ptr,
         (unsigned)s->gate, (unsigned)s->en,
         (unsigned)s->raw_x, (unsigned)s->raw_y,
         s->touch_x, s->touch_y, s->d40, s->d48, s->d50,
         g_frame_count);
}

static int inputstate_differs(const inputstate_t *a, const inputstate_t *b) {
    return a->queue_count != b->queue_count || a->rd != b->rd ||
           a->wr != b->wr || a->gate_ptr != b->gate_ptr ||
           a->gate != b->gate || a->en != b->en ||
           a->raw_x != b->raw_x || a->raw_y != b->raw_y ||
           a->touch_x != b->touch_x || a->touch_y != b->touch_y ||
           a->d40 != b->d40 || a->d48 != b->d48 || a->d50 != b->d50;
}

/* Bug 33 (Build P): ring census — bug33-analysis.md 27 item 2.
 * Read-only, loader-side. Each frame, in the same post-input_poll
 * sample hook, reconstruct how many records were pushed since the
 * previous sample from the ring state the sampler already reads
 * (count/rd/wr), and read out the newly written records from the
 * ring buffer itself (engine BSS 0x902A50 — resolved firsthand
 * from BOTH push 0x3d4c8c (add @0x3d4cc8 + literal 0x52dd84) and
 * pop 0x3da134 (add @0x3da158 + literal 0x5288f4) during the
 * Build P audit; 256 slots x 20-byte records).
 *
 * Push semantics (0x3d4c8c, disassembled firsthand): the write
 * index runs 0..255 and wraps 255->0 (cmp wr,#254) — NOTE: pass
 * 3's spec text said "mask 0x3F"; the code and bootlog-16's
 * rd/wr values (up to 254) both say the modulus is 256, so the
 * census enumerates mod 256. A push normally advances wr and
 * writes at the new wr; a push into an EMPTY ring (count == 0)
 * writes at the current wr WITHOUT advancing it. It follows that
 * pushes_in_interval = (count - prev_count) + pops_in_interval,
 * where pops = (rd - prev_rd) mod 256, and the difference
 * ew = pushes - (wr - prev_wr mod 256) is the number of
 * empty-ring writes (0, 1, or in the worst case 2: the producer
 * pump's first push before the drain and input_poll's first push
 * after it can both find the ring empty). The written slots are
 * the contiguous run ending at wr: (prev_wr, wr] when ew == 0,
 * [prev_wr, wr] when ew >= 1 — except that when ew == 2 the
 * second empty write lands on the slot the batch's last pre-drain
 * write used, so one pre-drain record is overwritten before it
 * can be read; that loss is counted in `missed`, not hidden.
 * Anything outside these shapes means the sampler missed index
 * wraps; the interval is then resynced (counted in `missed`)
 * rather than enumerated on a guess.
 *
 * Observation-point caveat: records pushed and drained inside
 * the same onDrawFrame (the internal producer pump's batch) are
 * read here one frame later, after pop has copied them out; pop
 * appears to clear the record as it goes, so such slots may read
 * back zeroed — a REC line of all zeros marks a consumed slot
 * position in that case, not necessarily a literal type-0
 * record. Records pushed by input_poll this frame (Lola's taps,
 * drags, button and axis records) are still in the ring,
 * undrained, and read back intact.
 *
 * The first CENSUS_LOG_TOTAL records of the run are logged
 * individually (never more than CENSUS_LOG_PER_FRAME per frame;
 * further records in the same frame are counted in `excess`).
 * Every enumerated record also feeds a per-type histogram that
 * rides the 300-frame heartbeat, so the census keeps counting
 * silently after the individual logging budget is spent.
 *
 * Build Q (bug33-analysis.md 32 item 2): the REC budget above is
 * now reserved for NON-axis records. In build P the type-14 axis
 * flood (~3.16 records/frame, pass 4 section 29) burned all 300
 * REC lines before a single type-10 copy-path record was ever
 * printed, so the producer behind those 333 records stayed
 * anonymous. Axis records still feed the histogram and total
 * exactly as before — they simply never print a REC line and
 * never consume budget or a per-frame slot, and `logged` in the
 * HIST line therefore counts REC lines printed (all non-axis).
 * Every drained non-axis record prints with all its fields
 * while the reserved budget lasts. */
#define RING_BUFFER_VA 0x902a50
#define RING_REC_SIZE 20
#define CENSUS_LOG_TOTAL 300
#define CENSUS_LOG_PER_FRAME 8
#define CENSUS_TYPE_BUCKETS 32
#define CENSUS_AXIS_TYPE 14

static uint32_t g_census_hist[CENSUS_TYPE_BUCKETS];
static uint32_t g_census_hist_other = 0;
static unsigned long long g_census_total = 0;
static unsigned long long g_census_excess = 0;
static unsigned long long g_census_missed = 0;
static unsigned g_census_logged = 0;
static int g_census_have_prev = 0;
static int32_t g_census_prev_count = 0;
static uint32_t g_census_prev_rd = 0;
static uint32_t g_census_prev_wr = 0;

static void ring_census_update(const inputstate_t *cur) {
    if (!g_census_have_prev) {
        g_census_prev_count = cur->queue_count;
        g_census_prev_rd = cur->rd;
        g_census_prev_wr = cur->wr;
        g_census_have_prev = 1;
        return;
    }
    uint32_t pops = (cur->rd - g_census_prev_rd) & 0xff;
    long pushes = (long)cur->queue_count - (long)g_census_prev_count +
                  (long)pops;
    uint32_t delta_wr = (cur->wr - g_census_prev_wr) & 0xff;
    long ew = pushes - (long)delta_wr;
    if (pushes < 0 || pushes > 220 || ew < 0 || ew > 2) {
        if (pushes > 0 && pushes < 100000)
            g_census_missed += (unsigned long long)pushes;
        g_census_prev_count = cur->queue_count;
        g_census_prev_rd = cur->rd;
        g_census_prev_wr = cur->wr;
        return;
    }
    /* Slots to read: the contiguous run of `n` slots ending at
     * the current wr (inclusive). */
    long n = (long)delta_wr + (ew >= 1 ? 1 : 0);
    g_census_missed += (unsigned long long)(pushes - n);
    uint32_t start = (cur->wr - (uint32_t)n + 1) & 0xff;
    uintptr_t base = mod_engine.text_base;
    int logged_this_frame = 0;
    for (long i = 0; i < n; i++) {
        uint32_t idx = (start + (uint32_t)i) & 0xff;
        const uint8_t *rec =
            (const uint8_t *)(base + RING_BUFFER_VA + idx * RING_REC_SIZE);
        uint32_t type, f04, f08;
        uint16_t x, y;
        kuKernelCpuUnrestrictedMemcpy(&type, rec, sizeof(type));
        kuKernelCpuUnrestrictedMemcpy(&f04, rec + 0x04, sizeof(f04));
        kuKernelCpuUnrestrictedMemcpy(&f08, rec + 0x08, sizeof(f08));
        kuKernelCpuUnrestrictedMemcpy(&x, rec + 0x0c, sizeof(x));
        kuKernelCpuUnrestrictedMemcpy(&y, rec + 0x0e, sizeof(y));
        if (type < CENSUS_TYPE_BUCKETS)
            g_census_hist[type]++;
        else
            g_census_hist_other++;
        g_census_total++;
        /* Build Q: axis records (type 14) skip the REC log
         * entirely — the budget belongs to the other types. */
        if (type != CENSUS_AXIS_TYPE &&
            g_census_logged < CENSUS_LOG_TOTAL &&
            logged_this_frame < CENSUS_LOG_PER_FRAME) {
            blog("REC: type=%d f04=%08x f08=%08x x=%u y=%u",
                 (int)type, f04, f08, (unsigned)x, (unsigned)y);
            g_census_logged++;
            logged_this_frame++;
        } else if (type != CENSUS_AXIS_TYPE &&
                   g_census_logged < CENSUS_LOG_TOTAL) {
            g_census_excess++;
        }
    }
    g_census_prev_count = cur->queue_count;
    g_census_prev_rd = cur->rd;
    g_census_prev_wr = cur->wr;
}

/* The per-type histogram, emitted with the 300-frame heartbeat:
 * cumulative counts of every record the census has enumerated
 * since boot (including the individually logged ones). */
static void ring_census_heartbeat(void) {
    char buf[512];
    int off = snprintf(buf, sizeof(buf),
        "HIST: total=%llu logged=%u excess=%llu missed=%llu |",
        g_census_total, g_census_logged,
        g_census_excess, g_census_missed);
    for (int t = 0; t < CENSUS_TYPE_BUCKETS && off > 0 &&
         off < (int)sizeof(buf); t++) {
        if (g_census_hist[t])
            off += snprintf(buf + off, sizeof(buf) - off, " %d:%u",
                            t, g_census_hist[t]);
    }
    if (g_census_hist_other && off > 0 && off < (int)sizeof(buf))
        snprintf(buf + off, sizeof(buf) - off, " other:%u",
                 g_census_hist_other);
    blog("%s", buf);
}

/* Bug 33 (Build Q, bug33-analysis.md 32 item 1): the loader-side
 * poll counters from input.c, emitted with the 300-frame
 * heartbeat. Cumulative since boot. Read against the census:
 * peek errors climbing or touch_reports flat while Lola touches
 * = the Vita never told the poller; reports/send/btn counters
 * moving while the HIST census shows no type 8/9/12/13 = the
 * poller forwarded but nothing queued (the follow-up's
 * push-entry trampoline territory); reports flat but peek_last
 * healthy = her touches never reached the front panel poll at
 * all. axis_calls is the control group: it advances ~4/frame
 * whenever poll_gamepad runs, proving the poll itself is alive. */
static void poll_counters_heartbeat(void) {
    blog("POLL: touch_reports=%u send_down=%u send_move=%u send_up=%u send_ptr=%u btn_calls=%u btn_press=%u btn_release=%u axis_calls=%u touch_peek_err=%u touch_peek_last=%d ctrl_peek_err=%u ctrl_peek_last=%d",
         g_input_poll_counters.touch_reports,
         g_input_poll_counters.send_down,
         g_input_poll_counters.send_move,
         g_input_poll_counters.send_up,
         g_input_poll_counters.send_ptr,
         g_input_poll_counters.btn_calls,
         g_input_poll_counters.btn_press,
         g_input_poll_counters.btn_release,
         g_input_poll_counters.axis_calls,
         g_input_poll_counters.touch_peek_err,
         g_input_poll_counters.touch_peek_last,
         g_input_poll_counters.ctrl_peek_err,
         g_input_poll_counters.ctrl_peek_last);
}

static inputstate_t g_inputstate_logged;
static int g_inputstate_have_logged = 0;
static unsigned g_inputstate_change_lines = 0;
static uint64_t g_inputstate_last_change_us = 0;
static int32_t g_inputstate_prev_pair_x = 0;
static int32_t g_inputstate_prev_pair_y = 0;
static int g_inputstate_have_prev_pair = 0;

/* Called once per loader frame, after input_poll() (so the queue
 * fields show the records input_poll just queued, pre-drain — the
 * readout bug33-analysis.md 12 expects) and before onDrawFrame.
 * Logs only when a sampled value differs from the last logged
 * snapshot; the first sample always logs as the baseline. Build P
 * (bug33-analysis.md 27 item 3): during the first 600 frames a
 * change of the poll pair alone logs immediately, with no rate
 * limit — a tap store that survives only a few frames would
 * otherwise fall between the 1 Hz change lines. The ring census
 * rides the same hook and runs unconditionally every frame. */
static void inputstate_check(void) {
    inputstate_t cur;
    inputstate_sample(&cur);
    ring_census_update(&cur);
    int pair_changed = g_inputstate_have_prev_pair &&
        (cur.touch_x != g_inputstate_prev_pair_x ||
         cur.touch_y != g_inputstate_prev_pair_y);
    g_inputstate_prev_pair_x = cur.touch_x;
    g_inputstate_prev_pair_y = cur.touch_y;
    g_inputstate_have_prev_pair = 1;
    if (g_frame_count < 600 && pair_changed &&
        g_inputstate_change_lines < INPUTSTATE_CHANGE_CAP) {
        inputstate_log(&cur);
        g_inputstate_logged = cur;
        g_inputstate_have_logged = 1;
        g_inputstate_change_lines++;
        g_inputstate_last_change_us = sceKernelGetProcessTimeWide();
        return;
    }
    if (g_inputstate_have_logged &&
        !inputstate_differs(&cur, &g_inputstate_logged))
        return;
    if (g_inputstate_change_lines >= INPUTSTATE_CHANGE_CAP)
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (g_inputstate_have_logged &&
        now - g_inputstate_last_change_us < INPUTSTATE_MIN_INTERVAL_US)
        return;
    inputstate_log(&cur);
    g_inputstate_logged = cur;
    g_inputstate_have_logged = 1;
    g_inputstate_change_lines++;
    g_inputstate_last_change_us = now;
}

/* Heartbeat emission: a post-frame snapshot with the same fields
 * (queue fields here are the post-drain residue). Does not touch
 * the change-detection state. */
static void inputstate_heartbeat(void) {
    inputstate_t cur;
    inputstate_sample(&cur);
    inputstate_log(&cur);
}

/* Bug 33: the view singleton — READ-ONLY since Build P
 * (bug33-analysis.md 13-27). In mode 3 the tail preprocessor
 * 0x2220a8 passes every polled touch coordinate through transform
 * 0x2ad01c, out = (in - origin) / scale, reading origin/scale as
 * doubles from the global view object at libengine VA 0x7FE648
 * (+0x48/+0x50 origin, +0x68/+0x70 scale).
 *
 * Build O wrote an identity transform (origin 0, scale 1) into
 * that singleton on every scene arm, on pass 2's theory that the
 * values were unregistered garbage. Pass 3 REFUTED the theory:
 * the "-1.49M / 65535 garbage" was our own sampler reading the
 * touch pair two bytes low (sections 19-22), and bootlog-17's
 * VIEWSTATE lines show the singleton holding exactly the values
 * it should: the engine's logical space is 800x480, the surface
 * is 960x544, and origin (27.1667, 0.5) / scale 1.13333 is the
 * fit-height letterbox for that pairing. The letterbox is
 * correct by design — the preprocessor's outputs are
 * logical-space coordinates and hit-testing is in logical space;
 * identity would have mis-scaled every hit by 1.1333x and offset
 * it. Verdict (section 25): REVERT the write. It is removed in
 * this build; the write was also proven to feed only the resting
 * pair's derivation, never the menu's behaviour (build O's run
 * was unchanged: "still just one tap").
 *
 * What stays is the readback: viewstate_arm_log samples and logs
 * the singleton on every scene arm (from scene_trace_log), and
 * viewstate_check rides the INPUTSTATE sampler's cadence (once
 * per frame, change-only, rate-limited, capped) so an engine-side
 * re-derivation between arms shows up in the log on its own.
 * Together they are the cheapest regression check that the
 * singleton stays intact (sections 25, 27 item 1). */
#define VIEW_SINGLETON_VA 0x7fe648
#define VIEWSTATE_CHANGE_CAP 120

typedef struct {
    int32_t w;
    int32_t h;
    double  ratio;
    double  ox, oy;
    double  sx, sy;
} viewstate_t;

static void viewstate_sample(viewstate_t *v) {
    uintptr_t p = mod_engine.text_base + VIEW_SINGLETON_VA;
    kuKernelCpuUnrestrictedMemcpy(&v->w,
        (const void *)(p + 0x1c), sizeof(v->w));
    kuKernelCpuUnrestrictedMemcpy(&v->h,
        (const void *)(p + 0x20), sizeof(v->h));
    kuKernelCpuUnrestrictedMemcpy(&v->ratio,
        (const void *)(p + 0x28), sizeof(v->ratio));
    kuKernelCpuUnrestrictedMemcpy(&v->ox,
        (const void *)(p + 0x48), sizeof(v->ox));
    kuKernelCpuUnrestrictedMemcpy(&v->oy,
        (const void *)(p + 0x50), sizeof(v->oy));
    kuKernelCpuUnrestrictedMemcpy(&v->sx,
        (const void *)(p + 0x68), sizeof(v->sx));
    kuKernelCpuUnrestrictedMemcpy(&v->sy,
        (const void *)(p + 0x70), sizeof(v->sy));
}

static void viewstate_log(const viewstate_t *v) {
    blog("VIEWSTATE: w=%d h=%d ratio=%f o=(%f,%f) s=(%f,%f)",
         v->w, v->h, v->ratio, v->ox, v->oy, v->sx, v->sy);
}

static viewstate_t g_viewstate_logged;
static int g_viewstate_have_logged = 0;
static unsigned g_viewstate_change_lines = 0;
static uint64_t g_viewstate_last_change_us = 0;

/* Build P (bug33-analysis.md 25, 27 item 1): Build O's identity
 * write is REVERTED — this function only reads and logs now. The
 * sampler's baseline becomes the arm-time state, so viewstate_check's
 * change lines afterwards report only engine-side re-derivations
 * after this arm. */
static void viewstate_arm_log(const char *scene_name) {
    (void)scene_name;
    viewstate_t cur;
    viewstate_sample(&cur);
    viewstate_log(&cur);
    g_viewstate_logged = cur;
    g_viewstate_have_logged = 1;
}

static int viewstate_differs(const viewstate_t *a, const viewstate_t *b) {
    return a->w != b->w || a->h != b->h || a->ratio != b->ratio ||
           a->ox != b->ox || a->oy != b->oy ||
           a->sx != b->sx || a->sy != b->sy;
}

/* Called once per loader frame next to inputstate_check; logs a
 * VIEWSTATE line only when the singleton's fields move away from the
 * last logged snapshot (same 1 s rate limit and change cap as the
 * INPUTSTATE sampler). */
static void viewstate_check(void) {
    viewstate_t cur;
    viewstate_sample(&cur);
    if (g_viewstate_have_logged &&
        !viewstate_differs(&cur, &g_viewstate_logged))
        return;
    if (g_viewstate_change_lines >= VIEWSTATE_CHANGE_CAP)
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (g_viewstate_have_logged &&
        now - g_viewstate_last_change_us < INPUTSTATE_MIN_INTERVAL_US)
        return;
    viewstate_log(&cur);
    g_viewstate_logged = cur;
    g_viewstate_have_logged = 1;
    g_viewstate_change_lines++;
    g_viewstate_last_change_us = now;
}

/* Bug 37 (Build X; bug37-analysis.md 5.1): scene-request EXIT trace,
 * registry roster dump, and burst latch readout. Diagnostics only.
 *
 * Pass 1's verdict: SHOP's per-frame update requests "GAME" ~0.49x/
 * frame forever because the request fn 0x2a8c3c silently returns 0 —
 * "GAME" was never registered, so the pending latch [mgr+4] is never
 * set and SHOP stays current. The Bug 31 entry hook logs requests
 * but never outcomes; this build adds the missing half:
 *
 * (1) An exit hook on the request fn's SHARED exit (file offset
 *     0x2a8ca6: `ldmia.w sp!, {r8, r9, sl}` then
 *     `pop {r4, r5, r6, r7, pc}` — the found path branches here from
 *     0x2a8ca2 after `movs r0, #1`, the not-found path falls through
 *     0x2a8ca4's `movs r0, #0`). At that point r0 = return value,
 *     r9 = mgr, sl = the requested name pointer: everything the
 *     outcome line needs is live in registers, so no entry/exit
 *     correlation state is required.
 * (2) A one-time-per-trigger dump of the whole scene registry
 *     (entry-pointer array at VA 0x8EE910, count at 0x8EED10, name
 *     chars inline at entry+4): fired from scene_exit_log at the
 *     first successful "SHOP" request (its arm) and at the first
 *     "GAME" request, whichever each comes first.
 * (3) scene_latch_heartbeat rides the frame loop's 300-frame
 *     heartbeat while a requested name keeps failing AND stays
 *     unregistered: pending [mgr+4] / current [mgr+0] sampled
 *     through the manager singleton cell at VA 0x701DE0, plus the
 *     completer's latch-gate byte at VA 0x701DF5. A failure that
 *     ages past 300 frames, or whose name registers, stops the
 *     line on its own.
 *
 * Outcome logging policy: the first SCENE_REQ_STRAIGHT_CAP requests
 * log unconditionally; past that, a request logs only when its
 * (retval, pending, current) tuple differs from the last logged
 * one — a GAME burst is one unchanging tuple, so the bootlog stays
 * readable, while every state change (a latch, a completed switch)
 * still prints. */
#define SCENE_REGISTRY_ARRAY_VA  0x8EE910
#define SCENE_REGISTRY_COUNT_VA  0x8EED10
#define SCENE_REGISTRY_CAPACITY  255
#define SCENE_MGR_SINGLETON_VA   0x701DE0
#define SCENE_LATCH_BYTE_VA      0x701DF5
#define SCENE_REQ_STRAIGHT_CAP   40

static volatile unsigned g_scene_req_total = 0;
static int g_scene_last_ret = -1;
static uint32_t g_scene_last_pending = 0;
static uint32_t g_scene_last_current = 0;
static volatile unsigned g_game_req_total = 0;
static volatile unsigned g_game_req_failed = 0;
static int g_roster_dumped_shop = 0;
static int g_roster_dumped_game = 0;
static int g_unreg_active = 0;
static char g_unreg_name[48] = "";
static long long g_unreg_last_fail_frame = 0;

/* Bounded copy of an engine scene name (the request fn strcmp's the
 * chars at entry+4 directly, so plain C-string layout; bounded
 * anyway — a corrupt entry must not run the sampler away). */
static void scene_name_copy(char *dst, const void *src) {
    kuKernelCpuUnrestrictedMemcpy(dst, src, 47);
    dst[47] = '\0';
}

static void scene_registry_dump(const char *trigger) {
    uintptr_t tb = mod_engine.text_base;
    uint32_t count = 0;
    kuKernelCpuUnrestrictedMemcpy(&count,
        (const void *)(tb + SCENE_REGISTRY_COUNT_VA), sizeof(count));
    uint32_t n = count;
    if (n > SCENE_REGISTRY_CAPACITY)
        n = SCENE_REGISTRY_CAPACITY;
    blog("ROSTER: begin count=%u trigger=\"%s\"", n, trigger);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t entry = 0;
        kuKernelCpuUnrestrictedMemcpy(&entry,
            (const void *)(tb + SCENE_REGISTRY_ARRAY_VA + 4u * i),
            sizeof(entry));
        char nm[48];
        nm[0] = '\0';
        if (entry)
            scene_name_copy(nm, (const void *)(uintptr_t)(entry + 4));
        blog("ROSTER: [%u] \"%s\" obj=%p", i, entry ? nm : "(null)",
             (void *)(uintptr_t)entry);
    }
    blog("ROSTER: end");
    /* Bug 37 (Build Y; bug37-analysis.md pass 2): probe the GAME
     * scene's static object (engine VA 0x7210E8). Statically, its
     * derived ctor (0x1318d4) is called only from the init fn at
     * 0x858b0 (.init_array index 82, bracketed by working inits) and
     * would leave: vtable at +0, "GAME" at +4, byte 1 at +0x141.
     * Untouched BSS reads as zeros, so this read-only probe settles
     * whether the ctor ever ran — no load-time hook could (the
     * init_array runs during so_load, before any patch installs).
     * SHOP's object (VA 0x7309B4, roster-proven) is the control. */
    {
        uint8_t ghead[16], shead[16];
        uint32_t gvt = 0, svt = 0;
        uint8_t gflag = 0;
        kuKernelCpuUnrestrictedMemcpy(ghead,
            (const void *)(tb + 0x7210E8u), sizeof(ghead));
        kuKernelCpuUnrestrictedMemcpy(&gflag,
            (const void *)(tb + 0x7210E8u + 0x141u), sizeof(gflag));
        kuKernelCpuUnrestrictedMemcpy(shead,
            (const void *)(tb + 0x7309B4u), sizeof(shead));
        memcpy(&gvt, ghead, sizeof(gvt));
        memcpy(&svt, shead, sizeof(svt));
        blog("GAMEOBJ: game vtbl=%08x name=\"%s\" flag141=%u | shop vtbl=%08x name=\"%s\"",
             gvt, (const char *)(ghead + 4), (unsigned int)gflag,
             svt, (const char *)(shead + 4));
    }
}

static int scene_registry_has(const char *name) {
    uintptr_t tb = mod_engine.text_base;
    uint32_t count = 0;
    kuKernelCpuUnrestrictedMemcpy(&count,
        (const void *)(tb + SCENE_REGISTRY_COUNT_VA), sizeof(count));
    if (count > SCENE_REGISTRY_CAPACITY)
        count = SCENE_REGISTRY_CAPACITY;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = 0;
        kuKernelCpuUnrestrictedMemcpy(&entry,
            (const void *)(tb + SCENE_REGISTRY_ARRAY_VA + 4u * i),
            sizeof(entry));
        if (!entry)
            continue;
        char nm[48];
        scene_name_copy(nm, (const void *)(uintptr_t)(entry + 4));
        if (strcmp(nm, name) == 0)
            return 1;
    }
    return 0;
}

/* Called exclusively from the Bug 37 exit trampoline at the request
 * fn's shared exit: retval = the fn's r0, mgr = its r9, name = its
 * sl (the requested scene name, still live at the exit). */
static void scene_exit_log(uint32_t retval, uint32_t mgr,
                           const char *name) {
    uint32_t pending = 0, current = 0;
    if (mgr) {
        kuKernelCpuUnrestrictedMemcpy(&current,
            (const void *)(uintptr_t)mgr, sizeof(current));
        kuKernelCpuUnrestrictedMemcpy(&pending,
            (const void *)(uintptr_t)(mgr + 4), sizeof(pending));
    }
    g_scene_req_total++;
    int is_game = (name && strcmp(name, "GAME") == 0);
    if (is_game) {
        g_game_req_total++;
        if (!retval)
            g_game_req_failed++;
    }
    /* Roster dumps: at the first "GAME" request, and at the first
     * successful "SHOP" request (its arm) if that comes earlier. */
    if (is_game && !g_roster_dumped_game) {
        g_roster_dumped_game = 1;
        scene_registry_dump("first \"GAME\" request");
    } else if (name && retval && !g_roster_dumped_shop &&
               strcmp(name, "SHOP") == 0) {
        g_roster_dumped_shop = 1;
        scene_registry_dump("first \"SHOP\" arm");
    }
    /* Burst tracking for the LATCH heartbeat line. */
    if (!retval && name) {
        size_t k = 0;
        while (k < sizeof(g_unreg_name) - 1 && name[k]) {
            g_unreg_name[k] = name[k];
            k++;
        }
        g_unreg_name[k] = '\0';
        g_unreg_active = 1;
        g_unreg_last_fail_frame = g_frame_count;
    }
    /* Outcome line: straight cap, then state changes only. */
    int changed = ((int)retval != g_scene_last_ret) ||
                  pending != g_scene_last_pending ||
                  current != g_scene_last_current;
    if (g_scene_req_total <= SCENE_REQ_STRAIGHT_CAP || changed) {
        blog("SCENE: request \"%s\" -> %d (pending=%p current=%p)",
             name ? name : "(null)", (int)retval,
             (void *)(uintptr_t)pending, (void *)(uintptr_t)current);
        g_scene_last_ret = (int)retval;
        g_scene_last_pending = pending;
        g_scene_last_current = current;
    }
}

/* Rides the frame loop's 300-frame heartbeat. Prints only while a
 * requested name is still failing recently AND still unregistered;
 * self-terminates when the failure ages out or the name registers. */
static void scene_latch_heartbeat(void) {
    if (!g_unreg_active)
        return;
    if (g_frame_count - g_unreg_last_fail_frame > 300) {
        g_unreg_active = 0;
        return;
    }
    if (scene_registry_has(g_unreg_name)) {
        g_unreg_active = 0;
        return;
    }
    uintptr_t tb = mod_engine.text_base;
    uint32_t mgr = 0, pending = 0, current = 0;
    uint8_t latch = 0;
    kuKernelCpuUnrestrictedMemcpy(&mgr,
        (const void *)(tb + SCENE_MGR_SINGLETON_VA), sizeof(mgr));
    if (mgr) {
        kuKernelCpuUnrestrictedMemcpy(&current,
            (const void *)(uintptr_t)mgr, sizeof(current));
        kuKernelCpuUnrestrictedMemcpy(&pending,
            (const void *)(uintptr_t)(mgr + 4), sizeof(pending));
    }
    kuKernelCpuUnrestrictedMemcpy(&latch,
        (const void *)(tb + SCENE_LATCH_BYTE_VA), sizeof(latch));
    blog("LATCH: pending=%p current=%p latch=0x%02x unreg=\"%s\" game_reqs=%u game_failed=%u",
         (void *)(uintptr_t)pending, (void *)(uintptr_t)current,
         (unsigned)latch, g_unreg_name, g_game_req_total,
         g_game_req_failed);
}

/* Bug 29 EH round 3: the EH-READBACK readback logger that lived here
 * was removed in Build M (cleanup, diagnostics off). The Find_exidx
 * containment wrapper itself (so_util) is unchanged. */

// Native entry points (Java_com_blitwise_engine_jni_CPJNILib_*)
static void (*N_onCreate)(void *env, void *clazz);
static void (*N_onSurfaceCreated)(void *env, void *clazz);
static void (*N_onSurfaceChanged)(void *env, void *clazz, int w, int h);
static int (*N_onDrawFrame)(void *env, void *clazz);
static void (*N_setAcceleration)(void *env, void *clazz, int enabled, float x, float y, float z);
static void (*N_onPause)(void *env, void *clazz, int b);
static void (*N_onResume)(void *env, void *clazz);

extern so_default_dynlib default_dynlib[];
extern const int default_dynlib_size;
extern void bionic_sF_init(void);
// Bug 17: exception-throw interposers (implemented in bionic_compat.c).
void __cxa_throw_soloader(void *ex, void *tinfo, void (*dest)(void *));
void __cxa_rethrow_soloader(void);
void cxa_throw_hook_init(void *real_throw, void *real_rethrow,
                         void *cur_exception_type);

// Bug 17: overwrite one of the module's GOT slots (JUMP_SLOT/GLOB_DAT) for
// an imported symbol with our replacement, returning the previous target.
// Used for __cxa_throw/__cxa_rethrow: the game binds those from its own
// libc++_shared.so via DT_NEEDED, which wins over our fallback table, so
// direct GOT patching post-resolution is the only interception point.
static void *patch_got_import(so_module *mod, const char *symname, void *replacement) {
    for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i]
                                            : &mod->relplt[i - mod->num_reldyn];
        int type = ELF32_R_TYPE(rel->r_info);
        if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT)
            continue;
        Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        if (sym->st_shndx != SHN_UNDEF)
            continue;
        if (strcmp(mod->dynstr + sym->st_name, symname) != 0)
            continue;
        uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
        void *real = (void *)*slot;
        blog("main: GOT patch %s: slot=%p real=%p -> hook=%p",
             symname, (void *)slot, real, replacement);
        kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof(replacement));
        return real;
    }
    blog("main: GOT patch %s: import not found", symname);
    return NULL;
}

// Per-initializer logger for so_initialize: pinpoints which static
// initializer crashes (see bootlog).
static void init_logger(int idx, int total, uintptr_t addr) {
    if (idx <= -1000)
        blog("init: [%d/%d] SKIPPED (diagnostic skip list) %p", -1000 - idx, total, (void *)addr);
    else
        blog("init: [%d/%d] calling %p", idx, total, (void *)addr);
}

static void *get_sym(so_module *mod, const char *name, int required) {
    void *p = (void *)so_symbol(mod, name);
    if (!p && required) {
        blog("get_sym: REQUIRED export missing: %s", name);
        fatal_error("Missing required native export: %s", name);
    }
    if (!p)
        blog("get_sym: optional export not found: %s", name);
    else
        blog("get_sym: %s -> %p", name, p);
    return p;
}

// List the data dir early: if her game data is missing/misplaced we see it
// immediately instead of guessing.
static void log_data_dir(void) {
    blog("datadir: listing " DATA_PATH);
    SceUID dfd = sceIoDopen(DATA_PATH);
    if (dfd < 0) {
        blog("datadir: sceIoDopen FAILED: 0x%08x", dfd);
        return;
    }
    SceIoDirent de;
    int count = 0;
    while (sceIoDread(dfd, &de) > 0) {
        blog("datadir: [%s] size=%d", de.d_name, (int)de.d_stat.st_size);
        count++;
        if (count > 60) { blog("datadir: ... (truncated)"); break; }
    }
    sceIoDclose(dfd);
    blog("datadir: %d entries total", count);

    const char *need[] = {
        "libengine.so", "libfmod.so", "libc++_shared.so",
        "libsqlcipher.so", "assets", NULL
    };
    for (int i = 0; need[i]; i++) {
        char p[256];
        snprintf(p, sizeof(p), DATA_PATH "%s", need[i]);
        blog("datadir: %-16s %s", need[i],
             file_exists(p) ? "present" : "MISSING");
    }
}

/* ------------------------------------------------------------------ */
/* Bug 34 (Build U): keyboard service — the Vita IME bridge
 * (bug34-analysis.md §7.3/§7.4). The JNI handler for
 * setKeyboardVisibility only raises kb_show_pending (it runs
 * mid-drain inside onDrawFrame); this service runs once per frame
 * from the main loop, after input_poll and before N_onDrawFrame. It
 * opens SceImeDialog when the engine has asked for the keyboard,
 * polls its status non-blockingly (the render loop never waits on
 * it), and on ENTER injects the finished text into the engine
 * through onKeyDown character calls — the only text path the engine
 * has. While the dialog is open, input_poll is suppressed (input.c)
 * and the engine keeps rendering/presenting underneath, which is
 * the cadence CommonDialog expects. onKeyUp is a native no-op and
 * is deliberately never resolved or called. */
static int kb_ime_state = 0; /* 0 = IDLE, 1 = OPEN */
/* Bug 40 round 3: HCR's exact buffer arrangement — writable,
 * dialog-max-sized buffers for title/initial/input (ours were
 * 64-wide with a const title). */
static SceWChar16 kb_ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceWChar16 kb_ime_name[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceWChar16 kb_ime_title[] = {
    'P','l','a','y','e','r',' ','N','a','m','e', 0
};
static int kb_msg_probe_state = 0; /* msg-dialog probe showing */
static int kb_msg_probe_tried = 0; /* once per boot is enough */

static int kb_ime_module_ready(void) {
    static int state = 0; /* 0 = untried, 1 = ready, -1 = unavailable */
    if (state == 0) {
        if (sceSysmoduleIsLoaded(SCE_SYSMODULE_IME)) {
            state = 1;
        } else {
            int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
            blog("KB: sceSysmoduleLoadModule(IME) -> 0x%x", rc);
            state = (rc >= 0 || sceSysmoduleIsLoaded(SCE_SYSMODULE_IME))
                        ? 1 : -1;
        }
        if (state < 0)
            blog("KB: IME sysmodule unavailable - keyboard stays closed");
    }
    return state > 0;
}

/* The §7.4 injection protocol, mirroring the stock Java listener's
 * order: characters while the session is live, session flag drops,
 * then Done (10). */
static void kb_deliver_text(const SceWChar16 *text) {
    if (!PT_onKeyDown) {
        blog("KB: deliver skipped - PT_onKeyDown unresolved");
        kb_set_active(0);
        return;
    }
    /* Remember the entered name as the next session's initialText. */
    kb_ime_name[0] = 0;
    for (int i = 0; i < 63 && text[i]; i++) {
        kb_ime_name[i] = text[i];
        kb_ime_name[i + 1] = 0;
    }
    /* 1. Clear the field. Its pre-filled content is not readable
     * from the loader, so clear deterministically: 24 backspaces
     * (backspace on empty is a no-op in the widget). */
    for (int i = 0; i < 24; i++)
        PT_onKeyDown(&jni, NULL, 8);
    /* 2. The text itself: printable ASCII as-is (the engine's table
     * folds case itself, §5); skip anything outside 0x20..0x7E. */
    int nchars = 0;
    for (int i = 0; i < 63 && text[i]; i++) {
        unsigned int c = text[i];
        if (c < 0x20 || c > 0x7E)
            continue;
        PT_onKeyDown(&jni, NULL, (int)c);
        nchars++;
    }
    /* 3. Done: drop the active flag first, then code 10 — the exact
     * stock sequence (Java hides the keyboard, then forwards 10). */
    kb_set_active(0);
    PT_onKeyDown(&jni, NULL, 10);
    blog("KB: delivered %d char(s) + Done(10)", nchars);
}

static void keyboard_service(void) {
    if (kb_msg_probe_state) {
        if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            sceMsgDialogTerm();
            kb_msg_probe_state = 0;
            blog("KB: msg-dialog probe finished + termed");
        }
        return;
    }
    if (kb_ime_state == 0) { /* IDLE */
        kb_clear_abort(); /* a hide with no dialog open is a no-op */
        if (!kb_show_pending())
            return;
        kb_consume_show();
        if (!kb_ime_module_ready())
            return; /* keyboard unavailable: game stays as playable
                       as build T outside the name field */
        SceImeDialogParam param;
        sceImeDialogParamInit(&param);
        /* Bug 40: working references (SDK sample, HCR's dialog.c)
         * both set the language mask; ours was ParamInit's zero,
         * which reads as "no languages" to the dialog. */
        param.supportedLanguages = 0x0001FFFF;
        param.languagesForced = SCE_TRUE;
        param.type = SCE_IME_TYPE_BASIC_LATIN; /* a Latin name field */
        /* Bug 40 bisect, round 2 (bootlog-33): the Bug 40 prereqs
         * (AppUtil init + dialog config, languages) all succeeded
         * and init STILL returned 0x800204c4. Both working
         * references leave the modes at ParamInit defaults, and the
         * HCR port sets the common-param magic explicitly on its
         * dialog params — apply both differences here. */
        param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_DEFAULT;
        param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
        _sceCommonDialogSetMagicNumber(&param.commonParam);
        param.title = kb_ime_title;
        param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
        param.initialText = kb_ime_name;
        param.inputTextBuffer = kb_ime_text;
        memset(kb_ime_text, 0, sizeof(kb_ime_text));
        int rc = sceImeDialogInit(&param);
        blog("KB: sceImeDialogInit -> 0x%x", rc);
        if (rc < 0) {
            if (!kb_msg_probe_tried) {
                /* Bug 40 split test: prereqs, languages, modes,
                 * magic and HCR-shaped buffers have all failed to
                 * move 0x800204c4. Fire a plain message dialog
                 * (HCR's exact recipe): if it inits, the common-
                 * dialog subsystem is healthy and the fault is
                 * IME-param-specific; if it fails the same way,
                 * the dialog layer itself is unavailable in this
                 * process. Once per boot. */
                kb_msg_probe_tried = 1;
                SceMsgDialogUserMessageParam mp;
                memset(&mp, 0, sizeof(mp));
                mp.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
                mp.msg = (const SceChar8 *)"Keyboard probe: if you can read this, dialogs work - the IME call itself is the fault. Tap OK.";
                SceMsgDialogParam dp;
                sceMsgDialogParamInit(&dp);
                _sceCommonDialogSetMagicNumber(&dp.commonParam);
                dp.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
                dp.userMsgParam = &mp;
                int mrc = sceMsgDialogInit(&dp);
                blog("KB: msg-dialog probe init -> 0x%x", mrc);
                if (mrc == 0)
                    kb_msg_probe_state = 1;
            }
            return;
        }
        kb_ime_state = 1;
        kb_ime_set_open(1);
        kb_set_active(1);
        blog("KB: IME dialog OPEN");
        return;
    }

    /* OPEN */
    if (kb_abort_pending()) {
        kb_clear_abort();
        blog("KB: IME abort (engine hid the keyboard mid-dialog)");
        sceImeDialogAbort();
        sceImeDialogTerm();
        kb_ime_state = 0;
        kb_ime_set_open(0);
        kb_set_active(0);
        blog("KB: IME dialog CLOSED (abort)");
        return;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return; /* still running: poll again next frame, never block */
    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    int rc = sceImeDialogGetResult(&result);
    blog("KB: IME finished: button=%d getResult=0x%x", result.button, rc);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER)
        kb_deliver_text(kb_ime_text);
    else
        blog("KB: IME closed without ENTER - no injection");
    sceImeDialogTerm();
    kb_ime_state = 0;
    kb_ime_set_open(0);
    kb_set_active(0);
    blog("KB: IME dialog CLOSED");
}

int main(void) {
    bootlog_init();
    blog("main: entry (Pocket Tanks Vita, BUILD BO)");
    blog("main: BUILD BO = BN + writable-RAM range check on out-pointers");

    // Overclock for headroom (same as other .so ports)
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    blog("main: clocks set 444/222/222/166");

    /* Bug 40: common-dialog prerequisites. The Vita IME dialog is a
     * common dialog; the SDK's own IME sample inits AppUtil and sets
     * the common-dialog config before any dialog use, and this
     * loader never did either — with the IME sysmodule loaded and a
     * well-formed param, sceImeDialogInit still failed 0x800204c4
     * (bootlog-32). Cheap to satisfy both, so satisfy both. */
    {
        SceAppUtilInitParam au_init;
        SceAppUtilBootParam au_boot;
        memset(&au_init, 0, sizeof(au_init));
        memset(&au_boot, 0, sizeof(au_boot));
        int aurc = sceAppUtilInit(&au_init, &au_boot);
        SceCommonDialogConfigParam cd_conf;
        memset(&cd_conf, 0, sizeof(cd_conf));
        int cdrc = sceCommonDialogSetConfigParam(&cd_conf);
        blog("main: dialog prereqs: sceAppUtilInit -> 0x%x, sceCommonDialogSetConfigParam -> 0x%x",
             aurc, cdrc);
    }

    bionic_sF_init();
    blog("main: bionic stdio init done");

    log_data_dir();

    // Data files must be installed by the user (see data_prep.sh)
    if (!file_exists(DATA_PATH "libengine.so")) {
        blog("main: FATAL - libengine.so not found in " DATA_PATH);
        fatal_error("Data files not found. Install the game data to %s "
                    "(see data_prep.sh).", DATA_PATH);
    }
    blog("main: libengine.so present, loading libraries");

    // Load libraries in dependency order (so_util chains them by SONAME)
    int rc;
    rc = so_file_load(&mod_cpp, DATA_PATH "libc++_shared.so", LOAD_ADDR_CPP);
    blog("main: so_file_load libc++_shared.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libc++_shared.so load failed"); fatal_error("Could not load libc++_shared.so"); }

    rc = so_file_load(&mod_fmod, DATA_PATH "libfmod.so", LOAD_ADDR_FMOD);
    blog("main: so_file_load libfmod.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libfmod.so load failed"); fatal_error("Could not load libfmod.so"); }

    rc = so_file_load(&mod_engine, DATA_PATH "libengine.so", LOAD_ADDR_ENGINE);
    blog("main: so_file_load libengine.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libengine.so load failed"); fatal_error("Could not load libengine.so"); }

    // Bug 25: libengine.so's internal file-open helper takes the fopen path
    // only when filename[0] == '/'. On Android the prefs path is absolute,
    // but our "ux0:..." paths fell into the dead asset path, so every prefs
    // write failed ("cp_fwrite failed"). libengine.so lives in the data dir
    // (never updated by a VPK reinstall), so patch the loaded image in
    // memory instead: file offset 0x3d3366 sits in the executable PT_LOAD
    // (p_offset=0), so runtime addr = text_base + 0x3d3366.
    // Patch: cmp r0, #47 (0x282f) -> cmp r0, #117 (0x2875).
    {
        uint16_t *pp = (uint16_t *)(mod_engine.text_base + 0x3d3366);
        uint16_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, pp, sizeof(before));
        if (before == 0x282f) {
            uint16_t after = 0x2875;
            kuKernelCpuUnrestrictedMemcpy(pp, &after, sizeof(after));
            so_flush_caches(&mod_engine);
            blog("main: [libengine] runtime patch APPLIED: cmp r0,#47->#117 at %p (0x%04x->0x%04x)",
                 (void *)pp, before, after);
        } else {
            blog("main: [libengine] runtime patch SKIPPED at %p (halfword=0x%04x, want 0x282f)",
                 (void *)pp, before);
        }
    }

    // Bug 27: libengine.so's throwing get_child (boost::property_tree) throws
    // ptree_bad_path "No such node (<xmlattr>)" when the zoom UI config
    // queries attributes of an XML element that has none. The game expects an
    // empty ptree (it iterates attributes / reads them with defaults).
    //
    // Bug 28: the Bug27 trampoline (return fake for ALL missing nodes) broke
    // ptree::get<T>(path, default): get() catches ptree_bad_path to return
    // the default, but our fake made get_value<T>() do lexical_cast<T>(""),
    // throwing bad_lexical_cast (uncaught) -> terminate. Confirmed via
    // disassembly: the throw is lexical_cast<ushort> inside boost::gregorian
    // date parsing, reached from get<date>(path, default) on a missing node.
    //
    // Fix: path-based policy in the trampoline. Only "<xmlattr>" queries get
    // the fake empty ptree (the XML-attribute iteration pattern). ALL other
    // missing-node queries take the ORIGINAL throw path (text_base+0xa48d0,
    // re-derived from the pre-patch `cbz r0,<throw>`) so get-with-default
    // keeps working.
    //
    // Register facts (verified from disassembly): at the site, r0 = find_child
    // result (NULL if not found), r8 = path_type& (callee-saved, set at
    // 0xa4898 `mov r8,r1`, preserved by find_child, passed to the throw-path
    // message builder at 0xa48de). The path is a boost string_path wrapping
    // the game's 12-byte string: byte[0] even -> short (len=byte[0]>>1, data
    // at str+1); byte[0] odd -> long (len=[str+4], data=[str+8]).
    //
    // Site: file offset 0xa48b0 in the executable PT_LOAD (p_offset=0),
    // so runtime addr = text_base + 0xa48b0.
    // Original: 4605 b168 (mov r5, r0; cbz r0, <throw @ 0xa48d0>)
    // Patched:  b.w <trampoline> (trampoline in the RX patch arena, <16MB away)
    //
    // Trampoline (122 bytes; every halfword verified against arm-vita-eabi-as):
    //   T+0:  cbnz r0, FOUND            ; r0 != NULL -> r5 = r0
    //   T+2:  mov r3, r8                ; 0x4643, r3 = path
    //   T+4:  ldrb r2, [r3]             ; 0x781A, r2 = m_value[0]
    //   T+6:  mov r1, r2                ; 0x4611, save byte
    //   T+8:  lsls r2, r2, #1           ; 0x0052, carry = bit0 (long?)
    //   T+10: bcs LONG
    //   T+12: lsrs r2, r1, #1           ; 0x084A, r2 = len (short)
    //   T+14: adds r1, r3, #1           ; 0x1C59, r1 = data (short)
    //   T+16: b SCAN
    //   T+18: LONG: ldr r1, [r3, #8]    ; 0x6899, r1 = data (long)
    //   T+20: ldr r2, [r3, #4]          ; 0x685A, r2 = len (long)
    //   T+22: SCAN: cmp r2, #9          ; 0x2A09, need >= 9 bytes
    //   T+24: blo NOTFOUND
    //   T+26: movw r3, #0x783C          ; r3 = "<xml" (LE 0x6C6D783C)
    //   T+30: movt r3, #0x6C6D
    //   T+34: LOOP: ldr r0, [r1]        ; 0x6808
    //   T+36: cmp r0, r3                ; 0x4298
    //   T+38: bne ADVANCE
    //   T+40: push {r3}                 ; 0xB408, save "<xml"
    //   T+42: movw r3, #0x7461          ; r3 = "attr" (LE 0x72747461)
    //   T+46: movt r3, #0x7274
    //   T+50: ldr r0, [r1, #4]          ; 0x6848
    //   T+52: cmp r0, r3                ; 0x4298
    //   T+54: pop {r3}                  ; 0xBC08, restore "<xml"
    //   T+56: bne ADVANCE
    //   T+58: ldrb r0, [r1, #8]         ; 0x7A08
    //   T+60: cmp r0, #0x3E             ; 0x283E, '>'
    //   T+62: beq LOG
    //   T+64: ADVANCE: adds r1, r1, #1  ; 0x3101
    //   T+66: subs r2, r2, #1           ; 0x3A01
    //   T+68: cmp r2, #9                ; 0x2A09
    //   T+70: bhs LOOP
    //   T+72: NOTFOUND: b.w THROW       ; THROW = text_base + 0xa48d0
    //   T+76: LOG: ldr r3, [r7, #4]     ; 0x687B, saved LR = direct caller
    //   T+78: mov r0, r8                ; 0x4640, arg0 = path object
    //   T+80: mov r1, r3                ; 0x4619, arg1 = saved LR
    //   T+82: movw r2, #lo16(text_base) ; arg2 = text_base
    //   T+86: movt r2, #hi16(text_base)
    //   T+90: movw r5, #lo16(&empty_ptree)
    //   T+94: movt r5, #hi16(&empty_ptree)
    //   T+98: movw r12, #lo16(ptree_fake_diag_log)
    //   T+102: movt r12, #hi16(ptree_fake_diag_log)
    //   T+106: sub sp, #4               ; 0xB081, AAPCS 8B-align at call
    //   T+108: blx r12                  ; 0x47E0
    //   T+110: add sp, #4               ; 0xB001
    //   T+112: b.w NORMAL               ; NORMAL = text_base + 0xa48b4
    //   T+116: FOUND: mov r5, r0        ; 0x4605
    //   T+118: b.w NORMAL
    // Bug 29 diagnostic: the LOG block is the ONLY change vs the shipping
    // trampoline — it calls the loader's FAKE-DIAG logger on the fake
    // branch, then falls into the exact same fake exit (r5 = empty ptree,
    // b.w NORMAL). Fake-vs-throw decisions and both other exits are
    // byte-identical in behavior. New halfwords verified against
    // arm-vita-eabi-as: 0x687B (diagnosis), 0x4640, 0x4619, 0xB081, 0x47E0,
    // 0xB001, and movw/movt for r2/r12 via the proven encoders.
    // Only r0-r3/r12 are clobbered (caller-saved); r8 is preserved
    // (read-only); SP is restored before the NORMAL branch.
    {
        uintptr_t site = mod_engine.text_base + 0xa48b0;
        uintptr_t normal = mod_engine.text_base + 0xa48b4;
        uintptr_t throw_path = mod_engine.text_base + 0xa48d0;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xb1684605u) {
            void *empty_ptree = ptree_fake_init();
            if (!empty_ptree) {
                blog("main: [libengine] Bug27/28 patch SKIPPED: empty ptree alloc failed");
            } else {
                // Allocate trampoline in the RX patch arena, within b.w range.
                uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 128);
                if (tramp) {
                    uint16_t tcode[61];
                    uintptr_t ept = (uintptr_t)empty_ptree;
                    uintptr_t text_base = mod_engine.text_base;
                    uintptr_t diag_fn = (uintptr_t)(void *)ptree_fake_diag_log;
                    // Offsets (in halfwords) for branch targets
                    const uintptr_t O_FOUND = 58, O_LONG = 9, O_SCAN = 11,
                                        O_LOOP = 17, O_ADV = 32, O_NOTFOUND = 36,
                                        O_LOG = 38;
                    encode_cbnz(&tcode[0], tramp + 0, tramp + O_FOUND*2, 0, 1);
                    tcode[1] = 0x4643;  // mov r3, r8
                    tcode[2] = 0x781A;  // ldrb r2, [r3]
                    tcode[3] = 0x4611;  // mov r1, r2
                    tcode[4] = 0x0052;  // lsls r2, r2, #1
                    encode_b_cond(&tcode[5], tramp + 10, tramp + O_LONG*2, 2);  // bcs LONG
                    tcode[6] = 0x084A;  // lsrs r2, r1, #1
                    tcode[7] = 0x1C59;  // adds r1, r3, #1
                    encode_b_n(&tcode[8], tramp + 16, tramp + O_SCAN*2);  // b SCAN
                    tcode[9] = 0x6899;   // LONG: ldr r1, [r3, #8]
                    tcode[10] = 0x685A;  // ldr r2, [r3, #4]
                    tcode[11] = 0x2A09;  // SCAN: cmp r2, #9
                    encode_b_cond(&tcode[12], tramp + 24, tramp + O_NOTFOUND*2, 3);  // blo NOTFOUND
                    encode_movw(&tcode[13], 3, 0x783Cu);  // movw r3, #"<xml"
                    encode_movt(&tcode[15], 3, 0x6C6Du);  // movt r3
                    tcode[17] = 0x6808;  // LOOP: ldr r0, [r1]
                    tcode[18] = 0x4298;  // cmp r0, r3
                    encode_b_cond(&tcode[19], tramp + 38, tramp + O_ADV*2, 1);  // bne ADVANCE
                    tcode[20] = 0xB408;  // push {r3}
                    encode_movw(&tcode[21], 3, 0x7461u);  // movw r3, #"attr"
                    encode_movt(&tcode[23], 3, 0x7274u);  // movt r3
                    tcode[25] = 0x6848;  // ldr r0, [r1, #4]
                    tcode[26] = 0x4298;  // cmp r0, r3
                    tcode[27] = 0xBC08;  // pop {r3}
                    encode_b_cond(&tcode[28], tramp + 56, tramp + O_ADV*2, 1);  // bne ADVANCE
                    tcode[29] = 0x7A08;  // ldrb r0, [r1, #8]
                    tcode[30] = 0x283E;  // cmp r0, #0x3E ('>')
                    encode_b_cond(&tcode[31], tramp + 62, tramp + O_LOG*2, 0);  // beq LOG
                    tcode[32] = 0x3101;  // ADVANCE: adds r1, r1, #1
                    tcode[33] = 0x3A01;  // subs r2, r2, #1
                    tcode[34] = 0x2A09;  // cmp r2, #9
                    encode_b_cond(&tcode[35], tramp + 70, tramp + O_LOOP*2, 2);  // bhs LOOP
                    encode_bw(&tcode[36], tramp + 72, throw_path);  // NOTFOUND: b.w THROW
                    // LOG (Bug 29 diagnostic): log path + caller, then the
                    // exact same fake exit as before. All halfwords verified
                    // against arm-vita-eabi-as (see trampoline comment).
                    tcode[38] = 0x687B;  // ldr r3, [r7, #4] (saved LR)
                    tcode[39] = 0x4640;  // mov r0, r8 (path object)
                    tcode[40] = 0x4619;  // mov r1, r3 (saved LR)
                    encode_movw(&tcode[41], 2, (uint32_t)(text_base & 0xFFFFu));
                    encode_movt(&tcode[43], 2, (uint32_t)((text_base >> 16) & 0xFFFFu));
                    encode_movw(&tcode[45], 5, (uint32_t)(ept & 0xFFFFu));  // r5 = &empty_ptree
                    encode_movt(&tcode[47], 5, (uint32_t)((ept >> 16) & 0xFFFFu));
                    encode_movw(&tcode[49], 12, (uint32_t)(diag_fn & 0xFFFFu));
                    encode_movt(&tcode[51], 12, (uint32_t)((diag_fn >> 16) & 0xFFFFu));
                    tcode[53] = 0xB081;  // sub sp, #4 (align for the call)
                    tcode[54] = 0x47E0;  // blx r12
                    tcode[55] = 0xB001;  // add sp, #4
                    encode_bw(&tcode[56], tramp + 112, normal);  // b.w NORMAL
                    tcode[58] = 0x4605;  // FOUND: mov r5, r0
                    encode_bw(&tcode[59], tramp + 118, normal);  // b.w NORMAL
                    kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                    // Patch site with b.w to trampoline
                    uint16_t bcode[2];
                    encode_bw(bcode, site, tramp);
                    kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                    so_flush_caches(&mod_engine);
                    blog("main: [libengine] Bug27/28 patch APPLIED: get_child not-found -> <xmlattr>?fake:throw (tramp=%p)",
                         (void *)tramp);
                    blog("main: [loader] EH-FIX BUILD 2026-10-13-X: registry roster");
                } else {
                    blog("main: [libengine] Bug27/28 patch SKIPPED: no arena space in range");
                }
            }
        } else {
            blog("main: [libengine] Bug27/28 patch SKIPPED at %p (word=0x%08x, want 0xb1684605)",
                 (void *)site, before);
        }
    }

    // Bug 31 trace: hook the scene manager's switch function (libengine
    // file offset 0x2a8c3c) at its entry and log every requested scene
    // name. This is the function that produces the game log's
    // "Switching to scene %s": it searches the scene registry for the
    // name in r1 (a plain C string — strcmp'd against each registered
    // scene's name and passed as the log call's %s), stores the found
    // scene into the manager at [r0+4], and returns 1. On no match it
    // returns 0 and logs NOTHING — so the game log cannot distinguish
    // "title scene never requested" from "requested but not found".
    // The entry hook logs ALL requests and changes nothing else.
    //
    // Site: file offset 0x2a8c3c in the executable PT_LOAD (p_offset=0),
    // so runtime addr = text_base + 0x2a8c3c.
    // Original: b5f0 af03 (push {r4,r5,r6,r7,lr}; add r7, sp, #12)
    // Patched:  b.w <trampoline> (trampoline in the RX patch arena)
    //
    // Trampoline (32 bytes; fixed halfwords verified against
    // arm-vita-eabi-as, branches via the proven encoders):
    //   T+0:  push.w {r0, r1, r2, r3, lr}   ; E92D 400F
    //   T+4:  mov r0, r1                    ; 0x4608, arg0 = scene name
    //   T+6:  movw r12, #lo16(scene_trace_log)
    //   T+10: movt r12, #hi16(scene_trace_log)
    //   T+14: sub sp, #4                    ; 0xB081, AAPCS 8B-align (20B pushed)
    //   T+16: blx r12                       ; 0x47E0
    //   T+18: add sp, #4                    ; 0xB001
    //   T+20: pop.w {r0, r1, r2, r3, lr}    ; E8BD 400F
    //   T+24: push {r4, r5, r6, r7, lr}     ; 0xB5F0, displaced verbatim
    //   T+26: add r7, sp, #12               ; 0xAF03, displaced verbatim
    //   T+28: b.w site+4
    // The displaced instructions are not PC-relative, so executing them
    // verbatim in the arena is exact. Only caller-saved regs are touched
    // and all are restored; SP is balanced; r4-r12 and the stack the
    // function sees are identical to the unpatched entry.
    {
        uintptr_t site = mod_engine.text_base + 0x2a8c3c;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xaf03b5f0u) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 48);
            if (tramp) {
                uintptr_t log_fn = (uintptr_t)(void *)scene_trace_log;
                uint16_t tcode[16];
                tcode[0] = 0xE92D; tcode[1] = 0x400F;  // push.w {r0,r1,r2,r3,lr}
                tcode[2] = 0x4608;                     // mov r0, r1
                encode_movw(&tcode[3], 12, (uint32_t)(log_fn & 0xFFFFu));
                encode_movt(&tcode[5], 12, (uint32_t)((log_fn >> 16) & 0xFFFFu));
                tcode[7] = 0xB081;  // sub sp, #4
                tcode[8] = 0x47E0;  // blx r12
                tcode[9] = 0xB001;  // add sp, #4
                tcode[10] = 0xE8BD; tcode[11] = 0x400F;  // pop.w {r0,r1,r2,r3,lr}
                tcode[12] = 0xB5F0;  // displaced: push {r4,r5,r6,r7,lr}
                tcode[13] = 0xAF03;  // displaced: add r7, sp, #12
                encode_bw(&tcode[14], tramp + 28, site + 4);  // b.w site+4
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                blog("main: [libengine] Bug31 scene trace APPLIED: switch fn 0x2a8c3c entry hooked (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug31 scene trace SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug31 scene trace SKIPPED at %p (word=0x%08x, want 0xaf03b5f0)",
                 (void *)site, before);
        }
    }

    // Bug 37 (Build X): exit hook on the same switch fn's SHARED
    // exit (libengine file offset 0x2a8ca6) — the outcome half of
    // the Bug 31 entry trace above, see scene_exit_log. Both of the
    // fn's exits funnel through 0x2a8ca6 (found: `movs r0, #1` at
    // 0x2a8ca0 + b.n; not-found: `movs r0, #0` at 0x2a8ca4), where
    // r0 = return value, r9 = mgr, sl = requested name — all live.
    //
    // Site: file offset 0x2a8ca6 in the executable PT_LOAD
    // (p_offset=0), so runtime addr = text_base + 0x2a8ca6.
    // Original: e8bd 0700 (ldmia.w sp!, {r8, r9, sl}), followed at
    // 0x2a8caa by bdf0 (pop {r4, r5, r6, r7, pc}).
    // Patched:  b.w <trampoline> (trampoline in the RX patch arena)
    //
    // Exit trampoline (28 bytes; every fixed halfword verified
    // against arm-vita-eabi-as — 0x4649 mov r1, r9 and 0x4652
    // mov r2, sl included — and the stream contains no branches,
    // so the only absolute content is the movw/movt helper load):
    //   T+0:  push.w {r0, r1, r2, r3}   ; E92D 000F (preserves the
    //                                   return value; SP is 8-aligned
    //                                   at the exit point — entry SP
    //                                   aligned, the fn pushed 32B —
    //                                   so the C call needs no adjust)
    //   T+4:  mov r1, r9                ; 0x4649, arg1 = mgr
    //   T+6:  mov r2, sl                ; 0x4652, arg2 = name
    //   T+8:  movw/movt r12, &scene_exit_log
    //   T+16: blx r12                   ; 0x47E0
    //   T+18: pop.w {r0, r1, r2, r3}    ; E8BD 000F
    //   T+22: ldmia.w sp!, {r8, r9, sl} ; E8BD 0700, displaced verbatim
    //   T+26: pop {r4, r5, r6, r7, pc}  ; 0xBDF0, displaced verbatim —
    //                                   the fn's own return, executed
    //                                   on its untouched stack
    // r9/sl are AAPCS callee-saved, so the C helper cannot disturb
    // them; after the pop.w the stack is byte-identical to the
    // unpatched exit point.
    {
        uintptr_t site = mod_engine.text_base + 0x2a8ca6;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0x0700e8bdu) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 48);
            if (tramp) {
                uintptr_t log_fn = (uintptr_t)(void *)scene_exit_log;
                uint16_t tcode[14];
                tcode[0] = 0xE92D; tcode[1] = 0x000F;  // push.w {r0,r1,r2,r3}
                tcode[2] = 0x4649;                     // mov r1, r9
                tcode[3] = 0x4652;                     // mov r2, sl
                encode_movw(&tcode[4], 12, (uint32_t)(log_fn & 0xFFFFu));
                encode_movt(&tcode[6], 12, (uint32_t)((log_fn >> 16) & 0xFFFFu));
                tcode[8] = 0x47E0;  // blx r12
                tcode[9] = 0xE8BD; tcode[10] = 0x000F;  // pop.w {r0,r1,r2,r3}
                tcode[11] = 0xE8BD; tcode[12] = 0x0700;  // displaced: ldmia.w sp!, {r8,r9,sl}
                tcode[13] = 0xBDF0;  // displaced: pop {r4,r5,r6,r7,pc}
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                blog("main: [libengine] Bug37 scene exit trace APPLIED: switch fn 0x2a8c3c exit hooked at 0x2a8ca6 (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug37 scene exit trace SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug37 scene exit trace SKIPPED at %p (word=0x%08x, want 0x0700e8bd)",
                 (void *)site, before);
        }
    }

    // Bug 32-I: interpose the scene-dispatch forwarder (libengine file
    // offset 0x2a8e6a) to bracket every vtable +0x18 dispatch with
    // enter/exit lines once the TITLE switch arms the tracer. Per
    // bug32-analysis.md section 3.3, the stuck frame's never-returning
    // call is dispatch #2 — TITLE's first +0x18 call, made through
    // this forwarder from the frame tail. The enter line with no
    // matching exit names it, and the logged target is TITLE's +0x18
    // body address for static follow-up.
    //
    // The forwarder (disassembled from the ELF; a family of identical
    // stubs, one per vtable slot):
    //   0x2a8e6a: 6800  ldr r0, [r0, #0]   ; scene = *cell
    //   0x2a8e6c: b110  cbz r0, 0x2a8e74   ; NULL scene -> return
    //   0x2a8e6e: 6801  ldr r1, [r0, #0]   ; vtable = *scene
    //   0x2a8e70: 6989  ldr r1, [r1, #24]  ; target = vtable[+0x18]
    //   0x2a8e72: 4708  bx r1              ; tail call, r0=scene
    //   0x2a8e74: 4770  bx lr
    // Note the register state it hands the callee: r0=scene,
    // r1=target itself, r2/r3 = the forwarder caller's leftovers, SP
    // and LR passed through untouched. The trampoline replicates that
    // state exactly (dispatch_log_enter does the deref/NULL-check/
    // slot-load in C; the trampoline then sets r0=scene, r1=target).
    //
    // Site: runtime addr = text_base + 0x2a8e6a; first 4 bytes are
    // 0xb1106800 (LE). Patched: b.w <trampoline> (RX patch arena).
    //
    // Entry trampoline (42 bytes; every fixed halfword verified
    // against arm-vita-eabi-as, address loads via the proven
    // movw/movt encoders):
    //   T+0:  push {r4, lr}              ; B510 — saved pair stays on
    //          the stack across the target call (SP is SP0-8 during
    //          it; the +0x18 contract provably uses no stack args —
    //          the forwarder itself poisons r1 — and no register
    //          args beyond r0)
    //   T+2:  push.w {r0, r1, r2, r3}    ; E92D 000F
    //   T+6:  movw/movt r12, &dispatch_log_enter
    //   T+14: blx r12                    ; 47E0 — r0=cell in, target out;
    //          pushed 24B total keeps SP 8-aligned at the call
    //   T+16: mov r4, r0                 ; 4604 — r4 = target (or 0)
    //   T+18: pop.w {r0, r1, r2, r3}     ; E8BD 000F
    //   T+22: cbz r4, T+40               ; encoder; NULL scene -> return
    //   T+24: ldr r0, [r0, #0]           ; 6800 — scene (verbatim insn)
    //   T+26: mov r1, r4                 ; 4621 — r1 = target, as orig
    //   T+28: movw/movt lr, exit_stub|1  ; return into the exit stub
    //   T+36: bx r4                      ; 4720 — tail into the target
    //   T+38: nop                        ; 46C0 — padding, never executed
    //   T+40: pop {r4, pc}               ; BD10 — NULL path: plain return
    // Exit stub (at T+48, 20 bytes; entered via the target's bx lr,
    // SP is SP0-8 with the {r4, lr} pair still on the stack):
    //   E+0:  push.w {r0, r1, r2, r3}    ; E92D 000F — keep target's
    //          return value and regs; SP back to 8-aligned SP0-24
    //   E+4:  movw/movt r12, &dispatch_log_exit
    //   E+12: blx r12                    ; 47E0
    //   E+14: pop.w {r0, r1, r2, r3}     ; E8BD 000F
    //   E+18: pop {r4, pc}               ; BD10 — restore r4, return to
    //          the forwarder's caller with the target's r0 intact
    {
        uintptr_t site = mod_engine.text_base + 0x2a8e6a;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xb1106800u) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 96);
            if (tramp) {
                uintptr_t enter_fn = (uintptr_t)(void *)dispatch_log_enter;
                uintptr_t exit_fn = (uintptr_t)(void *)dispatch_log_exit;
                uintptr_t exit_stub = (tramp + 48) | 1u;  // Thumb bit for lr
                uint16_t tcode[34] = {0};
                tcode[0] = 0xB510;   // push {r4, lr}
                tcode[1] = 0xE92D; tcode[2] = 0x000F;   // push.w {r0,r1,r2,r3}
                encode_movw(&tcode[3], 12, (uint32_t)(enter_fn & 0xFFFFu));
                encode_movt(&tcode[5], 12, (uint32_t)((enter_fn >> 16) & 0xFFFFu));
                tcode[7] = 0x47E0;   // blx r12
                tcode[8] = 0x4604;   // mov r4, r0
                tcode[9] = 0xE8BD; tcode[10] = 0x000F;  // pop.w {r0,r1,r2,r3}
                encode_cbnz(&tcode[11], tramp + 22, tramp + 40, 4, 0);  // cbz r4, T+40
                tcode[12] = 0x6800;  // ldr r0, [r0, #0]
                tcode[13] = 0x4621;  // mov r1, r4
                encode_movw(&tcode[14], 14, (uint32_t)(exit_stub & 0xFFFFu));
                encode_movt(&tcode[16], 14, (uint32_t)((exit_stub >> 16) & 0xFFFFu));
                tcode[18] = 0x4720;  // bx r4
                tcode[19] = 0x46C0;  // nop (padding)
                tcode[20] = 0xBD10;  // pop {r4, pc}  (NULL path)
                // T+42..T+47: zero gap, never executed (entry ends in
                // bx/pop; the exit stub is reached only via lr).
                tcode[24] = 0xE92D; tcode[25] = 0x000F;  // exit: push.w {r0,r1,r2,r3}
                encode_movw(&tcode[26], 12, (uint32_t)(exit_fn & 0xFFFFu));
                encode_movt(&tcode[28], 12, (uint32_t)((exit_fn >> 16) & 0xFFFFu));
                tcode[30] = 0x47E0;  // blx r12
                tcode[31] = 0xE8BD; tcode[32] = 0x000F;  // pop.w {r0,r1,r2,r3}
                tcode[33] = 0xBD10;  // pop {r4, pc}
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                blog("main: [libengine] Bug32 dispatch trace APPLIED: forwarder 0x2a8e6a interposed (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug32 dispatch trace SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug32 dispatch trace SKIPPED at %p (word=0x%08x, want 0xb1106800)",
                 (void *)site, before);
        }
    }

    // Bug 32-L: THE FIX's sensor — entry hook on the engine's present
    // wrapper (libengine file offset 0x3d6628), counting every call
    // in g_engine_present_count (see present_count_inc). The wrapper
    // is the engine's only present path: the frame tail calls it
    // unconditionally in the mode-3 arm, and drawBumper tail-jumps
    // it; its body re-checks the mode (==3) and calls eglSwapBuffers.
    // The frame loop snapshots the counter around onDrawFrame and
    // skips its own graphics_swap() whenever the engine presented —
    // one swap per iteration total, so the draw target alternates
    // between the two display buffers and frame 0's stale loading
    // screen is overwritten by a fresh menu render within two
    // iterations instead of being re-queued forever (section 5.3).
    // g_conditional_swap is set ONLY in the APPLIED branch below:
    // if this hook is SKIPPED the loop keeps the unconditional swap.
    //
    // Site: file offset 0x3d6628 in the executable PT_LOAD
    // (p_offset=0), so runtime addr = text_base + 0x3d6628.
    // Original: b0b5 02af (push {r4,r5,r7,lr}; add r7, sp, #8) —
    // 4 bytes, not PC-relative (the PIC ldr follows at +4 and runs
    // at the site), displaced verbatim after the counting call.
    // Patched:  b.w <trampoline> (trampoline in the RX patch arena)
    //
    // Trampoline (30 bytes; fixed halfwords verified against
    // arm-vita-eabi-as, the full stream capstone-verified at arena
    // placements above and below the site — same shape as the
    // Bug 31 scene-switch hook):
    //   T+0:  push.w {r0, r1, r2, r3, lr}   ; E92D 400F
    //   T+4:  movw/movt r12, &present_count_inc
    //   T+12: sub sp, #4                    ; 0xB081, AAPCS 8B-align (20B pushed)
    //   T+14: blx r12                       ; 0x47E0
    //   T+16: add sp, #4                    ; 0xB001
    //   T+18: pop.w {r0, r1, r2, r3, lr}    ; E8BD 400F
    //   T+22: push {r4, r5, r7, lr}         ; 0xB5B0, displaced verbatim
    //   T+24: add r7, sp, #8                ; 0xAF02, displaced verbatim
    //   T+26: b.w site+4
    // Only caller-saved registers are touched and all are restored;
    // SP is balanced; the wrapper sees its exact original entry
    // state (the first flag-setting instruction it executes is its
    // own, as with every entry hook in this file).
    {
        uintptr_t site = mod_engine.text_base + 0x3d6628;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xaf02b5b0u) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 48);
            if (tramp) {
                uintptr_t cnt_fn = (uintptr_t)(void *)present_count_inc;
                uint16_t tcode[15];
                tcode[0] = 0xE92D; tcode[1] = 0x400F;  // push.w {r0,r1,r2,r3,lr}
                encode_movw(&tcode[2], 12, (uint32_t)(cnt_fn & 0xFFFFu));
                encode_movt(&tcode[4], 12, (uint32_t)((cnt_fn >> 16) & 0xFFFFu));
                tcode[6] = 0xB081;  // sub sp, #4
                tcode[7] = 0x47E0;  // blx r12
                tcode[8] = 0xB001;  // add sp, #4
                tcode[9] = 0xE8BD; tcode[10] = 0x400F;  // pop.w {r0,r1,r2,r3,lr}
                tcode[11] = 0xB5B0;  // displaced: push {r4,r5,r7,lr}
                tcode[12] = 0xAF02;  // displaced: add r7, sp, #8
                encode_bw(&tcode[13], tramp + 26, site + 4);  // b.w site+4
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                g_conditional_swap = 1;
                blog("main: [libengine] Bug32 present count APPLIED: present wrapper 0x3d6628 entry hooked (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug32 present count SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug32 present count SKIPPED at %p (word=0x%08x, want 0xaf02b5b0)",
                 (void *)site, before);
        }
    }

    // Bug 32-L: bracket the frame tail's render fn (libengine file
    // offset 0x220b80) with enter/exit counters (section 5.5 item 2
    // in counts-only form; reported in the frame heartbeat). The
    // render is the mode-3 arm's own draw — there is no DRAW vtable
    // slot (5.1.2) — called from the tail between dispatch #2 and
    // the present. Enter/exit pairing in the heartbeat proves it
    // runs every TITLE-era frame and returns.
    //
    // Same LR-interception shape as the Bug 32-I dispatch bracket,
    // with one twist: the render's prologue sets up an r7 frame
    // pointer, so the displaced prologue MUST execute at the
    // original SP. The incoming LR is therefore handed to
    // render_enter_inc (read from the pushed copy at [sp,#16]),
    // which stacks it in C; the trampoline then drops the saved-LR
    // stack slot (add sp, #4) so SP is exact when the displaced
    // prologue runs, with LR = the exit stub. The render's epilogue
    // pops that planted LR into pc, landing in the exit stub, which
    // counts the exit, restores the true LR from render_exit_inc,
    // and returns — r0 (any return value) preserved throughout.
    //
    // Site: file offset 0x220b80. Original: f0b5 03af
    // (push {r4,r5,r6,r7,lr}; add r7, sp, #12) — displaced verbatim.
    //
    // Entry trampoline (42 bytes; every fixed halfword verified
    // against arm-vita-eabi-as — 0x9804 ldr r0,[sp,#16] and 0x4686
    // mov lr,r0 included — and the full stream capstone-verified
    // at arena placements above and below the site):
    //   T+0:  push.w {r0, r1, r2, r3, lr}   ; E92D 400F
    //   T+4:  ldr r0, [sp, #16]             ; 0x9804, saved LR = arg
    //   T+6:  movw/movt r12, &render_enter_inc
    //   T+14: sub sp, #4                    ; 0xB081, AAPCS 8B-align (20B pushed)
    //   T+16: blx r12                       ; 0x47E0
    //   T+18: add sp, #4                    ; 0xB001
    //   T+20: pop.w {r0, r1, r2, r3}        ; E8BD 000F (LR slot stays)
    //   T+24: movw/movt lr, exit_stub|1
    //   T+32: add sp, #4                    ; 0xB001, drop saved-LR slot -> SP exact
    //   T+34: push {r4, r5, r6, r7, lr}     ; 0xB5F0, displaced verbatim
    //          (pushes lr = exit stub into the render's LR slot)
    //   T+36: add r7, sp, #12               ; 0xAF03, displaced verbatim
    //   T+38: b.w site+4
    //   T+42..47: zero gap, never executed
    // Exit stub (at T+48, 22 bytes; entered with SP fully restored
    // by the render's epilogue, return value in r0):
    //   E+0:  push.w {r0, r1, r2, r3}       ; E92D 000F (SP stays 8-aligned)
    //   E+4:  movw/movt r12, &render_exit_inc
    //   E+12: blx r12                       ; 0x47E0, r0 <- saved true LR
    //   E+14: mov lr, r0                    ; 0x4686
    //   E+16: pop.w {r0, r1, r2, r3}        ; E8BD 000F (restores return value)
    //   E+20: bx lr                         ; 0x4770
    {
        uintptr_t site = mod_engine.text_base + 0x220b80;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xaf03b5f0u) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 96);
            if (tramp) {
                uintptr_t enter_fn = (uintptr_t)(void *)render_enter_inc;
                uintptr_t exit_fn = (uintptr_t)(void *)render_exit_inc;
                uintptr_t exit_stub = (tramp + 48) | 1u;  // Thumb bit for lr
                uint16_t tcode[35] = {0};
                tcode[0] = 0xE92D; tcode[1] = 0x400F;  // push.w {r0,r1,r2,r3,lr}
                tcode[2] = 0x9804;                     // ldr r0, [sp, #16]
                encode_movw(&tcode[3], 12, (uint32_t)(enter_fn & 0xFFFFu));
                encode_movt(&tcode[5], 12, (uint32_t)((enter_fn >> 16) & 0xFFFFu));
                tcode[7] = 0xB081;  // sub sp, #4
                tcode[8] = 0x47E0;  // blx r12
                tcode[9] = 0xB001;  // add sp, #4
                tcode[10] = 0xE8BD; tcode[11] = 0x000F;  // pop.w {r0,r1,r2,r3}
                encode_movw(&tcode[12], 14, (uint32_t)(exit_stub & 0xFFFFu));
                encode_movt(&tcode[14], 14, (uint32_t)((exit_stub >> 16) & 0xFFFFu));
                tcode[16] = 0xB001;  // add sp, #4 (drop saved-LR slot)
                tcode[17] = 0xB5F0;  // displaced: push {r4,r5,r6,r7,lr}
                tcode[18] = 0xAF03;  // displaced: add r7, sp, #12
                encode_bw(&tcode[19], tramp + 38, site + 4);  // b.w site+4
                // T+42..T+47: zero gap, never executed (entry ends in
                // b.w; the exit stub is reached only via the planted LR).
                tcode[24] = 0xE92D; tcode[25] = 0x000F;  // exit: push.w {r0,r1,r2,r3}
                encode_movw(&tcode[26], 12, (uint32_t)(exit_fn & 0xFFFFu));
                encode_movt(&tcode[28], 12, (uint32_t)((exit_fn >> 16) & 0xFFFFu));
                tcode[30] = 0x47E0;  // blx r12
                tcode[31] = 0x4686;  // mov lr, r0
                tcode[32] = 0xE8BD; tcode[33] = 0x000F;  // pop.w {r0,r1,r2,r3}
                tcode[34] = 0x4770;  // bx lr
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                blog("main: [libengine] Bug32 render trace APPLIED: render fn 0x220b80 bracketed (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug32 render trace SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug32 render trace SKIPPED at %p (word=0x%08x, want 0xaf03b5f0)",
                 (void *)site, before);
        }
    }

    // Bug 33-R: entry hook on the engine's input-ring push
    // (libengine file offset 0x3d4c8c) — see push_trace_log above.
    // The push is the ring's only entrance (57 call sites funnel
    // into this one function; pass 4 section 28), so the hook sees
    // every record the census sees, at entry, with its producer's
    // return address — the separation bug33-analysis.md 38 item 3
    // requires. Diagnostics only: the helper reads the record and
    // logs non-axis pushes; it changes no state and no return
    // value, and the push body runs exactly as before.
    //
    // Site: file offset 0x3d4c8c in the executable PT_LOAD
    // (p_offset=0), so runtime addr = text_base + 0x3d4c8c.
    // Original: f0b5 03af (push {r4,r5,r6,r7,lr}; add r7, sp, #12)
    // — the same prologue as the render fn above, not PC-relative
    // (the PIC ldr follows at +4 and runs at the site), displaced
    // verbatim after the trace call. The record pointer arrives in
    // r0: call sites build the 20-byte record on the stack and
    // pass its address in r0 (e.g. the bl at 0x3d48ea is preceded
    // by add r0, sp, #0x10), and the push copies from r0 before
    // anything else. Patched: b.w <trampoline> (RX patch arena).
    //
    // Trampoline (32 bytes; fixed halfwords verified against
    // arm-vita-eabi-as — 0x9904 ldr r1,[sp,#16] included — and the
    // full stream capstone-verified at arena placements above and
    // below the site, same shape as the present-count hook with an
    // LR argument added):
    //   T+0:  push.w {r0, r1, r2, r3, lr}   ; E92D 400F
    //   T+4:  ldr r1, [sp, #16]             ; 0x9904, saved LR = arg 2
    //   T+6:  movw/movt r12, &push_trace_log
    //   T+14: sub sp, #4                    ; 0xB081, AAPCS 8B-align (20B pushed)
    //   T+16: blx r12                       ; 0x47E0 (r0 = record, still incoming)
    //   T+18: add sp, #4                    ; 0xB001
    //   T+20: pop.w {r0, r1, r2, r3, lr}    ; E8BD 400F
    //   T+24: push {r4, r5, r6, r7, lr}     ; 0xB5F0, displaced verbatim
    //   T+26: add r7, sp, #12               ; 0xAF03, displaced verbatim
    //   T+28: b.w site+4
    // Only caller-saved registers are touched and all are restored;
    // SP is balanced; the push sees its exact original entry state.
    {
        uintptr_t site = mod_engine.text_base + 0x3d4c8c;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xaf03b5f0u) {
            uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 48);
            if (tramp) {
                uintptr_t log_fn = (uintptr_t)(void *)push_trace_log;
                uint16_t tcode[16];
                tcode[0] = 0xE92D; tcode[1] = 0x400F;  // push.w {r0,r1,r2,r3,lr}
                tcode[2] = 0x9904;                     // ldr r1, [sp, #16]
                encode_movw(&tcode[3], 12, (uint32_t)(log_fn & 0xFFFFu));
                encode_movt(&tcode[5], 12, (uint32_t)((log_fn >> 16) & 0xFFFFu));
                tcode[7] = 0xB081;  // sub sp, #4
                tcode[8] = 0x47E0;  // blx r12
                tcode[9] = 0xB001;  // add sp, #4
                tcode[10] = 0xE8BD; tcode[11] = 0x400F;  // pop.w {r0,r1,r2,r3,lr}
                tcode[12] = 0xB5F0;  // displaced: push {r4,r5,r6,r7,lr}
                tcode[13] = 0xAF03;  // displaced: add r7, sp, #12
                encode_bw(&tcode[14], tramp + 28, site + 4);  // b.w site+4
                kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                uint16_t bcode[2];
                encode_bw(bcode, site, tramp);
                kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                so_flush_caches(&mod_engine);
                blog("main: [libengine] Bug33 push trace APPLIED: ring push 0x3d4c8c entry hooked (tramp=%p)",
                     (void *)tramp);
            } else {
                blog("main: [libengine] Bug33 push trace SKIPPED: no arena space in range");
            }
        } else {
            blog("main: [libengine] Bug33 push trace SKIPPED at %p (word=0x%08x, want 0xaf03b5f0)",
                 (void *)site, before);
        }
    }

    // Relocate + resolve each module (engine last so deps are in the chain)
    so_module *mods[] = {&mod_cpp, &mod_fmod, &mod_engine};
    const char *names[] = {"libc++_shared", "libfmod", "libengine"};
    for (int i = 0; i < 3; i++)
        blog("main: [%s] text_base=%p num_init=%d", names[i],
             (void *)mods[i]->text_base, mods[i]->num_init_array);
    so_init_logger = init_logger;
    for (int i = 0; i < 3; i++) {
        blog("main: [%s] so_relocate...", names[i]);
        so_relocate(mods[i]);
        blog("main: [%s] so_resolve...", names[i]);
        so_resolve(mods[i], default_dynlib, default_dynlib_size, 0);
        blog("main: [%s] so_flush_caches...", names[i]);
        so_flush_caches(mods[i]);
        blog("main: [%s] so_initialize...", names[i]);
        so_initialize(mods[i]);
        blog("main: [%s] relocated+resolved+initialized OK", names[i]);
    }

    // Bug 17: interpose the game's exception-throw entry points so the
    // bootlog names every exception it throws (the fatal one currently dies
    // mid-unwind on 0x0C-corrupted memory before we learn its type).
    {
        void *real_throw =
            patch_got_import(&mod_engine, "__cxa_throw", __cxa_throw_soloader);
        void *real_rethrow =
            patch_got_import(&mod_engine, "__cxa_rethrow", __cxa_rethrow_soloader);
        void *cur_ex_type = get_sym(&mod_cpp, "__cxa_current_exception_type", 0);
        cxa_throw_hook_init(real_throw, real_rethrow, cur_ex_type);
        blog("main: throw hooks installed (throw=%p rethrow=%p)",
             real_throw, real_rethrow);
    }

    /* Build AW: FMOD's CPU detection (NDK cpufeatures, statically
     * linked into libfmod) reads /proc/cpuinfo and scans the Features
     * line for "neon". The Vita has no /proc/cpuinfo, so FMOD has
     * concluded "no NEON" in every build and System_Create returns
     * error 41 (its DSP engine requires NEON). The Vita's Cortex-A9
     * HAS NEON — write a truthful cpuinfo into the datadir; the path
     * mapper in normalize_path_soloader serves it for /proc/cpuinfo. */
    {
        static const char cpuinfo_text[] =
            "Processor\t: ARMv7 Processor rev 0 (v7l)\n"
            "BogoMIPS\t: 1990.65\n"
            "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls "
            "vfpv4 idiva idivt\n"
            "CPU implementer\t: 0x41\n"
            "CPU architecture: 7\n"
            "CPU variant\t: 0x2\n"
            "CPU part\t: 0xc09\n"
            "CPU revision\t: 0\n"
            "Hardware\t: PS Vita\n"
            "Revision\t: 0000\n"
            "Serial\t\t: 0000000000000000\n";
        SceUID cfd = sceIoOpen("ux0:data/pockettanks/cpuinfo",
                               SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (cfd >= 0) {
            sceIoWrite(cfd, cpuinfo_text, sizeof(cpuinfo_text) - 1);
            sceIoClose(cfd);
            blog("main: wrote synthetic cpuinfo (NEON) for FMOD");
        } else {
            blog("main: cpuinfo write failed: 0x%x", cfd);
        }
    }

    // Fake JNI environment
    blog("main: jni_init...");
    jni_init();
    blog("main: jni_init done");

    // JNI_OnLoad: Android calls this when the .so loads; the game stashes
    // the JavaVM* in a global used by its thread-local JNIEnv* getter
    // (jvm->GetEnv). Without it, that getter dereferences a NULL VM*
    // and the game crashes inside onCreate.
    {
        typedef int (*jni_onload_fn)(void *vm, void *reserved);
        jni_onload_fn onload =
            (jni_onload_fn)get_sym(&mod_engine, "JNI_OnLoad", 0);
        if (onload) {
            blog("main: calling JNI_OnLoad...");
            int v = onload((void *)&jvm, NULL);
            blog("main: JNI_OnLoad returned %d (0x%x)", v, v);
        }
    }

    /* Build AV: libfmod's JNI_OnLoad has NEVER been called (only the
     * engine's was). FMOD's Android build registers its JavaVM state
     * and native methods there; System_Create failing with error 41
     * (NEEDSHARDWARE) is consistent with that registration missing. */
    {
        typedef int (*jni_onload_fn)(void *vm, void *reserved);
        jni_onload_fn fonload =
            (jni_onload_fn)get_sym(&mod_fmod, "JNI_OnLoad", 0);
        if (fonload) {
            blog("main: calling FMOD JNI_OnLoad...");
            int v = fonload((void *)&jvm, NULL);
            blog("main: FMOD JNI_OnLoad returned %d (0x%x)", v, v);
        } else {
            blog("main: FMOD JNI_OnLoad not found");
        }
    }

    /* Build AY: switch on FMOD's own debug logging, callback mode,
     * all levels + all types, forwarded to the bootlog — so FMOD's
     * init can say which output/plugin it cannot find (error 54). */
    {
        typedef int (*fmod_debug_cb_t)(unsigned int, const char *, int,
                                       const char *, const char *);
        typedef int (*dbg_init_fn)(unsigned int, int, fmod_debug_cb_t,
                                   const char *);
        dbg_init_fn din =
            (dbg_init_fn)get_sym(&mod_fmod, "FMOD_Debug_Initialize", 0);
        if (din) {
            int dr = din(0x000f0007u, 2,
                         (fmod_debug_cb_t)pt_fmod_debug_cb, NULL);
            blog("main: FMOD_Debug_Initialize -> %d", dr);
        } else {
            blog("main: FMOD_Debug_Initialize not found");
        }
    }

    // Pre-seed pthread key 0 for the main thread (see bionic_pthread.c for
    // the full story): the game's JNIEnv* getter expects [cell] == JNIEnv*,
    // but its simple thread-struct getter may claim key 0 first with a
    // zeroed cell. Seed it ourselves so onCreate's first getter call
    // finds a valid JNIEnv*.
    {
        void *cell = calloc(2049, 1);
        if (cell) {
            *(void **)cell = (void *)&jni;
            bionic_pthread_setspecific(0, cell);
            blog("main: pre-seeded pthread key 0 with cell %p (JNIEnv* %p)",
                 cell, (void *)&jni);
        }
    }

    // Input (touch + gamepad)
    blog("main: input_init...");
    input_init();
    blog("main: input_init done");

    // Graphics: vitaGL + GLES1.1 EGL context
    blog("main: graphics_init...");
    graphics_init();
    blog("main: graphics_init done");

    // Resolve native entry points
    blog("main: resolving JNI entry points...");
    N_onCreate = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onCreate", 1);
    N_onSurfaceCreated = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onSurfaceCreated", 1);
    N_onSurfaceChanged = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onSurfaceChanged", 1);
    N_onDrawFrame = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onDrawFrame", 1);
    N_setAcceleration = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_setAcceleration", 0);
    N_onPause = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onPause", 0);
    N_onResume = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onResume", 0);
    PT_onTouch = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onTouch", 0);
    PT_onGamepadButton = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onGamepadButton", 0);
    PT_onGamepadAxis = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onGamepadAxis", 0);
    /* Bug 34: the character channel for the keyboard bridge. */
    PT_onKeyDown = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onKeyDown", 0);
    blog("main: entry points resolved");

    // The native methods are static; pass a fake jclass (any pointer).
    // FindClass returns a strdup'd name which serves as the class token.
    void *clazz = jni->FindClass(&jni, "com/blitwise/engine/jni/CPJNILib");
    blog("main: FindClass -> %p", clazz);

    /* Build AU: observe the engine's FMOD boot chain (GOT interpose). */
    fmod_trace_install(mod_engine.text_base, mod_fmod.text_base);

    blog("main: calling onCreate...");
    N_onCreate(&jni, clazz);
    blog("main: onCreate returned");

    blog("main: calling onSurfaceCreated...");
    N_onSurfaceCreated(&jni, clazz);
    blog("main: onSurfaceCreated returned");

    blog("main: calling onSurfaceChanged(960, 544)...");
    N_onSurfaceChanged(&jni, clazz, 960, 544);
    blog("main: onSurfaceChanged returned");

    /* Build AI/AJ: the missing lifecycle call — the root cause of
     * every silent build. The engine's sound initialization lives
     * in CPJNILib_onResume (libengine 0x3d3c0c), which gates on its
     * state word and calls the FMOD setup fn (0x3d3f14 ->
     * FMOD_System_Create); this loader resolved the pointer years
     * ago but never invoked it. AI called it in strict Android
     * order (after onCreate, before the surface) and the run then
     * CRASHED inside onSurfaceChanged (bootlog-39): onResume sets
     * the engine's resumed state and builds sound state — at that
     * point half-built, with six CPJNISound methods unresolved —
     * which the surface path then tripped on. AJ therefore calls
     * onResume AFTER the surface is fully up (the surface completes
     * once in its known-good pre-resume state) and completes the
     * CPJNISound method table in jni_bridge.c. */
    blog("main: calling onResume...");
    if (N_onResume)
        N_onResume(&jni, clazz);
    blog("main: onResume returned");

    blog("main: entering frame loop");
    /* Bug 32-L: state the swap policy once, so the bootlog is
     * self-describing about whether the fix is live in this run. */
    blog("main: [loader] Bug32-L conditional swap %s",
         g_conditional_swap
             ? "ACTIVE: loader swaps only when the engine presented 0 times in the frame"
             : "INACTIVE: present hook SKIPPED, loader swap stays unconditional (pre-L behavior)");
    long long frame = 0;
    int last_r = 0;
    int have_last_r = 0;
    while (1) {
        /* Bug 32-J: publish the frame number for the tracer lines
         * and sample the frame tail's dispatch mode at the frame
         * boundary (tail_mode_check logs only on change; it is also
         * called inside dispatch_log_enter for the in-frame value). */
        g_frame_count = frame;
        tail_mode_check();

        input_poll();

        /* Bug 33 (Build N): sample the engine's input state once
         * per frame, after input_poll has queued this frame's
         * records and before onDrawFrame drains them; logs only
         * on change (rate-limited, capped). Read-only. */
        inputstate_check();

        /* Bug 33 (Build O): same change-only sampling for the view
         * singleton's transform fields, so an engine-side
         * re-derivation between scene arms is visible in the log. */
        viewstate_check();

        /* Bug 34 (Build U): service the keyboard bridge — open /
         * poll the Vita IME dialog when the engine has asked for
         * the keyboard, inject the finished text as onKeyDown
         * calls. Non-blocking; dialog calls never happen inside a
         * JNI handler. */
        keyboard_service();

        if (N_setAcceleration)
            N_setAcceleration(&jni, clazz, 0, 0.0f, 0.0f, 0.0f);

        /* Bug 32-L: snapshot the engine-present counter before the
         * frame; the delta across the call drives the swap decision
         * below. Presents during init-era calls are irrelevant —
         * only this call's delta is consulted. */
        unsigned presents_before = g_engine_present_count;
        int r = N_onDrawFrame(&jni, clazz);
        // Bug 32: the old rule logged only when r != 0, which made a
        // whole class of runs invisible — after the TITLE switch in
        // bootlog-9 no frame line appears past frame 12, and the log
        // cannot distinguish "onDrawFrame stopped returning" from
        // "it now returns 0". Log every CHANGE of the return value,
        // plus a heartbeat every 300 frames regardless of value
        // (interval raised from 60 in Build M, the cleanup build).
        if (!have_last_r || r != last_r) {
            blog("FRAME: onDrawFrame r changed -> %d at frame %lld", r, frame);
            last_r = r;
            have_last_r = 1;
        }

        /* Bug 32-L: THE FIX (bug32-analysis.md 5.3). The engine
         * already presented this frame (mode-3 tail present, plus
         * any bumper presents during an in-frame load) — a second,
         * loader-side swap would re-queue the OTHER display buffer,
         * which nothing has drawn into since frame 0, and two swaps
         * per iteration pin the draw target to a single buffer
         * forever. Swap ourselves only when the engine presented
         * zero times (clock-gated frames, non-presenting eras), so
         * every iteration performs exactly one swap and the draw
         * target alternates buffers. If the present hook was
         * SKIPPED at patch time, g_conditional_swap is 0 and this
         * stays the pre-L unconditional swap. */
        if (g_conditional_swap && g_engine_present_count != presents_before) {
            g_skipped_swap_count++;
        } else {
            graphics_swap();
            g_swap_count++;
            g_loader_swap_count++;
        }

        /* Bug 32-L: heartbeat extended with the fix's counters (all
         * cumulative since boot). Healthy TITLE era: engine_presents
         * advances ~1/frame, skipped tracks it, loader_swaps stays
         * flat, renders == render_returns and both advance ~1/frame.
         * swaps (the pre-L field) counts only real loader swaps, as
         * it always has. */
        if ((frame % 300) == 0)
            blog("FRAME: heartbeat frame=%lld r=%d swaps=%lld engine_presents=%u loader_swaps=%u skipped=%u renders=%u render_returns=%u",
                 frame, r, g_swap_count, g_engine_present_count,
                 g_loader_swap_count, g_skipped_swap_count,
                 g_render_enter_count, g_render_exit_count);

        /* Bug 33 (Build N): the input-state snapshot rides along
         * with the heartbeat (same 300-frame cadence, uncapped).
         * Build P: the ring census's per-type histogram rides
         * along too (bug33-analysis.md 27 item 2). Build Q: the
         * loader poll counters ride along as well (32 item 1). */
        if ((frame % 300) == 0) {
            inputstate_heartbeat();
            ring_census_heartbeat();
            poll_counters_heartbeat();
            /* Bug 37 (Build X): the latch readout rides along too;
             * it prints only during an unregistered-scene burst
             * (self-terminating, see scene_latch_heartbeat). */
            scene_latch_heartbeat();
        }

        frame++;
        if ((frame % 600) == 0)
            blog("main: frame %lld - still alive", frame);
    }

    blog("main: main returning (UNEXPECTED - loop should never exit)");
    bootlog_close();
    return 0;
}
