# Bug 33 — input is delivered, forwarded, and queued… and the menu never reacts

**Symptom (Lola, Build M):** the game runs at a solid 60 fps at the TITLE
menu, but touch *and* buttons produce zero reaction.

**Headline:** the input path is intact from the Vita hardware all the way
into the engine's central event queue, and the queue's only consumer (the
frame tail's Loop B) runs every frame. The path dies *inside* the tail's
per-record preprocessor `0x2220a8`, at a mode-3 early-exit: **if the
pointer at `[0x8D7E40]` is non-null, every event record of type 0–10 —
touch down/move/up and key records alike — is returned "consumed"
before the engine's global touch state is updated, before the scene's
`onEvent`, before everything.** That pointer belongs to the
load/transition machinery, and Bug 32 already proved TITLE's load never
completes in this port (its bulletin/moreGames payloads never settle) —
the same unfinished load that pins the tail mode at 3 also arms this
branch, because the branch only exists in mode 3. One stuck condition,
two symptoms: first the covered menu (fixed by the swap), now the deaf
menu. A second, independent break is *proven* for the buttons alone:
`onGamepadButton`'s Android-keycode map is built from
`android/view/KeyEvent` static fields resolved through FalsoJNI, whose
field table contains only the four `Rect` fields — every KEYCODE_*
lookup resolves to the default, the map never contains the codes
`input.c` sends, and every gamepad record is dropped before it is even
queued.

Sections: 1 path map · 2 loader side · 3 JNI readback · 4 the ring ·
5 Loop B · 6 the preprocessor and its gates · 7 why `onEvent` is a red
herring · 8 the gamepad break · 9 coordinates · 10 runtime evidence ·
11 ranked breaks · 12 next build.

---

## 1. The path, end to end

```
Vita touch/pad
  └─ loader input.c: input_poll() (called every frame, before onDrawFrame)
       ├─ touch → motionevent_update() → PT_onTouch(env, clazz, fakeEvent)
       └─ pad   → PT_onGamepadButton(env, clazz, androidKeycode, pressed, 0)
                  PT_onGamepadAxis(env, clazz, axis, value, 0)
  └─ libengine JNI entries (real exports, resolved by get_sym):
       onTouch         0x3d475c   onGamepadButton 0x3d559c   onGamepadAxis 0x3d58a9
       ├─ onTouch reads the MotionEvent back through the JNIEnv
       │    (CallIntMethodV / CallFloatMethodV wrappers 0x3d4bcc / 0x3d4c04)
       │    → FalsoJNI methods 200–206 (fake_motionevent.c state)
       ├─ both build a 20-byte event record on the stack
       └─ push 0x3d4c8c(record) → GLOBAL EVENT RING (ungated)
  └─ ring: buffer 0x902A50, 256 records × 100 B; state struct 0x904854
           (+0x3C mutex, +0x40 count, +0x44 read idx, +0x48 write idx)
  └─ frame tail 0x170620, Loop B (runs every frame, behind the clock gate):
       while (pop 0x3da134(&rec)) {            // sole pop caller in the binary
           0x224964(rec);                       // state-module notify thunk
           if (0x2220a8(rec) != 0) continue;    // PREPROCESSOR — the fork in the road
           if (byte[0x8D7D3F] == 0)
               scene->onEvent(rec);             // forwarder 0x2a8ea6, vtable +0x2C
           0x28d398(rec);                       // post-step (types 1–10 bookkeeping)
       }
  └─ menu interaction itself is POLL-based: TITLE's update (0x183368)
     and the Zoom widget tree read the global touch state the
     preprocessor maintains (struct ≈0x8D7EA6, +0x30 = x, +0x34 = y).
```

## 2. Loader side (input.c) — clean

* Entry points are the engine's real JNI exports, resolved at boot
  (bootlog-8/-14: `get_sym: …onTouch -> 0x993d475d`,
  `…onGamepadButton -> 0x993d559d`, `…onGamepadAxis -> 0x993d58a9`).
  They are direct native calls, not stubs.
* Touch: Vita reports (1920×1088) are halved to the game's 960×544
  space before forwarding; actions are the standard Android codes
  (DOWN 0, UP 1, MOVE 2, POINTER_DOWN/UP 5/6 with the pointer index in
  bits 8–15). `motionevent_update()` snapshots the pointer set *before*
  the call, and the fake event is read back synchronously inside
  `onTouch`, so the snapshot lifetime is correct.
* Buttons: `button_map` sends genuine Android keycodes (DPAD 19–22,
  BUTTON_A 96, B 97, X 99, Y 100, L1 102, R1 103, L3 106, R3 107,
  START 108, SELECT 109). Axes: X 0, Y 1, Z 11, RZ 14, with deadzone.
* Ordering: `input_poll()` runs immediately before `N_onDrawFrame` in
  the loader's frame loop, so records are in the ring before the tail
  drains it.

## 3. JNI readback (onTouch → fake MotionEvent) — clean

`onTouch` (0x3d475c) lazily caches seven method IDs in the engine
globals struct (base 0x90475C) at +0x40…+0x58, obtained via
`GetObjectClass(event)` + `GetMethodID`: **getX, getY, getPointerCount,
getAction, getPointerId, getActionIndex, getActionMasked** — by string
table at 0x65832B–0x658379. Those are exactly FalsoJNI IDs 200–206
registered in `jni_bridge.c`. The readback is sound because:

* FalsoJNI `GetMethodID` matches **by name only** (class ignored except
  for `<init>`), so the `falso_jni_dummy` class token returned by
  `GetObjectClass` is harmless here.
* The engine's int wrapper (0x3d4bcc) calls JNIEnv slot **0xC8 =
  CallIntMethodV** and the float wrapper (0x3d4c04) calls **0xE0 =
  CallFloatMethodV** (offsets verified against the vendored
  `jni.h` table: GetObjectClass 0x7C, GetMethodID 0x84 — both match
  the engine's usage). Both wrappers build a genuine `va_list` over
  the saved index argument, which is exactly what
  `motionevent_getX/getY(id, va_list)` consume via `va_arg`.
* Actions dispatch correctly: masked action 0/5 (DOWN/POINTER_DOWN) →
  record **type 8**; 2 (MOVE) → **type 9** (with per-pointer delta
  tracking against a last-position table); 1/3/6 (UP/CANCEL/
  POINTER_UP) → **type 10**. Record layout: +0 type, +0x0C x (u16),
  +0x0E y (u16) — the same layout the consumer side reads.

## 4. The ring — ungated, and the tail really drains it

Push `0x3d4c8c` (65 callers — it is the engine's central event bus, not
an input-only queue): mutex-lock, copy 20 bytes into
`ring[write_idx]`, count++ (clamped at 0x100), write idx wraps at
0xFE, unlock. **No focus check, no enable flag, no mode check.** If
`onTouch` runs, the record is in the ring.

Pop `0x3da134` is the mirror image (returns 0 on empty, else copies
`ring[read_idx]` out, count--, read idx advances). A full BL census of
the binary gives it **exactly one caller: the tail's Loop B at
0x17071A**. There is no second consumer and no other drain.

## 5. Loop B (tail 0x170620) — two gates, both now named

Per popped record, in order:

1. `0x224964(rec)` — a thunk to `0x1cd614(state+0x40, rec)`; a notify,
   its result is not consulted.
2. `0x2220a8(rec)` — the preprocessor (§6). **Nonzero return = record
   consumed; `onEvent` and the post-step are skipped** (`continue`).
3. Gate byte: `0x223aa4()` returns the byte at **[0x8D7D3F]**
   (state-module struct 0x8D7D2E, +0x11). `onEvent` is dispatched
   (forwarder 0x2a8ea6 → vtable +0x2C) **only when this byte is 0**.
4. `0x28d398(rec)` — post-step; maintains per-type bookkeeping for
   record types 1–10, itself enabled by a flag byte at [0x8DAF74].

Loop B sits *before* the tail's mode-gated dispatch block, so it runs
in every mode. Build M's heartbeats (11,400 frames, presents and
renders locked 1:1 with frames) prove the tail — Loop B included —
runs at full rate. The queue is being drained; the records die inside
step 2/3 or are never enqueued (gamepad, §8).

## 6. The preprocessor 0x2220a8 — where touch lives, and its trap door

The preprocessor has two jobs: a suite of debug hotkeys (key records,
type 1, with letter codes — '1','7','9','O','P','R','M','T','V','X',
'F','C'… behind modifier bits in record +8), and **maintaining the
global touch state** the scenes poll:

* Types 9 and 4, and types in mask 0x560 (5, 6, 8, 10) — i.e. every
  touch record onTouch emits (8 down, 9 move, 10 up) — store the
  record's x/y into the touch-state struct (≈0x8D7EA6) at **+0x30
  (x)** and **+0x34 (y)**. In mode 3 the raw pair is then replaced by
  a transformed pair via `0x2ad01c`, a pure function:
  `out = (in − view.origin) / view.scale` per axis, reading the view
  struct's doubles at +0x48/+0x50 (origin) and +0x68/+0x70 (scale).

**The trap door (head of the function, 0x2220C2–0x2220E0):**

```
mode = engine_state();                    // [globals+0x28] via 0x3d64f4
if (mode == 3) {
    if ([0x8D7E40] != NULL) {             // struct 0x8D7E28, +0x18
        if (rec->type < 0x0B)
            return 1;                     // CONSUMED — nothing else runs
    }
}
```

With that pointer set, the function returns "consumed" for every
record type 0–10 *before* the touch-state stores, before the hotkey
checks, before the fall-through consumer check at 0x22238C. Loop B
then skips `onEvent` too. Touch state freezes at whatever it last
held; a poll-based menu sees no press, ever. Key records (type 1)
from any source die the same way. (Gamepad records are types 12/13 —
they escape this branch, but they never arrive; §8.)

What is `[0x8D7E40]`? The struct at 0x8D7E28 is referenced by the
load/transition module (functions at 0x225008 — which stores two
pointer arguments into +0/+8 at setup — 0x22526E, 0x225640), by the
render path (0x22018C, 0x220324), and by the bumper/load cluster that
shares the 0x8D7Dxx–0x8D82xx BSS neighbourhood. In context it is the
**active load/transition context pointer**: set while a scene load or
transition is in flight, cleared when it completes. During a load,
swallowing input is correct behaviour. The branch also explains why
the swallow is total rather than partial: it is an input lockout for
transitions, not a filter.

Why it is the prime suspect here: Bug 32 established that in this
port **TITLE's load never completes** — the bulletin/moreGames
payloads can only settle via a Java async fetch that does not exist
here — and the tail mode is pinned at 3 (the "switch/loading" mode)
for the entire run, where stock TITLE settles to mode 2. The
early-exit branch requires mode == 3 *and* the pointer; our port
supplies the mode permanently. If the pointer is cleared by the same
load-completion that never fires, it is set right now. Static
analysis cannot read live BSS; §12's trace reads the pointer
directly.

Two lesser hazards in the same function, noted for completeness:

* The mode-3 transform (0x2ad01c) divides by the view struct's scale
  doubles. If that view was never registered (its +0x68/+0x70 scale
  still 0), the transformed coordinates become garbage/∞ and are
  stored over the good raw pair — hit-tests would fail even with the
  trap door open. Distinguishable by the trace: touch-state x/y would
  move with taps but hold nonsense values.
* A second sub-handler `0x282AE0` runs for every record when byte
  [0x8D7D40] (state +0x12) is set. Its return is discarded; it cannot
  gate Loop B. Not a suspect for the swallow.

## 7. onEvent is a red herring (for TITLE)

TITLE's vtable +0x2C resolves to **0x183820**, a 12-instruction stub:
it handles exactly one event — key record (type 1) with code
**0x1B (ESC/BACK)** — sets a byte, returns 1; everything else returns
0, and Loop B ignores the return value anyway. (Vtable cross-check:
+0x18 = 0x183368, TITLE's known update ✓; segment-mapped via PT_LOAD
vaddr/offset delta 0x1000.) So even a perfectly dispatched `onEvent`
would do nothing for menu touches: **menu interaction is poll-based**
through the touch state of §6, which is why the preprocessor — not the
dispatch — is where this bug lives. The [0x8D7D3F] gate byte (§5.3)
therefore matters little for the reported symptom; it would only
affect ESC handling and the scene-transition completer it also gates
at 0x170780 (and a TITLE transition *did* complete, suggesting the
byte is 0 — the trace will confirm).

## 8. The gamepad break — proven statically, loader-side fixable

`onGamepadButton` (0x3d559C) does **not** push unconditionally:

1. On first use it lazily builds a map (globals +0x5C) from the names
   in its string table: `KEYCODE_BUTTON_A/B/X/Y`, `KEYCODE_BUTTON_L1/
   R1`, `KEYCODE_BUTTON_START/SELECT`, `KEYCODE_BUTTON_THUMBL/
   THUMBR`, `KEYCODE_BUTTON_MODE`, `KEYCODE_DPAD_UP/DOWN/LEFT/RIGHT`
   (and `onGamepadAxis` does the same for `AXIS_X/Y/Z/RZ/BRAKE/GAS`
   on `android/view/MotionEvent`). For each name it calls
   `GetStaticFieldID(KeyEvent, name, "I")` (JNIEnv slot 0x240) and
   `GetStaticIntField` (slot 0x258) and inserts {value → engine
   button flag}.
2. **FalsoJNI's `GetStaticFieldID` is `getFieldIdByName`, and the
   bridge's `nameToFieldId` table contains only the four
   `android.graphics.Rect` fields (IDs 300–303).** Every KEYCODE_*/
   AXIS_* lookup returns NULL, and `GetStaticIntField` for an
   unregistered ID returns the bridge default. All names therefore
   collapse to the same key (0 or 1), and the inserts overwrite each
   other.
3. At press time the handler builds the record — **type 12 (down) /
   13 (up)**, keycode halfword at +4, pressed byte at +0xC — then
   looks the incoming Android keycode up in the map
   (0x3D5864). `input.c` sends 96/97/99/100/…; the map holds none of
   them; the lookup returns a null node; at 0x3D56F6 the push is
   skipped (`cbz r0 → 0x3D56FE`). **No gamepad record is ever
   queued.** This also fully explains why Build F's INPUT lines
   (logged in the loader, before the call) showed button events
   "reaching the engine" while nothing ever reacted: they reach the
   JNI entry and die at the map.

The fix is pure bridge data (§12): register the static int fields
with their true Android values — BUTTON_A 96, B 97, X 99, Y 100,
L1 102, R1 103, THUMBL 106, THUMBR 107, START 108, SELECT 109,
MODE 110, DPAD_UP 19, DOWN 20, LEFT 21, RIGHT 22; AXIS_X 0, Y 1,
Z 11, RZ 14, BRAKE 23, GAS 22 — exactly the codes `input.c` already
sends. (The Vita has no MODE button; registering it is harmless.)
Caveat: even with records flowing, TITLE's menu may still ignore
buttons on stock behaviour — Pocket Tanks' menus are touch-driven —
but in-game button use depends on this path, and the record drop is
a defect regardless of the menu question.

## 9. Coordinates — not the problem

* Surface is 960×544, confirmed twice (log-11: `onSurfaceChanged -
  960, 544`, `OpenGL: Set(960, 544)`); the dims getters the tail
  caches (0x3D60F0/0x3D60D8) return that size, and the bridge
  registers the safe-area Rect as 960×544.
* The loader forwards taps in that same space; Lola's logged taps
  (484,378), (482,376) are sane near-centre menu coordinates.
* A coordinate-space error would misplace hits, not abolish them —
  and buttons carry no coordinates yet are equally dead. Total
  deafness across both families requires a shared gate, which is
  what §6/§8 provide. (The one coordinate hazard that *could* be
  total — the zero-scale transform — is tracked as break #3.)

## 10. Runtime evidence folded in

* Build M heartbeats are perfect across 11,400 frames
  (engine_presents == frames+19, loader_swaps flat at 1, renders ==
  render_returns == frames−1): the tail, Loop B included, executes
  at 60 fps. The consumer *runs*; consumption is what fails.
* log-11 (17 lines) contains zero input/focus/view lines — the engine
  simply has no logging on this path, so the game log can never
  arbitrate; only a consumer-side trace can (§12).
* bootlog-14's INPUT lines + the get_sym resolutions prove the
  forwarding calls happen with real coordinates and codes;
  bootlog-15 (Build M) stripped the INPUT lines but `input.c` is
  byte-identical to Build L's, so forwarding stands proven for M.

## 11. Ranked breaks

1. **`[0x8D7E40]` stuck non-null (touch + any type-1 keys).**
   Mechanism proven in code (§6); stuck-ness inferred from Bug 32's
   never-completing TITLE load and the permanently pinned mode 3
   that arms the branch. Explains total touch deafness at the menu
   in one step. *Confidence: high, pending the §12 readout.*
2. **Gamepad records never queued — FalsoJNI KeyEvent fields
   unregistered (buttons).** Proven end-to-end statically (§8):
   field resolution collapses → map lookup misses → push skipped at
   0x3D56F6. Independent of #1; would persist after #1 is fixed.
   *Confidence: proven (code-complete); runtime confirmation rides
   the same next build.*
3. **Zero-scale view transform (touch, alternate).** If the view
   struct behind 0x2AD01C was never initialised, mode-3 transformed
   coordinates are garbage and hit-tests fail despite the touch
   state updating. *Confidence: speculative; the trace separates it
   from #1 by whether touch-state x/y track taps sanely.*
4. **Gate byte [0x8D7D3F] stuck at 1.** Would block `onEvent`
   dispatch and the transition completer — but TITLE's onEvent is
   ESC-only and a transition did complete. *Confidence: low for this
   symptom; read by the trace for free.*
5. Ruled out: loader forwarding (§2), JNI readback (§3), the ring
   and its drain (§4, §10), the tail's execution (§10), coordinate
   space (§9), `onEvent` handling at TITLE (§7).

## 12. Next build (Build N) — consumer trace, loader-side only

No engine patching needed: the loader already reads engine BSS
directly (the Build J/L tail-mode print via
`kuKernelCpuUnrestrictedMemcpy` is the precedent). All addresses are
ELF VAs; runtime address = `mod_engine.text_base + VA`.

**Trace (sample at each heartbeat and on any change):**

```
INQ: count=%d rd=%d wr=%d            // 0x904894 / 0x904898 / 0x90489C
ING: ptr8d7e40=%p byte11=%d byte12=%d // 0x8D7E40 / 0x8D7D3F / 0x8D7D40
INT: x=%d y=%d                        // 0x8D7ED6 / 0x8D7EDA (touch state +0x30/+0x34)
```

Read during a run where Lola taps the menu centre several times:

* count oscillates 0↔n with taps and `ptr8d7e40 != 0` → **break #1
  convicted**; x/y frozen confirms the swallow point.
* count oscillates, ptr == 0, x/y track the taps in 960×544 →
  input path fully alive; the break is downstream in the scene's
  widget poll (new pass).
* count oscillates, ptr == 0, x/y garbage/huge → **break #3**
  (transform scale); inspect the view struct at 0x7FE648.
* count never moves on taps → onTouch delivery broken after all
  (would contradict §3/§10; re-open the JNI readback).

**Then Build N+1 (fix-shaped by N's readout):**

* If #1: when the SCENE hook sees TITLE arm, zero `[0x8D7E40]` once
  and log it (`ING: cleared load-context pointer at TITLE`) — the
  loader-side equivalent of delivering the load completion the
  engine is waiting for; keep a per-frame watch that logs if it is
  ever re-set. If the menu then answers, the permanent fix is to
  clear it at the same place the engine clears it on completion
  (or on every TITLE arm).
* Regardless of #1: register the KeyEvent/MotionEvent static int
  fields in `jni_bridge.c` (`nameToFieldId` + `fieldsInt`, new ID
  block, e.g. 400+) with the true Android values listed in §8, so
  `GetStaticFieldID`/`GetStaticIntField` return real codes and the
  gamepad map populates. Verify in N+1's log by a button press
  producing a queued record (queue count delta) — and by Lola's
  report of whether buttons now do anything, in menu or in game.

---

# Pass 2 — the view registration (written after Build N's run)

**Runtime verdict that frames this pass (bootlog-16, Build N):**
break #1 is **acquitted** — `[0x8D7E40]` is `0x0` in all 162
INPUTSTATE samples; the trap door never fires. The ring flows and
drains continuously (~4 records/frame, rd/wr advancing in lockstep
for 12,300+ frames). What the menu polls is garbage: touch_x ≈
−1,491,xxx with small tap-correlated jitter (40 distinct values,
span ≈760, pairwise-diff gcd 1), touch_y pinned at exactly 65535
in 161 of 162 samples (the exception is the pre-first-record
baseline). So the touch state **is** being written, by the mode-3
transform, with values no hit-test can match. Break #3 is the live
one. This pass dissects the transform (§13), the view object and
its registration machinery (§14–§15), decodes the garbage as far
as static evidence honestly goes (§16), settles the `[0x8D7D3F]`
side check (§17), and specifies the fix build (§18).

## 13. The transform 0x2ad01c — exact arithmetic

A pure function of three pointers: `r0` = out (int32 pair),
`r1` = view, `r2` = in (int32 pair — the touch state's just-stored
raw halfwords, zero-extended). Per axis (x shown; y identical with
+0x50/+0x70 and `[r2,#4]`):

```
vldr  s0, [r2]              ; in_x as raw int32 bits
vldr  d16, [r1, #0x48]      ; view.origin_x (double)
vldr  d17, [r1, #0x68]      ; view.scale_x  (double)
vcvt.f64.s32 d18, s0        ; (double)in_x
vsub.f64 d16, d18, d16      ; in_x − origin_x
vdiv.f64 d16, d16, d17      ;        / scale_x
vcvt.s32.f64 s0, d16        ; saturating trunc-to-int32
vstr  s0, [r0]              ; out_x
```

So `out = (int32)((in − origin) / scale)` in double precision,
**divide** (not multiply), origin/scale are **doubles**. Both
preprocessor call sites (types 9/4 at 0x222272; mask-0x560 types
at 0x2222b6) pass `r2` = &touch_state.xy *after* the raw store and
`r1` = the singleton view computed PC-relative as **0x7FE648**
(both sites verified: 0x222270+0x5DC3D8 and 0x2222B4+0x5DC394);
the result is written back over the raw pair with
`strd r0,r1,[r6,#0x30]`. For a 960×544 surface the healthy output
range is the surface itself — widget hit-tests consume this pair
in surface space (on stock the mode-2 path stores the raw pair
untransformed, §6).

Two sibling forms exist (0x2ad05e: double in/out; 0x2ad0aa: int
out with +0.5 rounding) — 30/36/61 BL callers across scenes and
the Zoom core; each caller passes *its own* view in `r1`. The
preprocessor is the only consumer hard-wired to the singleton,
because input is global, not per-scene.

## 14. The view object at 0x7FE648 — layout and lifecycle

A large object (fields past +0xA00) in BSS — so its file image is
all zeros and only runtime writes give it content. Field map
established from the method cluster at 0x2aaa80–0x2aae6c:

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| +0x0C | int | flags/mode arg | configure stores arg r3; getter 0x2aae58 |
| +0x1C/+0x20 | int | width / height | configure `strd`; getters 0x2aae50/54 |
| +0x28 | double | width/height ratio | configure `vdiv` |
| +0x30/+0x34 | int | design pair (stack args) | configure stores; getters 0x2aae5c/60 |
| +0x38/+0x3C | int | derived origin (int form) | apply stores vcvt results |
| +0x48/+0x50 | double | **origin** | transform reads; reset zeroes |
| +0x58/+0x60 | double | second derived pair | apply copies from +0xB8 |
| +0x68/+0x70 | double | **scale** | transform reads; getter 0x2aae64 (+0x68) |
| +0x78, +0xD8, +0x158, +0x9E0, +0xA00 | sub-objects | strings/maps/nodes | ctor/reset call their ctors |

Lifecycle methods:

* **Ctor (0x2aaa80)** — zeroes head fields, constructs the
  +0xA00 sub-object, calls reset.
* **Reset (0x2aaae4)** — zeroes +0x28..+0x78 in blocks; in
  particular **origin = (0.0, 0.0) and scale = (0.0, 0.0)**.
  The reset state is *not* identity: a constructed-but-never-
  configured view divides by zero.
* **Configure (0x2aac30)** — `configure(this, w, h, argC,
  stackA, stackB)`: list teardown (0x2aabd8), reset, then
  +0x1C=w, +0x20=h, +0x28=(double)w/(double)h, +0x0C=argC,
  +0x30=stackA, +0x34=stackB, and a tail call to apply.
* **Apply (0x2aacb4)** — derives the live transform: reads the
  display rect via **0x223040** (globals dims × safe-area
  fractions when state byte [0x8D7D57] is set, else the JNI
  `getSafeArea` rect through glue 0x3d61d4 — which pre-fills
  (0,0,960,544) from globals+0x38/+0x3C before the JNI call, so
  the rect is defended even if the JNI answer is a dummy) and
  the dims getters (0x3d60f0/0x3d60d8), runs two math helpers
  (0x34cfc6, 0x34d0f8), and copies the computed block into
  place: **origin ← [+0xA8..], scale ← [+0xC8]/[+0xD0]**,
  int origin ← vcvt at +0x38/+0x3C.

## 15. Who configures the singleton — census and audit

Complete BL census of the lifecycle methods (objdump, whole
binary). **No configure or apply call anywhere targets 0x7FE648
by address.** Every site configures a different instance,
typically *cloning* a master view's getters into it:

| Site | `this` configured | Values |
|---|---|---|
| 0x16f046 (scene load) | heap view via cell [0x701DD0] | 800×480, argC 1024 |
| 0x16f076 (scene load) | static 0x7F6738 | 1920×1080, argC 1024 |
| 0x16f35c (scene load) | static ≈0x7F7AF0 | 800×480, argC 1024 |
| 0x183e76 (TITLE setup) | TITLE scene member view | 1920×1080, argC 512 |
| 0x231770 | static 0x8D8248 | cloned from master via cell [0x701ECE] |
| 0x257642, 0x36807e, 0x369694, 0x371730 | heap/embedded views | cloned from object-member masters |
| 0x36b58e | (same clone pattern) | — |

Ctor sites for the singleton itself: exactly one — inside a
property-tree builder at ≈0x87048 (0x86xxx–0x87xxx game module),
which computes 0x7FE648 and calls the ctor on it. **Construction
happens; configuration by address never does.** The singleton's
transform fields can therefore only be written (a) by apply
invoked through a *pointer* (the clone sites pass pointers loaded
from cells/objects, so a pointer-mediated apply on the singleton
cannot be excluded by the address census), or (b) by the Zoom
stage machinery (0x21fxxx) that uses the singleton — the stage
function at 0x21f3b2–0x21f6e0 reads a **named property**
(0x1f6294) before computing with the singleton's getters and
transform, i.e. its inputs are property-tree-fed — and this
port's property tree has *proven* missing nodes (Bug 32 §3:
bulletin/moreGames). Static analysis cannot follow the pointer
mediation to ground; §18 measures the fields directly.

**Loader audit (the other half of the question).** Our loader's
lifecycle is complete and correctly ordered — `main.c`:
JNI_OnLoad → pthread key-0 seed → `input_init()` →
`graphics_init()` → `onCreate` → `onSurfaceCreated` →
`onSurfaceChanged(960, 544)` → frame loop. No lifecycle entry
exists that we omit (the full JNI export list has no separate
view-registration native). And `onSurfaceChanged` itself does
no native view registration on stock either: it stores (w,h) to
globals+0x38/+0x3C, sets globals byte +8, runs its JNI method-ID
caching ritual, sets engine state 3 (setter 0x3d4188), and fires
a core callback pointer held at globals+0x14. Surface size
reaches the core through globals + that callback — all
engine-internal, all triggered identically by our call.

**Conclusion:** there is no missing loader call to add. The
divergences are (1) the tail mode pinned at 3 (Bug 32's
unfinished TITLE load), which makes the mode-3 transform branch
*live at the menu at all* — on stock, TITLE settles to mode 2
and this branch never runs there; and (2) whatever the
pointer-mediated, property-fed stage machinery computed into
the singleton's fields in this environment (§16).

## 16. Decoding the garbage — what the numbers prove and refute

**Scale = 0 is refuted.** With origin = scale = (0,0) (the reset
state), `vcvt.s32.f64` saturates: in>0 gives (in−0)/0 = +∞ →
0x7FFFFFFF; in=0 gives 0/0 = NaN → 0. Observed x ≈ −1.49M is
neither, and no tap could change it. The fields are **not** in
the reset state at runtime — something wrote non-trivial
doubles.

**Random garbage is refuted.** The 40 distinct x values form a
smooth near-continuous lattice (diffs of 1, 2, 4, 5, 6, 8…; gcd
1) over a ≈760-wide span — the fingerprint of a *fixed* affine
map applied to a *varying* integer input (her taps + drag). A
fixed map means origin/scale are constant through the TITLE era.

**The x axis** fits a coherent-but-wrong affine: out span / in
span bounds |1/scale_x| ∈ [0.79, 2], and origin_x/scale_x ≈
+1.4915×10⁶ (e.g. scale_x ≈ 1 with origin_x ≈ +1,491,5xx, or
scale_x ≈ 2 with origin_x ≈ +2,983,xxx). No surface- or
design-sized quantity lives at 1.49M px; it reads as a value
*computed* from wrong inputs (a property-fed design size /
accumulated offset), not as an uninitialised field.

**The y axis** is degenerate in a different way: out_y = 65535
for *every* input. Under the same formula that requires either
scale_y ≳ 544 with origin_y ≈ −65535·scale_y, or an effectively
constant in_y. The two axes' fields were evidently filled from
different wrong sources — consistent with a computed transform
whose inputs (property values / rect / design pair) are wrong
in this port, not with a single clean view in any state.

**Bounded honestly:** the exact doubles, and which
pointer-mediated writer produced them, are not statically
closable — BSS holds no file image and the writer is reached
through runtime pointers. The fix build (§18) reads the four
doubles out before touching them, closing the decode
empirically in the same run that tests the fix.

## 17. Side check — byte [0x8D7D3F] is 1, and it does not matter here

bootlog-16: `b3f=0` in the first sample only, **1 in all 161
later samples** — it sets during init and never clears (this
corrects §7's guess that a completed transition implied 0).

Relevance audit: the byte (state struct 0x8D7D2E +0x11, getter
0x223aa4) is consulted at exactly two places — Loop B step 3
(gates the scene `onEvent` dispatch) and the tail at 0x170780
(gates the scene-transition completer). **The touch poll path
never reads it**: the preprocessor's stores and transform consult
only the mode, `[0x8D7E40]` (§6, acquitted) and `[0x8D7D40]` (the
discarded-return sub-handler enable). TITLE's interaction is
poll-based (§7), so `b3f=1` is **confirmed irrelevant to the
touch symptom**. It is not cost-free in general — `onEvent`
(incl. TITLE's ESC stub) is suppressed for the whole TITLE era,
and the transition completer is gated off (the TITLE transition
completed regardless, evidently before the byte set or by
another path) — a latent defect to remember for scenes whose
`onEvent` matters (in-game back/pause), but not this bug.

## 18. Fix build (Build O) — specified

Loader-side only; no engine patching. Base = Build N (all fixes,
conditional swap, interposition code path, INPUTSTATE trace).

1. **VIEWSTATE measurement (closes §16 empirically).** At TITLE
   arm and on any change (sampled with the INPUTSTATE sampler),
   read the singleton's fields and log them:
   `VIEWSTATE: w=%d h=%d ratio=%f o=(%f,%f) s=(%f,%f)` from
   0x7FE648 +0x1C/+0x20 (ints), +0x28, +0x48/+0x50, +0x68/+0x70
   (doubles, via the same unrestricted-memcpy read Build N uses).
2. **The view write.** In the SCENE hook's TITLE arm (the
   `g_title_trace_armed` precedent), after the VIEWSTATE
   pre-values are logged, write the identity surface transform
   into the singleton: **origin=(0.0, 0.0) at +0x48/+0x50,
   scale=(1.0, 1.0) at +0x68/+0x70** (doubles, loader BSS write),
   and log `VIEWFIX: set identity at TITLE (was o=(%f,%f)
   s=(%f,%f))`. Re-assert on *every* SCENE arm, not just TITLE —
   the poll state's contract ("raw surface px", exactly what
   stock's mode-2 path stores) is per-scene, and any scene load
   that re-derives the singleton gets the same correction.
   Rationale: with the transform reduced to identity, mode 3 in
   this port delivers to the widgets *precisely* what stock
   mode 2 delivers. If the pre-values logged in step 1 turn out
   sane (scale within [0.25, 4], |origin| ≤ surface dims) **and**
   the menu still does not respond, the write is exonerated by
   its own log and the break is downstream in the widget poll —
   revert the write next build.
3. **KeyEvent/AXIS registration (buttons, from §8).** In
   `jni_bridge.c`, register the static int fields with true
   Android values in a new ID block (400+), in both
   `nameToFieldId` and `fieldsInt`: KEYCODE_BUTTON_A 96, B 97,
   X 99, Y 100, L1 102, R1 103, THUMBL 106, THUMBR 107, START
   108, SELECT 109, MODE 110, DPAD_UP 19, DOWN 20, LEFT 21,
   RIGHT 22; AXIS_X 0, Y 1, Z 11, RZ 14, BRAKE 23, GAS 22.
4. **Distinguishing the two changes in the next bootlog** —
   they are mechanically disjoint (engine-BSS doubles vs
   FalsoJNI field table), separately gated, and separately
   logged: touch recovery shows as INPUTSTATE touch_x/y snapping
   to sane surface coordinates tracking Lola's taps (plus her
   report); button recovery shows as queue activity on presses
   alone — per §8 the gamepad records (types 12/13) never touch
   the preprocessor's touch branch, so rd/wr deltas on a
   button-only test isolate the field fix. **Interaction risk:**
   the only shared surface is the singleton's other readers
   (stage layout conversions via 0x2ad01c, e.g. 0x21f664);
   rendering is demonstrably independent of these fields
   (scenes render correctly today with garbage in them), so
   writing identity at scene arms cannot regress the picture —
   and if it somehow did, Lola's screen arbitrates and the
   write is one revertible loader change.

---

# Pass 3 — the transform's true inputs (2026-10-13, after Build O / bootlog-17)

## 19. Verdict up front — the pass-3 premise is REFUTED; the sampler was misaligned

The tasking premise ("fn 0x2ad01c does not read the global singleton; the transform
reads a different view than the one we patched") is **false**, and the measurement
that forced it was an artifact of our own tracer. Three findings, in order:

1. **0x2ad01c reads exactly the view Build O patched.** Re-disassembled in full
   (§21): it takes the view pointer as an argument and reads only that object's
   fields. The preprocessor passes the global singleton **0x7FE648** at both call
   sites. There is no scene-local view anywhere in the record path.
2. **Builds N and O sampled the touch pair two bytes low.** A PIC-base derivation
   error (the `Align(PC,4)` formula, §20) put the touch struct at 0x8D7EA6 instead
   of **0x8D7EA8**; the sampler read 0x8D7ED6/0x8D7EDA instead of the true poll
   pair at **0x8D7ED8/0x8D7EDC**. The "garbage" (−1,491,xxx / 65535) is a
   misaligned-read artifact and decodes exactly (§22).
3. **Decoded, the true pair behaves perfectly — on the wrong records.** Build N:
   the pair is (−23, 0) for the entire run = the letterbox transform of (0, 0).
   Build O: (−23, 0) until ~frame 1100, then (0, 0) = the identity transform of
   (0, 0) — i.e. the pair tracked Build O's singleton write precisely, flipping
   at TITLE load completion. The transform + singleton + preprocessor path is
   alive and correct; it is being fed records whose coordinates are (0, 0), and
   her taps' transforms never once appear as the resting state in either log.
   Where they die is §24 — two candidate mechanisms survive; the next build's
   ring census (§27) discriminates them in one run.

## 20. The PIC convention correction (affects every pass-1/2 address at ≡2 sites)

For `ldr rd, [pc, #imm]; add rd, pc` sequences in this binary, the base is
**raw (add address + 4) + literal** — NOT `Align(PC,4) + literal`. Decisive
cross-checks, all formula-independent anchors hit only by the raw rule:

- Bumper cluster: add @0x222fca (≡2) → raw **0x8D81D0**; its +0x38 = **0x8D8208**
  = the bumper texture cell established in the Bug 32 passes. (Align gives
  0x8D81CE, +0x38 = 0x8D8206 — wrong.)
- Load context: add @0x222110 (≡2) → raw **0x8D7E28**; its +0x18 = **0x8D7E40**
  = the gate-pointer cell the loader already reads correctly (§13 stood).
- Two scene-load functions compute the input-struct base at ≡0 sites (formula-
  independent) and land on **0x8D7EA8**: fn 0x2229cc (add @0x2229e8) and the
  0x223a08/1c/30 getter cluster's raw values agree. Align gives 0x8D7EA6 — a
  non-4-aligned base for a struct full of doubles; impossible.

Corrected addresses (pass-1/2 values in parentheses):

| Cell | Correct | Was |
|---|---|---|
| touch/input struct base | **0x8D7EA8** | 0x8D7EA6 |
| raw coord halfwords (+0x2C/+0x2E) | **0x8D7ED4 / 0x8D7ED6** | — |
| poll pair (+0x30/+0x34) | **0x8D7ED8 / 0x8D7EDC** | 0x8D7ED6 / 0x8D7EDA |
| STATE struct base | **0x8D7D30** | 0x8D7D2E |
| Loop B drain-gate byte (STATE+0x11) | **0x8D7D41** | sampled as 0x8D7D3F |
| preprocessor sub-handler enable (STATE+0x12) | **0x8D7D42** | sampled as 0x8D7D40 |
| GLOBALS base (mode at +0x24) | **0x90475C** | 0x90475A |
| LOADCTX / gate-ptr cell | 0x8D7E28 / [0x8D7E40] | confirmed |
| view singleton | 0x7FE648 | confirmed |

Consequences: builds N/O's `b3f`/`b40` bytes were STATE+0xF/+0x10, **not** the
gate/enable bytes — the byte that flipped 0→1 during load is STATE+0xF
(unidentified). The true gate (0x8D7D41) and enable (0x8D7D42) have never been
measured. (The drain demonstrably runs in TITLE — ring rd tracks wr all run —
so the gate must be open; the +0x12 enable for sub-handler 0x282ae0 remains
genuinely unknown.) VIEW 0x7FE648 and LOADCTX derivations were ≡0-site or
cell-based and stand. VIEWSTATE in builds N/O read the singleton directly and
was always correct.

## 21. Q1 — what 0x2ad01c actually reads; the store path re-dumped

**0x2ad01c (full semantics, re-verified):** inputs r0 = packed point (x = low
halfword, y = high halfword), r1 = view object. It reads ONLY r1's fields:
origin doubles at **+0x48 (x) / +0x50 (y)**, scale doubles at **+0x68 (x) /
+0x70 (y)**; computes per axis `out = (int32)((in − origin) / scale)` in double
precision (truncating conversion); packs the two int32 results into r0. No
globals, no scene object, no pointer chain. Its int-output siblings: 0x2ad05e
(double in/out, FORWARD `out = origin + in·scale`), 0x2ad080 (double in/out,
inverse). View lifecycle unchanged from §16 (ctor 0x2aaa80, reset 0x2aaae4 =
origin/scale zeroed, configure 0x2aac30, apply 0x2aacb4).

**Preprocessor 0x2220a8, mode-3 store path (re-dumped with corrected bases):**
sl = input struct 0x8D7EA8 (add @0x2221b2), r6 = STATE 0x8D7D30 (add @0x2220cc),
view = singleton [0x7FE648] loaded at 0x2221c4–0x2221d0.

- Trap door (§6) stands, addresses corrected: mode==3 (STATE+0x24), gate ptr
  [0x8D7E40] ≠ 0, record type ≤ 10 → return consumed before any store. The
  loader's gate_ptr = 0x0 in all 324 samples across both runs still acquits it.
- Raw store (0x2221e4–0x222202): record +0x0C/+0x0E halfwords → input +0x2C/+0x2E
  (`strh`), unconditionally, before branching.
- Site 1 (0x222272, types **9 and 4**): transform(record x, record y) with the
  singleton → `strd` to input **+0x30**.
- Site 2 (0x2222b6, bitmask types incl. **5, 6, 8, 10**): reloads the raw pair
  (`ldrd [sl, #0x2c]` @0x222296), transforms it with the singleton → input +0x30.
- Post-step 0x28d398 runs per record for types 1–10 (after the onEvent gate).

**Q2 (scene-local view) is therefore closed:** there is none in this path. The
per-scene Zoom objects of Bug 32 §4.1 exist, but the record preprocessor never
touches them. TITLE's singleton was perfect from the INTRO2 arm onward
(bootlog-17 VIEWSTATE: 800×480, origin (27.166667, 0.5), scale 1.133333) — the
global was never the problem.

## 22. The arithmetic proof, and the decode of both logs

With the letterbox singleton, the preprocessor's touch branch can only output
x = (u16 − 27.1667)/1.13333 ∈ **[−23, 57816]** and y ∈ **[0, 57816]** for any
record; the raw store is a zero-extended u16. The sampled −1,491,xxx and 65535
are **impossible** outputs of that branch — proof by arithmetic that the
sampled cells were not the branch's outputs. With the §20 correction they
decode exactly (sampled_x @+0x2E = rawY_u16 | (true_x << 16); sampled_y @+0x32
= (true_x >> 16) | (true_y << 16)):

- **Initial −1,507,328 = −0x170000** = (−23 << 16) with the +0x2E halfword still
  0: true_x = **−23** = trunc((0 − 27.1667)/1.13333) — the letterbox transform
  of input **0**. true_y = 0 = trunc((0 − 0.5)/1.13333). The pair held
  transform(0, 0) from the first frames, before any tap.
- **Build N, whole run:** true pair **(−23, 0)** in all 162 samples.
- **Build O:** (−23, 0) through frame ~1025; **(0, 0)** from frame ~1200 to the
  end (10,200 frames). The flip is NOT at a SCENE arm (INTRO2/TITLE requests are
  both before frame 900); it lands ~150–300 frames after the TITLE request =
  **TITLE load completion**, when the load machinery (fns 0x21f318 / 0x2229cc,
  §18) refreshes the input struct's derived fields and the resting pointer state
  is re-derived under the then-current singleton — identity in O, letterbox in
  N. This is direct runtime proof that Build O's write DID feed the transform
  path that maintains the resting pair.
- **The jitter is not in the pair at all.** Build N's sampled_x family
  (−1,491,072…−1,491,840) and both runs' late family (15512…16240) are the
  **+0x2E halfword** (the raw-Y slot) varying while true_x stays constant. Its
  values (stationary clusters 16208/16224/16232; a 15547–15602 family while she
  drags) are not a coordinate (raw Y would be ≤ 544) — the field is overwritten
  by a later per-record writer (post-step 0x28d398 or the drain bookkeeping) with
  a narrow-band, tap-modulated quantity, identity unknown. It is the only
  tap-correlated signal in either log, and it is a red herring for position.

## 23. What is now proven about the tap path (and two pass-1/2 corrections)

- **The JNI readback chain is sound, end to end (re-verified this pass).**
  onTouch calls the engine's variadic JNI thunks (CallIntMethodV 0x3d4bcc,
  CallFloatMethodV 0x3d4c04), which build genuine va_lists from the by-value
  argument: the thunk stores the incoming 4th register (the pointer index) into
  its frame and passes a va_list pointing at it, then calls env vtable slot
  0xC8/0xE0. FalsoJNI forwards that va_list to the registered impl;
  `motionevent_getX/getY` consume the index and return the floats input.c
  stored microseconds earlier. onTouch converts (vcvt.u32) and stores them as
  the record's +0x0C/+0x0E halfwords; record types are 8 (DOWN, bitmask
  {0,5} path), 10 (UP, {1,3,6} path), 9 (MOVE, pooled path) — all as §3 said.
  **Her real coordinates do enter the ring.** The "readback returns (0,0)"
  theory is dead.
- **Axis records cannot write the pair.** onGamepadAxis (0x3d58a9) pushes
  **type 14** records (record built at 0x3d597a–0x3d59a4: type 0xE, +0x0C =
  scaled axis value halfword). Type 14 is in neither transform branch.
- **Pass-1 correction:** push 0x3d4c8c has ~50 call sites, not 4 — including the
  game module (0x152xxx cluster), a large translator cluster at 0x22b5dc–
  0x22c00c, state-module functions (0x22593e, 0x226f12), other JNI entries
  (0x3d3b6a, and onGamepadButton's three sites at 0x3d4f74/8e/a2), and the tail
  itself (0x170708, the producer pump below).
- **Pass-2 correction:** ring churn in the TITLE era is **~0.4–0.5
  records/frame**, not ~4/frame (Build O: 144 records by frame 900; ~3,900 by
  frame 9,000). onGamepadAxis evidently pushes only on value change; the
  per-frame 4 axis calls from input.c mostly produce nothing. The ring is
  quiet — which sharpens §24: her tap is usually the ONLY touch-branch record
  in its drain, yet its store never survives to a single 1 Hz sample.
- **The internal producer (new).** Before each drain, the tail pumps an
  engine-internal queue into the ring: forwarder 0x224954 →
  0x1ce114(obj = **0x8D7F6C**), looping until empty, pushing each record
  (0x170708); the drain's per-record bookkeeping is 0x224964 → 0x1cd614(obj =
  **0x8D7F60**). Both objects live in a struct at **0x8D7F20** (call it INPUTQ),
  distinct from the input struct. Its fillers are engine/core-side; the loader
  never touches it. Because the pump runs inside the frame AFTER input_poll,
  any producer record drains AFTER her same-frame records (FIFO).

## 24. Q3 — where her taps die: two surviving mechanisms

The resting pair is transform(0, 0) at every sample of both runs, and is
re-derived at scene-load completion. Her records carry real coordinates (§23)
and the branch that would store their transforms is acquitted of the trap door
(§21). So either her stores are overwritten within the same drain, every time,
or the menu never reads the pair her stores land in.

**(A) Producer overwrite.** INPUTQ emits touch-branch records carrying (0, 0) —
most plausibly type 4, the "pointer position report" that shares site 1 with
MOVE — sourced from the core's pointer state, which sits at (0, 0) because the
core's own input feed (its Java view on stock Android) does not exist in this
port. Pump order (producer records drain after hers, §23) makes every tap's
store die in its own frame. Under (A), the one tap per run that Lola sees
register is the tap whose drain batch happens to contain no trailing
touch-branch producer record — most plausibly around the scene transition /
TITLE load window, where producer timing differs. Weakness: the ring is quiet
(§23), so the producer must emit sparsely yet coincide with her taps — it
would have to emit BECAUSE of input activity (e.g. the core echoing a pointer
sync per input frame), which is plausible but unproven.

**(B) The menu polls the core, not the int pair — LEADING hypothesis.** The
input struct's position interface is double-based: getters 0x223a08 / 0x223a1c
/ 0x223a30 return the doubles at +0x40 / +0x48 / +0x50 and have **no callers
anywhere in libengine** (nor does anything in libengine write +0x40/+0x48/
+0x50 — full accessor census this pass: preprocessor writes +0x2C/+0x30;
load fns write +0x20/+0x38 and zero +0x18/+0x28/+0x58; setters write +0x68
(0x2236c8, at scene load) and +0x70 (0x2237e8, at scene load); the resize fn
writes +0x60; the per-frame tick 0x222ea0 refreshes the struct head +0x00–
+0x0F and publishes &struct into STATE2+0x18). The doubles are written and read
from the core side (libc++_shared) through an interface this pass did not
disassemble. TITLE's Zoom-node functions (0x2da79c family) never reference the
input struct at all. Under (B), the widgets hit-test against the core's
pointer state — permanently (0, 0) in this port — and the int pair is a
game-module echo maintained by the load machinery; her taps could be stored
perfectly and still never move the menu, which fits EVERY observation,
including Build O (the pair obediently tracked the singleton write; the menu
did not change at all). Under (B), the one registering tap is the one
interaction consumed through a path that does read the game-module state —
e.g. during INTRO2 / the load window, before the Zoom poll path owns the
screen.

(A) and (B) are not mutually exclusive. Static analysis has taken this as far
as it goes: the discriminating evidence is a record-level census of the ring
(§27), which shows whether (0, 0)-carrying touch-branch records exist at all,
and whether her records' transforms ever appear in the corrected pair between
samples.

## 25. Q4 — verdict on Build O's identity write: REVERT

- The singleton's letterbox values are correct by design: the engine's logical
  space is 800×480, the surface is 960×544, and origin (27.1667, 0.5) / scale
  1.13333 is exactly the fit-height letterbox with the +0.5 rounding bias. The
  preprocessor's outputs are logical-space coordinates; hit-testing is in
  logical space.
- Identity would have been WRONG had the pair been the live path: a panel tap
  at (484, 378) must map to logical (403, 333); identity maps it to (484, 378)
  — a systematic 1.1333× mis-scale plus offset, i.e. taps landing up-left of
  every target.
- Build O proved the write lands and feeds the resting-pair transform (§22),
  and Lola's report was unchanged ("still just one tap"). The write is on the
  wrong lever. **Remove the VIEWFIX write in the next build** (keep the
  VIEWSTATE readback — it is the cheapest regression check that the singleton
  is intact). The KeyEvent registration (§26) is a separate change; keep it.

## 26. Q5 — buttons: not verifiable from bootlog-17; the census verifies them

Nothing in bootlog-17 (or any earlier log) exposes record types, so whether
Build O's KeyEvent registration (§12) made onGamepadButton's pushes fire is
**unknowable from existing evidence**. onGamepadButton itself pushes from
three sites (0x3d4f74/8e/a2 — press/release variants; the record at sp+0x20
carries a type word and a keycode byte at +0x24). The minimal addition that
verifies buttons is the same ring census that §24 needs — no separate tracer:
if she presses buttons during the census window and button records appear in
the ring, the registration works; if none appear, it doesn't.

## 27. Corrected fix spec — next build (P): revert + ring census

One variable of behavior (the revert) plus measurement only:

1. **Remove the VIEWFIX identity write** from main.c's SCENE arm. Keep
   viewstate_check/sample (read-only) and keep the KeyEvent/AXIS static-int
   registration in jni_bridge.c unchanged.
2. **Ring census (new, loader-side, read-only).** Each frame, in the existing
   sample hook (after input_poll, matching the current sampler's position):
   read the ring state at the addresses the current sampler already uses
   (queue_count/rd/wr — proven coherent in builds N/O). For each slot index in
   (prev_wr, wr] (mask 0x3F, 20-byte records, buffer at the push/pop address
   0x902A50 in engine BSS — resolve via the same engine-base pointer the
   sampler uses), log for the first 300 records of the run:
   `REC: type=%d f04=%08x f08=%08x x=%u y=%u` (type = +0x00 u32; f04/f08 = the
   +0x04/+0x08 words; x/y = +0x0C/+0x0E halfwords). Thereafter count silently
   per type and add a per-type histogram line to the 300-frame heartbeat.
   Cap: never log more than 8 records per frame; count any excess.
3. **INPUTSTATE v2 (corrected cells).** Sample: poll pair at
   **0x8D7ED8/0x8D7EDC**; raw halfwords at **0x8D7ED4/0x8D7ED6**; the doubles at
   **0x8D7EE8 (+0x40), 0x8D7EF0 (+0x48), 0x8D7EF8 (+0x50)**; gate byte
   **0x8D7D41**; enable byte **0x8D7D42**; gate_ptr [0x8D7E40] as before. Keep
   the 1 s change-log + heartbeat cadence, but ALSO log the pair on any change
   with no rate limit for the first 600 frames (catches a tap store that
   survives only frames).
4. **Lola's test protocol for the run:** reach the menu, tap one labelled
   button (e.g. Play) three times slowly, drag once, then press Cross, Circle,
   and Start once each. (Her report of what the screen does still outranks
   every line of the log.)

Decision tree from the census:

- **Type 8/9/10 records with her real coords + later same-frame touch-branch
  records (esp. type 4) with (0, 0)** → (A) confirmed. Fix: interpose at the
  producer — either stop the tail's pump from queueing (0, 0) pointer records
  whose source state was never fed, or feed the core's pointer state from the
  same records the loader sends. Choose after seeing which type carries them.
- **Her records present, no (0, 0) touch-branch records, corrected pair never
  moves, doubles at +0x40/+0x48/+0x50 stay 0.0** → (B) confirmed. Fix moves to
  the core interface: find where the core's pointer doubles are written
  (libc++_shared pass) and feed them from input.c's touch state.
- **No type 8/9/10 records at all during her taps** → the break is back at the
  JNI entry (contradicts §23's code reading — would mean PT_onTouch is not the
  function input.c thinks it is); re-audit the symbol resolution.
- **Button records absent while she presses buttons** → the KeyEvent
  registration did not take effect; re-audit the FalsoJNI static-field lookup
  for `android/view/KeyEvent` before any further button work.

---

# Pass 4 — the push-side gate (2026-10-13)

Build P's census (bootlog-18) reframed the bug: over ~7,000 drained records
the ring carried ONLY type 14 (axis) and type 10 (333, plateauing early).
ZERO type 8/9 (touch down/move), ZERO type 12/13 (buttons) — despite the
scripted TITLE-era protocol. Passes 1–3 audited the drain side; this pass
audits the PUSH side: every call site of the push 0x3d4c8c, the three JNI
handlers read end-to-end, and input.c's call conditions.

## 28. Method: exhaustive push-site census

A raw halfword scan of libengine's first PT_LOAD for Thumb BL encodings
targeting 0x3d4c8c (push) and 0x3da134 (pop) finds **57 push sites and
exactly 1 pop site (0x17071a, the tail's Loop B)** — an independent method
confirming §4/§9's sole-consumer result. Every push site's record
construction was then read. The complete type map:

| Push site(s) | Producer | Type pushed |
|---|---|---|
| 0x3d48ea | onTouch, action DOWN / POINTER_DOWN (bitmask 0x21) | **8** |
| 0x3d4a40 | onTouch, action MOVE (==2) | **9** |
| 0x3d494a | onTouch, action UP / CANCEL / POINTER_UP (bitmask 0x4A) | **10** |
| 0x3d56fa | onGamepadButton (press / release) | **12 / 13** |
| 0x3d59a4 | onGamepadAxis | **14** |
| 0x3d4f74, 0x3d4f8e, 0x3d4fa2 (+1 more below window) | **onKeyDown 0x3d4d68** (892 B export) | 3, 1, 1, 2 |
| 0x22b5dc–0x22c00c (30 sites) | input-module translator family | 1 (+0x04 byte = letter 0x74, 0x75, 0x76, … ascending) |
| 0x152844, 0x1529a0 | game module | 1 (+0x04 byte 0x28 / 0x27) |
| 0xeafae, 0xeb002, 0x152db0, 0x22593e, 0x226f12, 0x22e456, 0x22e95c | surface/dims code (state + game modules) | 11 (+0x04 = 5, dims + centered halves) |
| 0x170708 | frame tail: INPUTQ pump (copies a dequeued record) | passthrough |
| 0x3d3b6a | JNI deferred-list flush (copies list-node records, onDrawFrame neighborhood 0x3d3a60–0x3d3c0c) | passthrough |

Corrections to earlier sections that fall out of this table:

- **§23's "onGamepadButton's three sites at 0x3d4f74/8e/a2" was a
  misattribution.** Those sites belong to onKeyDown (0x3d4d68), a keyboard
  translator our loader never calls. onGamepadButton has exactly ONE push
  site (0x3d56fa). §8's map-gate reading of onGamepadButton itself stands.
- **§23's "onGamepadAxis pushes only on value change" is WRONG.** The
  handler pushes on every call (see §29).
- The 30-site 0x22b5dc cluster (pass 1's "translator cluster") pushes
  type-1 records whose +0x04 byte is an ascending letter code — the
  debug/hotkey translators of §6, not a touch path.

## 29. Record-type truth

**Type 14 IS our own axis flood.** onGamepadAxis (0x3d58a8) has NO
precondition on its common path: once-guard builds the axis map (six
inserts, masks 4/8/0x10/0x20/1/2 keyed by the AXIS_* field values), then
every call builds a type-14 record (+0x08 = looked-up mask, +0x04 = player,
+0x0C = (int)(value × const)) and pushes at 0x3d59a4. input.c's
poll_gamepad calls PT_onGamepadAxis 4× per frame, unconditionally, with
resting-stick values after the deadzone. The census's ~3.16/frame (vs 4
calls) is a minor residual — frames where sceCtrlPeekBufferPositive
short-circuits the whole poll, plus census edge effects; not load-bearing.
The census's +0x08 values {4, 8, 0x10, 0x20} are exactly the map flags for
the loader's axis codes 0/1/11/14 — which also proves the FalsoJNI
static-field registration path WORKS at runtime (see §30).

**Type 10 is touch-UP — and build P's 333 of them are NOT touch-UPs.**
onTouch's UP path (0x3d494a) is the ONLY site in libengine that constructs
a type-10 record. But every onTouch UP is necessarily preceded by a DOWN
push (type 8) from the same handler, and build P shows ZERO type 8; Lola's
protocol had at most 3 tap-UPs, not 333. Therefore the 333 entered the
ring through a COPY path — the INPUTQ pump (0x170708) or the deferred-list
flush (0x3d3b6a) — carrying records constructed outside libengine's static
code (core-side producers feeding INPUTQ / the deferred list through the
interface structs). Their profile fits a load-era producer: production
accelerates through the load (+12 / +54 / +88 / +179 per 300-frame
interval) and stops dead at ~frame 1200 — the same neighborhood where
build O's pair flip marked TITLE load completion (§24/§27-era data).
Producer identity is NOT determinable from libengine alone; §32 specs the
fingerprint that identifies it (the census's 300-line REC budget died in
the axis flood before any type-10 slot was ever printed — build P report's
caveat was exactly this).

## 30. The push-side gate: there isn't one (engine side)

All three handlers were read end-to-end this pass:

- **onTouch (0x3d475c): NO gate before its pushes.** Phase 1 resolves the
  MotionEvent method IDs (cached at globals +0x40…+0x58), reads
  getActionMasked / getActionIndex, and dispatches on the action: mask
  0x4A → type 10 push; mask 0x21 → type 8 push; ==2 → type 9 push;
  action > 6 → return. **If onTouch is called with any valid touch action,
  a push is unavoidable.** (Phase 2 is a pointer-state snapshot into
  globals — count at +0x5C, flag at +9, coords array — with no ring
  interaction; it remains the mechanism-(B) lead from §24, not a gate.)
- **onGamepadButton (0x3d559c): exactly one pre-push condition** — the §8
  `cbz` at 0x3d56f6 skipping the push when the keycode-map lookup returns
  0. It is DISPROVEN as build P's blocker, two ways: (1) the axis map,
  built by the identical helper through the identical FalsoJNI
  registration, demonstrably populated and resolves at runtime (type-14
  +0x08 values, §29); (2) jni_bridge.c's tables (build O, kept) carry the
  true Android values — KEYCODE_BUTTON_A = 96, B = 97, START = 108, etc.
  (nameToFieldId IDs 400–414, fieldsInt values to match), and input.c
  sends exactly those codes. If PT_onGamepadButton had been called with
  her Cross/Circle/Start presses, type 12/13 would have been pushed.
- **onGamepadAxis: no condition at all** (§29).
- **The [0x8D7D41] / [0x8D7D42] bytes are RETIRED as "gate/enable" cells.**
  They are byte lanes inside the STATE +0x10 / +0x14 word fields that
  Loop B itself maintains (ctx pointer / last-popped type) — artifacts of
  the corrected addressing (§27), not standalone flags. No focus/enable
  byte is consulted anywhere on any push path; the preprocessor's
  mode==3 / consume-mask logic (§§6–7) is drain-side only.

**Loader side:** main.c's frame loop calls input_poll() unconditionally;
input.c is the loader's ONLY sceTouch/sceCtrl consumer (tree-wide grep);
all three handlers resolved live in build P (bootlog-18 lines 327–329);
poll_touch is report-driven (DOWN on new id, MOVE on changed coords, UP on
release) and poll_gamepad edge-triggers buttons off the same SceCtrlData
whose axis half demonstrably flows 4×/frame.

**Verdict:** build P's zeros decompose as **PT_onTouch was never called
and PT_onGamepadButton was never called** during the run — poll_touch
never saw a touch report and poll_gamepad never saw a button change in
2,100 frames — while PT_onGamepadAxis was called ~4×/frame from the same
input_poll. Since one peek feeds both axes and buttons, her
Cross/Circle/Start presses also never appeared in SceCtrlData in that
run: NO hardware input of hers reached input.c at all in build P.
(Caveat, recorded honestly: the log alone cannot prove bootlog-18's run is
the protocol run; the §32 counters settle it live either way.)

## 31. Reconciliation with history: pushes DID happen in the L era

"Pushes never happened in any era" is REFUTED. Build L's 28 INPUT lines
sit ~4,100 log lines AFTER the TITLE switch (lines 14832–18034 vs TITLE
at 10694), with menu-area coords and Cross (code 96) press/release pairs:
input.c demonstrably called onTouch at TITLE in build L, and onTouch is
gateless — so type 8/10 pushes DID enter the ring in build L. The menu
still didn't react, which keeps the L-era failure on the drain/consume
side (mechanisms A/B, §24) compounded by L's ~1 fps poll sampling.

The N/O "exactly one tap registered" and P's "none" are consistent with
her inputs reaching input.c rarely-to-never in the sampler-era runs — but
**what changed between L and P is NOT in input.c** (build M removed only
its two log lines; the N/P sampler is read-only). The live fork:
(i) an era change elsewhere in the loader between L and P affecting
sceTouchPeek / sceCtrlPeekBufferPositive reporting; (ii) run variance /
protocol timing; (iii) bootlog-18 is not the protocol run. One run of the
§32 build discriminates all three.

## 32. Next-build spec (Q): diagnostics only, one variable

1. **Loader-side call counters in input.c**, printed in the existing
   300-frame heartbeat: poll_touch frames with reportNum > 0;
   send_touch_event calls split by action (0/1/2); PT_onGamepadButton
   calls; PT_onGamepadAxis calls; last return codes of sceTouchPeek and
   sceCtrlPeekBufferPositive. — Separates "loader never called" from
   "called but no push" for touch AND buttons in a single run.
2. **Census tweak:** reserve the REC log budget for non-14 types — log
   EVERY drained non-axis record with all fields (type/f04/f08/f0c). —
   Fingerprints the type-10 copy-path producer's payload.
3. Protocol unchanged (3 slow taps, a drag, Cross/Circle/Start at TITLE).
   If (1) shows handler calls but the census still shows no 8/9/12/13,
   the FOLLOW-UP (not this build) is a push-entry trampoline counting by
   return address.

No behavior changes. The §4.4 dispatch interposition stays (§4.4 preserve
rule). Raw .vpk delivery, as always.

## Pass 5 — the readback (2026-10-13, static; no build)

Brief: prove or refute that the handlers are called correctly but read
the fake MotionEvent/KeyEvent objects back as zeros through the JNI
Call*MethodV path, while onGamepadAxis (primitive params) works.
Result up front: **every link of both readback chains verifies
statically — send path, fake getters, field tables, FalsoJNI dispatch,
varargs thunks, handler dispatch — and the runtime still contradicts
the statics.** The hypothesis in its simple form ("getters read the
wrong struct", "a getter is missing", "floats die in the va_list") is
REFUTED at every point it names. What survives is narrower and
stranger, stated in §37; the deciding instrument is specified in §38.

## 33. The send path (what input.c actually hands the handlers)

Touch (`source/input.c`, `send_touch_event`, called from `poll_touch`):

- There is **no per-event object layout to get wrong**. The jobject
  argument is `pt_motion_event_obj = &dummy_me_obj` — the address of a
  single file-scope `int` in input.c, never written. All event state
  lives in ONE static struct in `source/fake_motionevent.c`:
  `me_state { int packedAction; int pointerCount; int ids[6];
  float xs[6]; float ys[6]; }`.
- `send_touch_event(packedAction)` first packs the currently-ACTIVE
  touches (count `n`, ids, xs, ys from `g_touches[]`), then calls
  `motionevent_update(packedAction, n, ids, xs, ys)` — which copies
  them into `me_state` — and only then calls
  `PT_onTouch(&jni, NULL, pt_motion_event_obj)`. Write-before-call
  ordering is correct.
- Consequence of the packing rule: the release path marks the touch
  inactive BEFORE sending, so **every ACTION_UP is delivered with
  pointerCount = 0**. The fake's getX/getY/getPointerId therefore
  return 0.0f / -1 for UP events by construction (bounds-checked
  against pointerCount), whatever else is true.
- `poll_touch`: DOWN when a report id is new; MOVE only when a held
  touch's coords changed; UP on release. In build Q's run this
  produced send_down = send_up = 215, send_move = 0 (see §37).

Buttons (`poll_gamepad`): **no event object at all** —
`PT_onGamepadButton(&jni, NULL, androidKeycode, pressed, 0)`, pure
primitives (r2 = keycode: Cross 96, Circle 97, Start 108; r3 =
pressed). The button handler's "readback" is not of an event object;
it is the handler's own lazy keycode-map build through static-field
JNI reads (§36).

Axes: `PT_onGamepadAxis(&jni, NULL, axisId, value, 0)` x4 per poll,
unconditional — primitives only, and the only path that queued
records in Q. The control group is real.

## 34. The fake getters and the field tables

`fake_motionevent.c` implements exactly the seven getters onTouch
caches (IDs per `jni_bridge.c`'s name-only method table): 200
getAction, 201 getActionMasked, 202 getActionIndex, 203
getPointerCount, 204 getPointerId (methodsInt), 205 getX, 206 getY
(methodsFloat). All read `me_state` — the same struct
`motionevent_update` fills (§33). There is no second MotionEvent
implementation or second state block anywhere in the tree (grep over
the full source: one `me_state`, one `motionevent_update`, one set of
getters). Signatures are `(jmethodID, va_list)`: the four no-arg
getters ignore `args`; getPointerId/getX/getY take one
`va_arg(args, jint)` index, bounds-checked against pointerCount
(out-of-range -> -1 / 0.0f).

No getter the handlers call is unimplemented. A table miss in
FalsoJNI's dispatchers would log `method ID %i not found in
methodsInt/methodsFloat` and return -1 (verified in
`FalsoJNI_ImplBridge.c` this pass); bootlog-19 contains **zero** such
warnings (grep count 0). The only JNI NOT FOUND lines in bootlog-19
are unrelated CPJNISound methods (freeWav/freeMod/stopMod/onResume/
modVolume/wavVolume) resolved at startup.

KeyEvent statics (`jni_bridge.c`, added build O, still present):
`nameToFieldId[]` maps KEYCODE_BUTTON_A..MODE and DPAD_* to field IDs
400-414; `fieldsInt[]` maps those IDs to the true Android values
(400->96, 401->97, 402->99, 403->100, 404->102, 405->103, 406->106,
407->107, 408->108, 409->109, 410->110, DPAD 19/20/21/22) — the exact
codes input.c sends. FalsoJNI's `GetStaticFieldID` ->
`getFieldIdByName` (name-only, first match; miss -> NULL + warning),
`GetStaticIntField` -> `getIntFieldValueById` (miss default is **1**,
not 0 — noted for §38). `FindClass` never returns NULL in this
FalsoJNI (it returns `strdup(name)`), so onGamepadButton's
FindClass("android/view/KeyEvent") gate cannot be the failure.

## 35. The varargs marshalling (FalsoJNI + the engine's thunks)

- `FalsoJNI.c`: `CallIntMethodV` -> `methodIntCall(methodID, args)`;
  `CallFloatMethodV` -> `methodFloatCall(methodID, args)` — the
  va_list is forwarded unchanged. The ImplBridge dispatchers
  linear-scan by ID and invoke `Method(id, args)`, a signature match
  for the fakes. `_AtoV` (the *A-variant adapter) is an identity
  no-op in this vendored copy, but the handlers do not use the *A
  variants, so it is not on this path.
- The engine's own call adapters (the layer that actually builds the
  va_list): int thunk 0x3d4bcc, float thunk 0x3d4c04. Read in full
  this pass: each stores its incoming r3 integer argument into a
  frame slot, passes a va_list pointing at that slot, calls JNIEnv
  slot 0xC8 (CallIntMethodV) / 0xE0 (CallFloatMethodV), then runs a
  stack-canary check. **They never interpret the return value** — the
  int thunk returns r0 untouched; the float thunk returns r0
  untouched. ABI-transparent.
- The float-promotion pitfall the brief flagged does not exist on
  this path: no float ever travels *through* a va_list. Arguments
  are ints (the pointer index); results come back in r0 under the
  softfp ABI and onTouch consumes them with `vmov sN, r0` — the
  correct softfp read. Int-vs-pointer slot widths: the thunk's slot
  is a full word holding a jint index — matches `va_arg(args, jint)`.

Marshalling: acquitted, at every layer, for both int and float.

## 36. The handlers' exact reads (disassembly, this pass)

### onTouch (0x3d475c)

Phase 1 (once): GetObjectClass + seven GetMethodID calls; the string
table decodes in order `getX (I)F / getY / getPointerCount ()I /
getAction / getPointerId (I)I / getActionIndex / getActionMasked`,
stored to the globals cache (base 0x90475C) at +0x40..+0x58 in that
order — i.e. +0x58 = getActionMasked = ID 201, +0x54 =
getActionIndex = 202, +0x48 = getPointerCount = 203, +0x40/+0x44 =
getX/getY = 205/206. Re-verified instruction by instruction this
pass; no slot shift.

Per call, exactly two reads decide everything:
call 1 = CallIntMethodV(getActionMasked) -> r5; call 2 =
CallIntMethodV(getActionIndex) -> r6. Dispatch:
- r5 > 6 (`cmp #6; bhi`) -> no branch, phase 2 only. **Silent.**
- bit test `(1<<r5) & 0x4A` (r5 in {1,3,6}) -> UP branch 0x3d48f0.
- bit test `(1<<r5) & 0x21` (r5 in {0,5}) -> DOWN branch 0x3d4884.
- else r5 == 2 -> MOVE path 0x3d4998; r5 == 4 -> silent.

DOWN branch (0x3d4884) is **straight-line code with no conditional**
between entry and the push: getY(idx) -> s16, getX(idx) -> s18 (float
thunk), helper 0x3d4c3c (pointer-tracker add; returns the sample's
sequence number), record built on the stack (+0x00 = 8, +0x04 =
sequence, +0x08 = 1, +0x0C/+0x0E = (u16)(u32)getX / getY,
+0x10 = 0), push at 0x3d48ea via 0x3d4c8c. **If the DOWN branch is
entered, a type-8 record is unavoidable.**

UP branch (0x3d48f0): record +0x04 = first word of the tracker's
12-byte slot for that pointer index (the sequence stamped by the
DOWN-side helper), +0x08 = 1, type 10, x/y halfwords from
getY/getX(idx); push at 0x3d494a; then helper 0x3d4d08 erases the
tracker slot. MOVE path (0x3d4998) likewise builds a type-9 record
**unconditionally** (no zero-delta gate); push at 0x3d4a40. Phase 2
(every call that reaches it) snapshots getPointerCount (clamped <= 8)
and per-pointer getX/getY into globals (count at globals+0x5C,
down-flag byte at globals+9) — the §30 mechanism-B state — and pushes
nothing.

The helpers are exclusive: BL-scan of the whole library —
0x3d4c3c (tracker add) is called ONLY from 0x3d48be (the DOWN
branch); 0x3db1d2 (vector push) only from inside it; 0x3d4d08
(tracker erase) only from 0x3d4952 (the UP branch). The push
0x3d4c8c itself was re-read this pass: mutex, copy 20 bytes into
ring[wr] (ring 0x902A50, state 0x904894), count++ clamped at 0x100 —
**no type filter, no drop path**, exactly as pass 1 found.

### onGamepadButton (0x3d559c)

Args are the primitives of §33 (keycode r2, pressed r3). First call
builds the keycode map: FindClass("android/view/KeyEvent"), then 14x
a helper (0x3d57b8) that does GetStaticFieldID(env, class, name, "I")
(JNIEnv slot 0x240) — on a zero field ID the insert is skipped —
else GetStaticIntField (slot 0x258) and inserts
(android keycode -> engine button code: BUTTON_A -> 0x1000, B ->
0x2000, X -> 0x4000, Y -> 0x8000, L1 -> 0x100, R1 -> 0x200, START ->
0x10, SELECT -> 0x20, THUMBL/R -> 0x40/0x80, MODE -> 0x400, DPAD ->
1/2/4/8). Per call, the record (type 12 press / 13 release) is built
and **`cbz` at 0x3d56f6 skips the push (0x3d56fa) iff the map lookup
for the incoming keycode yields 0**. That lookup is the handler's
only gate.

## 37. What build Q's fingerprint can and cannot be

Q (bootlog-19): handlers resolved to the true engine functions
(onTouch 0x993d475d, onGamepadButton 0x993d559d, onGamepadAxis
0x993d58a9); send_down = send_up = 215 in lockstep, btn_calls = 6
(3 press + 3 release = Lola's Cross/Circle/Start), axis_calls ~4/
frame; the ring received exactly 215 non-axis records — all type 10,
all (0,0), f04 = 0,1,2,...,214, f08 = 1 — and zero 8/9/12/13.

For buttons the fingerprint has exactly one reading: every map
lookup returned 0, i.e. at runtime the map was empty or keyed by
values other than {96, 97, 108} — despite §34's registration being
correct in the source. Candidate runtime divergences the statics
cannot see: the init never ran (a guard word at the handler's
globals+0x64 nonzero from another writer), the field IDs/values the
handler actually received differ from the tables (e.g. every
GetStaticIntField missing and returning the bridge's default 1,
collapsing all keys onto 1), or the lookup helper's map state was
reset after building. All are runtime facts, not code facts.

For touch the fingerprint is, on the code as read, **impossible** —
and that impossibility is this pass's main product:
1. If the 215 type-10s are onTouch's UP records, their f04 sequence
   is the tracker sequence (§36), which is written ONLY by the DOWN
   branch's helper — so the DOWN branch ran 215 times, and its
   straight-line push must have queued 215 type-8s. The census (whose
   code was re-read this pass: right ring, right state, missed = 0,
   excess = 0 all run) saw none.
2. If instead the DOWN calls' masked-action readback was in the
   silent set {4} U {>6} on all 215 calls, the type-10s are not
   onTouch's at all — they are the §29 copy-path producer's (the
   input-state pump stamping its own sequence), which also fits
   their (0,0) coords and the parked position state (INPUTSTATE
   position doubles all 0.0). But the fake's getActionMasked returns
   `packedAction & 0xFF` = 0 for a DOWN by construction, and every
   link from GetMethodID's name match to ID 201 to the fake checks
   out (§§34-36).
One of these two readings misdescribes the runtime; no amount of
further statics can say which, because both are airtight on paper.

A third observation belongs on the record: 215 DOWN/UP pairs with
zero MOVEs is not Lola's scripted protocol (3 slow taps + a drag) —
the poll evidently saw ~215 single-frame contact flickers (report
present one peek, absent the next; that pattern yields DOWN/UP pairs
with no MOVE and send_ptr = 0, exactly Q). It does not change the
analysis, but the next run's protocol should use long holds (§38).

Refined hypothesis, replacing the brief's: JNI delivery, the push,
the ring and the census are all proven live (axis flood; L-era taps).
The two dead handlers are precisely the two whose record decision
depends on a value read back through JNI — masked action for touch,
the static-field-built map for buttons. The break is in what those
values ARE at runtime, somewhere the source does not show.

## 38. Fix spec: no code fix yet — one tracer build (R)

No loader correction is justified yet: every candidate fix the brief
named (getter layout, missing fake method, va_list marshalling) is
statically refuted above, and "fixing" any of them would change code
that is provably right. Build R is diagnostics only, three cheap
instruments, all loader-side, one run:

1. **Getter readback log (the values the handlers READ).** Inside
   `fake_motionevent.c`: for the first ~80 calls each, log the RETURN
   value of getActionMasked, getActionIndex, getPointerCount,
   getX/getY, together with the `me_state` fields at that instant
   (packedAction, pointerCount). This is the decisive sample:
   masked = 0 on DOWN sends -> readback is fine and the fault is
   inside onTouch past the dispatch (instrument 3 localizes it);
   masked = -1 -> the runtime method-ID table diverges from the
   source; masked = 1/other on DOWN sends -> `me_state` is stale or
   re-written between update and read (ordering/threading), and the
   logged packedAction names it.
2. **Field-resolution log.** In `getFieldIdByName` /
   `getIntFieldValueById` (or a jni_bridge wrapper): log the first
   resolution of each KEYCODE_* name -> field ID -> value. Values
   400->96 ... prove the button map build; NULL IDs or the default
   value 1 prove the collapse and name the broken table.
3. **Push-entry log with return address.** Interpose at push
   0x3d4c8c (loader-side, §4.4-style) and log (type, f04, f08, x, y,
   LR) for the first ~250 non-14 pushes. LR separates the producers:
   0x3d48ea/0x3d4a40/0x3d494a = onTouch, 0x3d56fa = buttons, 0x170708
   = the pump copy path. Pushes seen here but not in the census
   would break the ring theory itself (least likely; P and Q both
   argue against it); pushes never seen close the case against the
   handlers.

Keep Q's POLL counters and census in R as cross-checks. Protocol
change (ask Lola): hold each tap ~2 seconds, hold each button ~2
seconds — long holds separate DOWN/HOLD/UP in the getter log and
give MOVE a chance to exist. §4.4 interposition stays. The fix that
follows R is written against whichever of (1)-(3) fires; each outcome
names its own minimal correction (a table/registration repair, an
update-ordering repair, or an engine-side guard this analysis has
not yet seen).


## Pass 6 — the drain (2026-10-13, static; no build)

## 39. Headline

**The store executes. The press is annihilated temporally, inside a
single frame-tail drain, by a bug in the loader's own poller.** The
preprocessor's pair store (site 2, §41) fires for every type-8 record
and writes the transformed press position to 0x8D7ED8/0x8D7EDC — and
the type-10 UP record that the loader's `poll_touch()` synthesizes
*in the same call, every call*, lands in the same drain batch right
behind it and overwrites the pair with the transform of (0,0) =
(−23, 0) before the scene update ever polls. On stock, DOWN and UP
are separate platform events in separate frames, so the pair holds
the press for the whole contact. Here a contact is re-reported as a
fresh DOWN+UP pair on every poll cycle (Build R: 94 peeks with
reports → send_down == send_up == 94 in lockstep, send_move == 0).
Root cause is one missing line in `input.c`: the new-touch branch
never marks the slot `seen[]`, so the release sweep at the bottom of
the same `poll_touch()` call reaps the just-registered touch and
emits ACTION_UP immediately (§43). The fix is that one line (§47).
No engine-side discriminator — type, f04, f08, the 0x223aa4 gate
byte, the onEvent routing, record source — blocks the store; every
one was checked instruction-by-instruction this pass (§42).

## 40. Method

Build R's instruments are ground truth for the ring (push census:
94 type-8 with real coords, f08=1; 94 type-10; 3+3 type-12/13) and
for the polled state (INPUTSTATE v2: pair parked at (−23, 0),
doubles 0.0, all 64 samples). This pass adds firsthand disassembly
of the whole drain path (fn 0x170620, preprocessor 0x2220a8 incl. its
sub-handler 0x282ae0 and fall-through check 0x222910, post-step
0x28d398), a raw-halfword BL census (capstone full-text sweeps OOM
on this box; the raw scan runs in <1 s), and a verbatim read of the
loader's `input.c`. PIC bases use pass 3's corrected rule (add-pc
base = raw addr+4).

## 41. One type-8 record's journey (instruction-by-instruction)

Record: type 8, f04 = sequence, f08 = 1, x≈499 @+0x0C, y≈378 @+0x0E
(Build R push census).

1. **Pop.** Loop B (fn 0x170620) pops it at 0x170718 via 0x3da134
   — the ring pop's only caller in the binary (raw BL census).
2. **Notify.** 0x17071e `bl 0x224964` → 0x1cd614(0x8D7F60-obj,
   record): a listener fan-out; writes neither the record nor the
   input struct.
3. **Preprocessor 0x2220a8(record)** (called 0x170728; its only
   caller). Register map: r4 = record, r5 = STATE (0x8D7D30),
   sl = input struct (0x8D7EA8), sb = INPUTQ-side cell (0x8D7F48),
   r8 = mode cell pointer; mode = 3 (constant this run).
   - *Trap door (acquitted again):* mode==3 && ptr [0x8D7E40]≠0 &&
     type<0xB → early exit return 1 @0x222394. The pointer is 0 in
     every run → door open.
   - *Sub-handler:* on the mode-3 fall-through, if [STATE+0x12]
     ([0x8D7D42]) = 1, 0x222104 calls **0x282ae0(gestureObj,
     record)**. Dumped in full this pass: it is a scene-graph
     stroke/trail tracker keyed on its own object's fields (+0x30
     enable byte, +0x58/+0x5C capture ids, +0x60 node). Type 8 →
     helper 0x282c40 creates a stroke node into [obj+0x60]; type 9
     → 0x282e04 move update; type 10 → 0x282edc/0x282f1e release;
     types 4/5/6 handled against the capture ids. It never
     references the input struct, never writes the record, and its
     return value is discarded by the caller. It cannot block,
     consume, or clear a press. Red herring, closed.
   - *Store site 1 (skipped):* @0x222246 — types 9 and 4 only.
   - *Store site 2 (FIRES):* @0x222284 — `cmp type,#0xA; bhi out`;
     `(1<<type) & 0x560 ≠ 0` → types {5,6,8,10}. For our record:
     `str.w ldrh[rec+0xC] → [input+0x30]` @0x222294,
     `str.w ldrh[rec+0xE] → [input+0x34]` @0x22229a; then mode==3 →
     `bl 0x2ad01c` (transform, in place on &input+0x30) @0x2222b6 →
     `strd result → [input+0x30]` @0x2222be. The pair now holds the
     transformed press position (≈(416, 333) for (499,378) under
     the build-P view calibration). **The store the last three
     passes hunted for executes here, unconditionally, for every
     one of the 94 DOWN records.**
   - *Fall-through check 0x222910(record):* a single type-1 'C'
     (0x43) hotkey toggle; returns 0 for every non-type-1 record.
     The preprocessor's tail (0x222420+) is further type-1 hotkey
     machinery ('z'/'L'/'Y' + modifier masks 0x180/0x60). Nothing
     on any of these paths sets the return for a type-8 record →
     **preprocessor returns 0 = not consumed.**
4. **Scene dispatch.** Back in Loop B: gate fn 0x223aa4 reads
   [0x8D7D41] = 0 in every run → dispatch allowed → onEvent
   forwarder 0x2a8ea6 → vtable slot +0x2C → TITLE::onEvent
   0x183820, which handles only type-1 code 0x1B (ESC) and returns.
   Stock-faithful: this stub is the same code on Android — TITLE
   input is poll-based, not event-based; the dispatch is a no-op
   for touch on stock too.
5. **Post-step 0x28d398(record)** (called 0x170740; only caller):
   a types-1..10 state machine over a module struct in the
   0x28xxxx region (copy x/y → +0x20/+0x24, f08-compared cases,
   a scroll accumulator at +0x10, node lists at +0x2C/+0x30) —
   pointer bookkeeping for that module; it never writes the input
   struct. Types 11+ exit at its range check.
6. **The kill step — the NEXT record in the same drain.** The UP
   record (type 10, coords (0,0) by construction: the loader's UP
   event carries pointerCount=0, §32.6) sits directly behind the
   DOWN in the same batch — both were pushed by the same
   `poll_touch()` call (§43), and FIFO order puts INPUTQ-pumped
   records after them. It replays steps 1–5: site 2 matches type
   10, stores raw (0,0), transforms, strd → **pair = (−23, 0)**.
   Only after the whole batch drains does control return to the
   scene update — which therefore polls a pair that shows "no
   contact, at the letterbox origin" on every frame of every tap.
   The INPUTSTATE sampler (even at ~0.25 s cadence) can never
   catch the press value: it exists only intra-drain, between two
   records of the same batch, before any poll.

## 42. Discriminator checklist (the brief's five suspects)

- **Record type:** site 2's mask {5,6,8,10} *includes* 8. Passes.
- **f04 (+0x04):** read by NO touch branch. Site 1/2 read only
  +0x00 (type), +0x0C, +0x0E. Only the type-1 hotkey paths and the
  post-step's node-matching cases read f04. Inert here.
- **f08 (+0x08):** NOT a preprocessor discriminator for touch.
  The sub-handler reads it only in its type-4/5/6 pointer
  branches (bit test); the post-step compares it (1/2/4) in two
  of its own type cases. Our f08=1 on type-8s is read by nobody
  on the pair-store path. Inert.
- **fn 0x223aa4 byte [0x8D7D41]:** gates the *scene dispatch* in
  Loop B, not the store (the store already happened, step 3). It
  is 0 → dispatch proceeds anyway.
- **onEvent routing (slot +0x2C via 0x2a8ea6):** happens *after*
  the store, once per record, and lands on TITLE's ESC-only stub
  by design (stock-identical). It is not an alternative to the
  store; both run.
- **Source-sensitivity (INPUTQ pump vs direct push):** none. The
  drain is a single FIFO: the tail's pump copies INPUTQ records
  into the same ring (push @0x170708) ahead of the pop loop, and
  direct pushes from the JNI handlers are already in the ring.
  A type-8 record takes the identical path regardless of which
  producer pushed it.

## 43. Why DOWN+UP share a drain: the poller, verbatim

`pockettanks-vita/source/input.c`, `poll_touch()` (called once per
frame from `input_poll`). For each reported contact it finds or
creates a slot; the new-touch branch registers the touch and sends
DOWN — **but never sets `seen[slot]`**. The release sweep
immediately below then runs in the same call:

```c
for (int s = 0; s < MAX_TOUCH; s++)
    if (touches[s].active && !seen[s]) {   /* the just-added touch
                                              has seen == 0 */
        touches[s].active = 0; touch_count--; released_any = 1;
    }
if (touch_count == 0 && released_any) {
    send_touch_event(ME_ACTION_UP); return;
}
```

So every poll cycle with a finger down emits DOWN then UP
back-to-back and frees the slot; the next frame's peek re-registers
the still-held finger as brand new. The held/moved branches
(`seen = 1/2`, the only sources of MOVE) are unreachable for any
contact lasting more than one poll. This predicts Build R's
telemetry exactly: touch_reports=94, send_down=94, send_up=94 in
lockstep, send_move=0 despite 2-second holds — and it retrodicts
the whole bug's shape since Build F ("input reaches the engine"
yet nothing reacts): the engine has been receiving well-formed,
correctly-coordinated presses all along, each one cancelled by
its own release inside the same frame.

## 44. The buttons' journey (type 12)

Same drain: notify → preprocessor (trap door open; sub-handler
has no 12/13 case — its type checks cover 4/5/6/8/9/10; site 1
no; site 2 rejects type 12 at the `cmp #0xA; bhi` bound) →
fall-through check returns 0 → gate byte 0 → onEvent → TITLE's
stub ignores type≠1 → post-step rejects it at its 1..10 range
check. **A type-12 record writes no state anywhere in the
engine; it dies at TITLE's onEvent stub.** That is also its
stock fate at TITLE — the title screen is touch-driven, and
gamepad-button records only matter in scenes whose onEvent
handles 12/13. (D-pad navigation, if a scene supports it, arrives
via onKeyDown types 1/2, which the loader never calls — a known,
separate gap; not the menu blocker.)

## 45. The stock comparison — and a correction about the "core"

What differs on stock Android is not a record field, not a module
byte, not a queue: it is the **temporal shape of the event
stream**. The platform delivers ACTION_DOWN and ACTION_UP as
separate callbacks separated by the contact's real duration, so
the DOWN record drains alone, the pair takes the press transform,
and it *stays* there — refreshed by MOVE records (site 1 covers
type 9) — until the UP drains in a later frame. Every engine-side
mechanism our port was suspected of missing is present and
running; only the loader's event stream is malformed in time.

The brief's hypothesized second channel does not exist:
**libc++_shared.so is the stock NDK C++ runtime** — 554,808
bytes, stripped, 2,393 dynamic symbols, *zero* `Java_` exports,
all `std::__ndk1` (readelf, this pass). The entire Zoom framework
lives inside libengine.so; the "core interface" the game consults
(interface table at [0x702bb0], doubles getters at +0x368) is an
in-module abstraction whose JNI backers are the game module's own
registered handlers — the very PT_onTouch / PT_onGamepadButton
forwards the loader already calls. There is no core JNI input
entry to additionally feed.

Residuals, explicitly *not* the menu blocker: (a) the position
doubles (input+0x40/+0x48/+0x50) stay 0.0 — their writers are
gated on INPUTQ-side producer state (§24) and their getters
(0x223a08/0x223a1c/0x223a30) have **zero BL callers in libengine**
(raw census, this pass; confirms §24) — no in-module consumer
exists to starve. (b) INPUTQ's type-4 producer (site 1's stock
feeder) remains unidentified; site 2 makes it unnecessary for
discrete presses. (c) The sampler's raw_x/raw_y cells
(input+0x2C/+0x2E) are not written by the preprocessor (see §46);
their writer is still unattributed and immaterial to the pair.

## 46. Corrections to earlier passes

- §21's store map was wrong in detail: there is no raw store at
  0x2221e4 and no `ldrd` overwrite at 0x222296 in the instruction
  stream. The real stores are `str.w` of the zero-extended
  `ldrh[rec+0xC/+0xE]` at 0x222250/0x222256 (site 1) and
  0x222294/0x22229a (site 2), each followed in mode 3 by the
  0x2ad01c transform `strd` (0x22227a / 0x2222be).
- Pass 5's working conclusion — "the store path is never taken" —
  is superseded: the store is taken twice per poll cycle (DOWN
  and UP), and the failure is temporal, in the loader, upstream
  of everything passes 3–5 examined. The engine-side suspects
  those passes cleared (handlers, getters, tables, ring, gate
  bytes) were in fact all innocent; the one component not under
  suspicion — the poller's own bookkeeping — held the bug.
- Lola's "one touch came through" reports (N and O runs): still
  unexplained by any mechanism in this pass; possibly a frame
  where peek raced a real release. Not evidence for or against
  the fix; the verification readout (§47) will settle the matter
  empirically.

## 47. THE FIX SPEC (build S — one line, loader-side)

In `source/input.c`, `poll_touch()`, new-touch branch — mark the
slot seen so the same call's release sweep leaves it alone:

```c
touches[slot].active = 1;
touches[slot].id = id;
touches[slot].x = x;
touches[slot].y = y;
seen[slot] = 1;            /* NEW: registered this call — not a release */
touch_count++;
```

Nothing else changes. The §4.4 dispatch interposition stays
untouched (its A/B is still owed separately). Expected stream
after the fix: DOWN once per contact; MOVE when the position
changes (reviving the dead type-9 path — site 1 then also
maintains the pair during drags); silence while held; UP once,
on the release frame. The DOWN record drains alone; the pair
holds the press transform for the contact's full duration.

**Verification readout for build S** (same protocol: menu, three
~2 s holds on one labelled button, one drag, buttons once each):
- POLL counters: send_down == number of distinct contacts (not
  per-frame); send_up == send_down; send_move > 0 during the
  drag; touch_reports ≫ send_down during holds.
- INPUTSTATE v2, samples *during* a hold: pair = the press
  transform (≈(416, 333) for a (499,378) tap) instead of
  (−23, 0); pair returns to (−23, 0) only after release.
- Push census (if the R hook is kept): type-8 and type-10
  records appear in *different* drain batches, frames apart.
- The verdict remains Lola's screen, per the standing rule: the
  menu button reacts to a held tap.

Optional polish, deliberately NOT bundled (one variable per
build): the UP event still carries pointerCount=0, so the
release-frame store writes the (0,0) transform rather than the
lift position — cosmetic only, since the press has already been
live for the whole contact. If ever wanted, keep the released
touch in the `motionevent_update` snapshot for the UP call.
