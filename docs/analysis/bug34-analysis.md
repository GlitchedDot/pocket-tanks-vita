# Bug 34 — the keyboard / text-entry surface (pass 1, 2026-10-13)

Build T state: navigation works (TITLE → MENU_OLD / OPTIONS_OLD by touch;
taps now highlight *and* activate). Lola is stuck at the player setup
step inside MENU_OLD (name + tank color). bootlog-22 carries 1,741 lines
of one line —

```
JNI: GetStaticMethodID(class="com/blitwise/engine/jni/CPJNILib",
    name="getKeyboardRect", sig="()Landroid/graphics/Rect;"): NOT FOUND
```

— the only unresolved CPJNILib method in any bootlog. This pass maps
the whole keyboard surface: the engine's methods, the call sites, what
the setup screen is waiting for, how text is supposed to enter, the
gamepad side question, and the Vita-native design spec. **No build.**

Headline up front: the "keyboard" is not Android's IME. Pocket Tanks
ships its **own on-screen keyboard** (an Android `KeyboardView` in the
activity layout) and drives it through exactly three CPJNILib methods.
The loader implements one as a constant FALSE, one as a no-op, and the
third not at all. There is **no text-commit channel** — no JNI method
anywhere takes a finished string. Text enters one character at a time
through the engine's own `onKeyDown(int)` export, which the loader has
never called, and each call is a complete key tap (down-records now, the
release appended to the deferred list). The Vita fix is therefore:
wire the three methods to a loader keyboard-state flag, and bridge
`setKeyboardVisibility(true)` to the Vita's native IME dialog
(SceImeDialog), injecting the finished string as `onKeyDown` character
calls when the dialog closes.

---

## 1. The stock design (Java side, from jadx-out)

Sources: `jadx-out/sources/com/blitwise/engine/jni/CPJNILib.java`,
`jadx-out/sources/C/d.java` (the keyboard listener, package `C`),
`jadx-out/sources/com/blitwise/engine/CPView.java`, and the three
keyboard layout AXMLs decoded from the APK (`res/gg.xml` = `keyboard`,
`res/6o.xml` = `keyboard_symbols`, `res/Y8.xml` = `keyboard_moresymbols`;
resource IDs per `R$xml.java`).

- `CPActivity`'s layout contains a `keyboardWrapper`/`keyboardView`
  (an `android.inputmethodservice.KeyboardView`). `C.d` constructs it
  with the `keyboard` layout and registers itself as its
  `OnKeyboardActionListener`.
- `CPJNILib.setKeyboardVisibility(boolean)` (instance method) posts a
  runnable that sets the wrapper + view visibility (0 = VISIBLE,
  8 = GONE) and flips the static Java flag `isKeyboardActive`.
- `CPJNILib.isKeyboardActive()` returns that flag.
- `CPJNILib.getKeyboardRect()` (static) returns
  `keyboardView.getDrawingRect(new Rect())` — the keyboard view's
  rectangle in **window coordinates** (top-left origin, y down). With
  the keyboard hidden (GONE) the drawing rect is empty.
- Key presses: `C.d.onKey(code, ...)` → `b(code, time)`:
  - Printable keys arrive with **character codes** (see §5): letters
    are the *lowercase ASCII* code points; if the layout's Shift key
    (code −1, momentary/lock logic in Java) is engaged, Java applies
    `Character.toUpperCase` before forwarding.
  - Code **10** ("Done" key): Java calls
    `setKeyboardVisibility(false)` and then still forwards
    `CPJNILib.onKeyDown(10)` (the `onKeyDown` call is the shared tail
    of the active branch).
  - Code **27** (ESC): if the keyboard is active, Java *only* hides
    it; if inactive, it forwards `onKeyDown(27)`.
  - Layout-switch codes −10/−11/−12 swap the `Keyboard` object
    (letters ↔ symbols ↔ more-symbols) in Java.
  - Releases (`onRelease`) and hardware-key releases forward to
    `CPJNILib.onKeyUp(code)` — which is a **native no-op** (§5).
  - A hardware keyboard path (`View.OnKeyListener`) converges on the
    same entry points with `keyEvent.getUnicodeChar()`, mapping
    KEYCODE_DEL (67) → 8, KEYCODE_BACK (4) → 27, SHIFT (59/60) → 16.
- Layout facts (letters, `res/gg.xml`): QWERTY rows with
  `codes` = lowercase ASCII (113 'q', 97 'a', …), Shift = −1,
  **Delete = 8**, Space = 32, **Done = 10**, symbols switch = −10.
  Root `keyHeight` is a 10%p fraction on all three layouts; the
  letters layout has 4 rows → **the keyboard occupies ≈ 40 % of the
  window height**, bottom-anchored. On a 960×544 surface that is a
  rect of about (0, 326, 960, 544).

So on stock: the *engine* asks Java to show/hide its own keyboard,
Java reports visibility + occupied rect back, and the keyboard view
sends character codes into the engine's `onKeyDown`. Nothing in this
design uses the Android system IME at all.

## 2. The engine's keyboard surface — census

libengine string census (file VAs; ELF VA == file offset): exactly
three keyboard-related CPJNILib names exist in the binary —

| Method | Sig | Name string VA | In jni_bridge.c? |
|---|---|---|---|
| `setKeyboardVisibility` | `(Z)V` | 0x659122 | ID 148, handler **NULL (no-op)** |
| `isKeyboardActive` | `()Z` | 0x659138 | ID 121, `jni_isKeyboardActive` → **always JNI_FALSE** |
| `getKeyboardRect` | `()Landroid/graphics/Rect;` | 0x65855f | **absent — the NOT FOUND flood** |

(The "Keyboard" strings at 0x664xxx–0x66fxxx are Dear ImGui debug-UI
residue, not CPJNILib.) Cross-check against every bootlog: the
all-logs NOT FOUND census contains exactly one CPJNILib entry —
`getKeyboardRect` ×1,741, all in bootlog-22. Everything else the
engine resolves on this class was already found. There is no
`getText*`, `setText*`, `showKeyboard`/`hideKeyboard`, or any
string-carrying text method in the engine's CPJNILib surface (§6).

## 3. Glue anatomy (libengine, disassembly-verified)

All three methods hang off one CPJNILib glue-state struct (file VA
≈ 0x9047d8; the usual ±2 PIC-base wobble applies to the base only, the
field offsets are read from instructions):

- `+0x10` CPJNILib instance jobject (from the loader's `getInstance`)
- `+0x14` `setKeyboardVisibility` method ID
- `+0x18` `isKeyboardActive` method ID
- `+0x1c` `getKeyboardRect` method ID (self-cached by its wrapper)
- `+0x20/+0x24/+0x28/+0x2c` Rect field IDs: `left`, `top`, `right`, `bottom`
- `+0x30` CPJNILib jclass

The instance-method IDs (+0x14/+0x18) are resolved once by the big
CPJNILib resolver fn (name refs at 0x3d4364 / 0x3d437e; `GetMethodID`,
env slot 0x84) — both names are in the loader's table, so both resolve
silently at runtime. `getKeyboardRect` is *static* in Java and is
resolved by its own wrapper with `GetStaticMethodID` (env slot 0x1c4).

### 3.1 setKeyboardVisibility — glue 0x3d5e60(bool)

Loads instance `[struct+0x10]` + mid `[struct+0x14]` and tail-calls
the CallVoidMethod-family thunk with the boolean as the vararg.
Eighteen BL call sites, clustered in the UI module
(0xa1bd6, 0xa4eb6, 0xab6ae–0xad75e, 0xaf08e — the text-field/widget
code), the Zoom core UI (0x27f228, 0x27f9a4), and one more module
(0x3bbd10/0x3bbd34). Today every one of those calls lands on the
loader's NULL handler: **the engine asks for the keyboard and nothing
anywhere records that it asked.**

### 3.2 isKeyboardActive — gate helper 0x3d5ebc

Calls instance mid `[struct+0x18]` and edge-detects the boolean
against shadow bytes in the engine-globals struct (0x90475c +0xa/+0xb),
returning a tri-state: **1** = active, **0** = inactive, **−1** =
changed this call. Nine call sites (0xa4d1a, 0xa503a, 0xab758, 0xac7d4,
0xad5c6, 0xaeeda, 0xaf218 in the UI widget module; 0x27f8f2; plus the
getKeyboardRect wrapper itself). Today it can only ever return the
"inactive" family of answers, because `jni_isKeyboardActive` is a
constant FALSE.

### 3.3 getKeyboardRect — wrapper 0x3d5f3c(out16)

Four BL call sites: 0xa2740, 0xab760, 0xad5ce (UI module) and 0x14ee2e
(game module). Instruction-level behavior:

**Phase 1 — runs while `[struct+0x1c] == 0`:**
1. `GetStaticMethodID(env, jclass, "getKeyboardRect",
   "()Landroid/graphics/Rect;")`; the result is stored to
   `[struct+0x1c]` **even when it is 0**.
2. Calls the method once via a CallStaticObjectMethodV helper
   (0x3d60a0) to obtain a Rect object, takes its class with
   GetObjectClass, and resolves the four field IDs with
   `GetFieldID(class, "left"/"top"/"right"/"bottom", "I")`
   (env slot 0x178), caching them at +0x20..+0x2c; deletes both local
   refs. (In FalsoJNI, GetFieldID matches by name only → these return
   the loader's already-registered IDs 300–303.)

**Phase 2 — every call:**
1. Zeroes the caller's 16-byte out struct.
2. Calls the gate helper (3.2). **Only if it returns exactly 1** does
   it call `getKeyboardRect()` again and read the Rect's fields with
   GetIntField (env slot 0x190), converting Android (top-left origin)
   to engine orientation using the globals height cell `H`:
   - `out[0] = left`
   - `out[1] = H − top`  ← the keyboard's height above the bottom edge
   - `out[2] = right − 1`
   - `out[3] = H − bottom − 1`
3. Deletes the Rect ref and returns.

The consumer pattern is visible at 0xab758 (a text-widget update):
`state = gate(); getKeyboardRect(out); if (state == 1 &&
out[1] != out[3]) { … }` — i.e., "keyboard active AND rect has real
extent" (out[1] ≠ out[3] ⟺ top ≠ bottom−1) drives the widget's
covered/shifted bookkeeping (bytes at widget +0xee0/+0xee1), using
out[1] (keyboard height in surface px) as the shift amount.

### 3.4 What a missing method actually does here

FalsoJNI's GetStaticMethodID logs NOT FOUND and returns **0** — not
−1, and the wrapper does not treat 0 as "skip": it caches the 0 and
continues into phase 1's remaining calls with mid 0. Because the
cached value stays 0, **phase 1 re-runs on every wrapper call** — that
retry is the flood (≈1–3 calls per frame while the setup screen's
widget polls; 1,741 lines from the name-field tap to log end, each
line two unbuffered `sceIoWrite`s plus a console print — the same
cost class that made build L run at 1 fps). The phase-2 *call* is
never reached today because the gate never returns 1.

Two traps this anatomy exposes:

- **Zero rect ≠ missing method.** With the method unimplemented and
  the gate at 0, the wrapper hands consumers a *zeroed* out struct —
  read as "no keyboard". Benign for layout; the damage is the flood
  and the session that can never proceed (§4).
- **The dummy-rect trap.** If `getKeyboardRect` were registered
  naïvely (return `&dummy_rect` like `getSafeArea`), the engine would
  read the loader's **global** Rect field values — FalsoJNI's
  `GetIntField` ignores the object and returns `fieldsInt[fieldID]`,
  currently left=0, top=0, right=960, bottom=544. With the gate at 1
  that yields `out[1] = H − 0 = H`: **the widget would believe the
  keyboard is a full screen tall** and shift the editing UI by a
  whole display height. Any implementation must maintain the field
  values (300–303) in step with the keyboard state (§7).

## 4. The setup screen — what Lola actually hit

### 4.1 bootlog-22 timeline (build T)

Scene switches: TITLE → MENU_OLD → TITLE → OPTIONS_OLD → TITLE →
MENU_OLD. MENU_OLD is the game-setup scene (its constructor at
0x14d2c8 stamps the scene-name field from the "MENU_OLD" string; its
game-module string cluster carries "PLAYER 1", "PLAYER 2", "Invalid
player selected", "LANMenu").

Her taps, converted to the 800×480 logical space with the view
transform `((x−27.166667)/1.133333, (y−0.5)/1.133333)` — the same
numbers INPUTSTATE stored, e.g. (457, 63):

| Vita tap | Logical | Era | Reading |
|---|---|---|---|
| (74, 472) | (41, 416) | MENU_OLD #1 | bottom-left → returned to TITLE (back zone) |
| (75, 385) | (42, 339) | MENU_OLD #1 | left column; no navigation |
| (93, 472) | (58, 416) | OPTIONS_OLD | bottom-left → back to TITLE |
| (484, 385) | (403, 339) | TITLE | a TITLE entry → opened MENU_OLD |
| **(546, 73)** | **(457, 63)** | MENU_OLD #2 | **top-center: the PLAYER 1 name field** |

The getKeyboardRect flood begins in the **same drain** as the
(546, 73) tap's UP record and runs unbroken to the end of the log
(line 662 → 2422). No further taps were logged after it
(POLL: touch_reports=6 for the whole run). The tap focused the name
field's text widget, the widget called the setKeyboardVisibility glue
(§3.1 — swallowed by our no-op), entered its editing state, and began
polling gate + rect every update.

### 4.2 Why she is stuck — the precise shape

The widget is now in a keyboard session that can never complete:

1. It asked for the keyboard (no-op'd). The engine-side session state
   does not depend on our answer, so the field is "being edited".
2. It polls `isKeyboardActive` → our constant FALSE, and
   `getKeyboardRect` → NOT FOUND flood. The stock session-ending
   events (typed characters; Done = code 10, which on stock also
   hides the keyboard from the Java side) **cannot occur** — there is
   no input path that produces them (§5).
3. While a text widget holds the editing state, the screen's ordinary
   tap routing is captured by the session (the widget is waiting for
   the keyboard the engine believes it requested), and every frame
   pays the flood's I/O cost on top.

**Is a typed name a hard gate for color + confirm?** The evidence
does not support a hard gate: nothing in the method census or the
widget consumer conditions progress on text *content*; the scene
ships default player labels ("PLAYER 1"/"PLAYER 2" strings live in
its own module), and on stock the field starts pre-filled, so a
player who never taps the name field can pick a color and confirm.
Lola never got to test that path in build T — her first MENU_OLD
visit's taps were the left column, and her second visit's only tap
was the name field itself. The honest verdict: **text entry is not
proven to gate progress; the un-completable keyboard session is the
blocker.** The fix build's protocol (§7.5) tests the default-name
path first, then the IME path.

## 5. How text enters — onKeyDown, decoded

`Java_com_blitwise_engine_jni_CPJNILib_onKeyDown` (0x3d4d68, 892 B;
args: env, class, **jint code in r2**). The loader has never resolved
this export (input.c/main.c resolve only onTouch / onGamepadButton /
onGamepadAxis).

- **The argument is a character code, not an Android KEYCODE.** Proof
  chain: the Java listener (§1) forwards layout codes / unicode
  chars; the layout XML's codes are ASCII (letters lowercase) plus
  8/10/32/−1/−10/−11/−12; the handler's own table (below) is built
  for exactly that alphabet.
- **Translation table** (built once, guarded by a byte in the glue
  struct, at bss 0x903e5c, 256 bytes): a per-code map —
  identity for digits and most punctuation, **lowercase letters
  folded to uppercase** ('a'→'A', …), control codes preserved
  (8→8 backspace, 10→10 enter, 16→16 shift, 27→27 escape), and a
  QWERTY-position permutation in the sub-0x20 range. The handler
  sign-extends the code as a **byte**: negative codes take a side
  path (gated on a module byte at 0x7078fe; the layout/shift custom
  codes −1/−10/−11 are the ones it recognizes) and otherwise produce
  **no records** — consistent with Java treating them as layout
  business.
- **Records pushed per call** (non-negative code, translated code T,
  mutex-guarded state array at 0x901a30 tracking per-code down-state):
  1. a **type-3** record: +0x04 byte = T, +0x0C word = the raw code —
     the "character typed" event;
  2. if the code is a shifted/punctuation class, a **type-1** record
     with code 0x10 (16) — a synthetic SHIFT key-down;
  3. a **type-1** record with +0x04 = T — the key-down event proper
     (this is the record class TITLE's onEvent matches for ESC:
     type 1, code 0x1B).
  …and in parallel it appends copies to the **JNI deferred list**
  (the list the frame tail flushes into the ring at 0x3d3b6a),
  including a **type-2** record (key up). **One `onKeyDown` call is
  therefore a complete, self-releasing key tap** — which is exactly
  what a virtual-keyboard tap is on stock.
- **`onKeyUp` (0x3d52dc) is a 2-byte `bx lr`** (dynsym size 2): a
  deliberate native no-op. Java calls it on every release; the engine
  ignores releases because onKeyDown completes the tap itself. The
  loader should not call it (resolving it is harmless; calling it is
  a no-op).
- **Downstream consumption:** records 1/2/3 flow through the same
  Loop B drain as everything else (bug 33 §§39–47): the preprocessor
  doesn't store them into the touch state; unconsumed records are
  dispatched to the current scene's `onEvent` slot (+0x2C via
  forwarder 0x2a8ea6). Scenes/widgets that edit text consume the
  type-3 (char) and type-1 (key) records there; TITLE's stub (§6)
  shows the pattern — match type 1, read code at +0x04.

### No commit channel exists

The full CPJNILib native surface (Java class + engine resolver +
strings) contains **no method that carries a finished string into the
engine** — no `setText`, `onTextEntered`, `commitText`, or similar.
The only string-typed natives are purchase/parse/push plumbing. Any
design that "hands the engine the final name" has nowhere to hand it:
**per-character `onKeyDown` injection is the only faithful text
path.** (The KeyboardView listener's `onText(CharSequence)` callback
in `C.d` is an empty method — even the stock Java side never uses a
string channel.)

## 6. Buttons at menus — verdict

Question: do any scenes' `onEvent` handlers consume gamepad records
12/13, or is there a polled gamepad state — and does the loader owe
buttons anything?

- The dispatch is uniform (bug 33 pass 6): Loop B drains records;
  the preprocessor stores touch/axis state and ignores 12/13; the
  rest go to the current scene's `onEvent` (+0x2C).
- The only `onEvent` body fully decoded is TITLE's (0x183820, runtime
  hook-verified in the bug-32 era): it matches **type 1 with code
  0x1B (ESC) only** — gamepad records are ignored by construction.
- `onGamepadButton` (bug 33 pass 4, read end-to-end) writes **no
  persistent state** — its only output is the 12/13 record push.
  There is no gamepad-state struct for scenes to poll for buttons
  (contrast: axes, whose state the preprocessor maintains from
  type-14 records; gameplay stick input rides that path).
- Runtime corroboration (bootlog-22): HIST shows 12:7 / 13:5 records
  drained across TITLE / MENU_OLD / OPTIONS_OLD eras with no scene
  reaction, matching Lola's "buttons don't work".
- MENU_OLD's own `onEvent` body was **not** isolated in this pass:
  scene vtable slots in the file are stored in pre-relocation form
  (the wall the bug-32 runtime hooks bypassed; MENU_OLD's ctor at
  0x14d2c8 points its vptr into the table group at ≈0x6ebc20, but the
  slot-to-function mapping does not survive static reading). This is
  flagged, not papered over: the verdict below rests on the uniform
  dispatch, the decoded TITLE body, the absence of any 12/13 consumer
  or button state anywhere in the push/drain census, and stock
  behavior (CPView forwards hardware gamepad keys to the same
  `onGamepadButton`; Pocket Tanks' Android menus are touch-only).

**Verdict: buttons at menus are dead on stock too. No loader change
is warranted for menus.** The loader's button delivery is already
stock-faithful (input.c sends exactly what CPView sends). One honest
residual: the GAME scene has not been reached yet; if it wants
buttons it will meet them through the same onEvent dispatch the
loader already feeds — a gap there would be engine behavior, not a
missing loader piece.

## 7. The Vita design spec (build U)

Goal: the stock session semantics, with the Vita's native IME dialog
(SceImeDialog) standing in for the game's built-in KeyboardView, and
per-character `onKeyDown` injection standing in for its key taps.

### 7.1 jni_bridge.c — make the three methods one feature

Add a loader keyboard state (in jni_bridge.c, exported via a small
header for main.c/input.c):

```c
static volatile int kb_active = 0;        // Java isKeyboardActive
static volatile int kb_show_pending = 0;  // set by the JNI handler,
                                          // serviced by the main loop
```

- `setKeyboardVisibility` (ID 148): replace the NULL entry in
  methodsVoid with a real handler: read the boolean vararg;
  `kb_show_pending = visible ? 1 : 0` on a *show* request (a hide
  request sets `kb_active = 0` and, if a dialog is open, a
  `kb_abort_pending` flag). **Do not call sceImeDialogInit from
  here** — this handler runs inside `N_onDrawFrame`'s drain, mid-frame
  (§7.3).
- `isKeyboardActive` (ID 121): return `kb_active`.
- `getKeyboardRect`: **new name-table entry {268, "getKeyboardRect",
  METHOD_TYPE_OBJECT}** + methodsObject handler returning
  `(jobject)&dummy_rect` (a dedicated `dummy_kb_rect` int is cleaner
  but the object identity is never inspected — FalsoJNI field reads
  ignore it). Registration alone kills the flood: the wrapper's
  phase 1 runs once, caches mid 268 and field IDs 300–303, and never
  re-resolves.
- **Rect field values are the keyboard state.** `fieldsInt` entries
  300–303 are global (shared with getSafeArea's rect — read once at
  init, before any keyboard session; restore on hide keeps later
  re-reads correct). Maintain them from the keyboard state:
  - active: `left=0, top=326, right=960, bottom=544`
    (544-tall surface; ≈40 % keyboard height per §1: 218 px, so the
    wrapper computes out[1] = 544−326 = 218, out = {0, 218, 959, −1}
    — real extent, out[1] ≠ out[3] ✓).
  - inactive: restore `left=0, top=0, right=960, bottom=544`.
  Update them in the same places `kb_active` changes.

### 7.2 main.c — resolve the key channel

Beside the existing `get_sym` resolutions (~line 1762):

```c
PT_onKeyDown = get_sym(&mod_engine,
    "Java_com_blitwise_engine_jni_CPJNILib_onKeyDown", 0);
```

declared like the other PT_* pointers (input.h/input.c). Do not
resolve/call `onKeyUp` (native no-op, §5).

### 7.3 main.c — the IME service (main loop, once per frame)

New `keyboard_service()`, called in the frame loop **after
`input_poll()` and before `N_onDrawFrame()`** — same thread as
everything else; the JNI handlers only set flags, so no dialog call
ever happens mid-drain:

```
IDLE:
  if kb_show_pending && !kb_active:
      kb_show_pending = 0
      load sysmodule if needed (see 7.4); build SceImeDialogParam:
        sceImeDialogParamInit(&param)
        param.type          = SCE_IME_TYPE_DEFAULT (libime.h: 0)
          — or SCE_IME_TYPE_BASIC_LATIN (1) for a Latin-only name field
        param.dialogMode    = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL (1)
        param.textBoxMode   = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR (2)
        param.title         = UTF-16 "Player Name"
        param.maxTextLength = 32   (cap is SCE_IME_DIALOG_MAX_TEXT_LENGTH 2048)
        param.initialText   = last entered name (loader-kept UTF-16
                              buffer; empty on first use)
        param.inputTextBuffer = kb_text_buf (UTF-16, 33+ WCHARs)
      sceImeDialogInit(&param) == 0  ->  kb_active = 1; state = OPEN
      (set the Rect field values with kb_active, §7.1)
OPEN:
  status = sceImeDialogGetStatus()
  if status == SCE_COMMON_DIALOG_STATUS_FINISHED:
      sceImeDialogGetResult(&result)
      if result.button == SCE_IME_DIALOG_BUTTON_ENTER (2):
          deliver_text(kb_text_buf)         // §7.4 protocol
      // CLOSE (1) / cancel: deliver nothing — mirrors stock
      // (keyboard hidden without Done; field keeps its old text)
      sceImeDialogTerm(); kb_active = 0; restore Rect fields; state = IDLE
  if kb_abort_pending (engine hid the keyboard / scene exit):
      sceImeDialogAbort(); sceImeDialogTerm(); kb_active = 0;
      restore Rect fields; state = IDLE
```

While state == OPEN, **input_poll must not forward touch or buttons
to the engine** — the dialog owns the screen and the pad; the engine
keeps rendering underneath (CommonDialog composites over the app's
presented frames, and our loop keeps presenting — that is the
required cadence: poll status once per frame, never block on it).

### 7.4 deliver_text — the injection protocol

Mirror the stock order in `C.d` (chars while the session is live,
session flag drops, then Done):

```
// 1. Clear the field: PT names are short; send backspaces first so
//    the result is deterministic regardless of the pre-filled text
//    (the field's own content is not readable from the loader).
for (i = 0; i < 24; i++) PT_onKeyDown(&jni, NULL, 8);
// 2. The text itself: UTF-16 -> code points; pass printable ASCII
//    through as-is (the engine's table folds case itself, §5);
//    skip anything outside 0x20..0x7E.
for each char c: PT_onKeyDown(&jni, NULL, c);
// 3. Done: drop the active flag first, then code 10 — the exact
//    stock sequence (Java hides the keyboard, then forwards 10).
kb_active = 0; restore Rect fields;
PT_onKeyDown(&jni, NULL, 10);
// keep the delivered string as next session's initialText
```

Notes: backspace-on-empty is a no-op in the widget; if the field
turns out to append-only weirdness, the first instrumented run will
show it (PUSH lines will carry the type-3 records with raw codes at
+0x0C — the existing Build T instruments already print them).
ENTER handling inside the IME text (multi-line) doesn't arise:
SceImeDialog's ENTER button finishes the dialog rather than
inserting '\n'.

### 7.5 Build/link prerequisites + pitfalls

- **Link:** add `SceIme_stub` to CMakeLists (SceCommonDialog_stub is
  already linked). Header: `psp2/ime_dialog.h` (+ `psp2/libime.h`
  for SceImeType); structs verified in
  `~/workspace/vitasdk-softfp/arm-vita-eabi/include/psp2/ime_dialog.h`:
  `sceImeDialogParamInit/Init/GetStatus/GetResult/Abort/Term`,
  `SceImeDialogParam` (0xA8 B), `SceImeDialogResult.button`.
- **Sysmodule:** the header's usage line names `SCE_SYSMODULE_IME`;
  the loader currently loads no sysmodules explicitly — the build
  must call `sceSysmoduleLoadModule(SCE_SYSMODULE_IME)` before the
  first Init (guard with sceSysmoduleIsLoaded) and treat failure as
  "keyboard unavailable" (stay IDLE, kb_active stays 0 — the game
  remains exactly as playable as build T outside the name field).
- **Threading:** all dialog calls on the main thread from the loop
  service (§7.3). Never call Init/GetResult from the JNI handler
  (mid-drain re-entrancy against the engine's own mutex at
  0x9047c8, which onKeyDown takes) and never block waiting for
  FINISHED — status polling per frame only.
- **Frame cadence:** while the dialog is open the engine's frames
  keep running (the IME is drawn by the OS over our presents). Do
  NOT skip `N_onDrawFrame` — CommonDialog expects the app to keep
  flipping; a stalled loop can wedge the dialog.
- **Prior art in-tree:** none — no loader file references IME today
  (grep: no `sceImeDialog` anywhere in the source tree). The pattern
  (flag from JNI handler + per-frame CommonDialog status service in
  the main loop) is the standard so-loader arrangement; the closest
  in-tree analogue is the loader's own per-frame change-only
  samplers (inputstate_check/viewstate_check) which already run at
  exactly the hook point proposed.

### 7.6 Verification protocol for the build

1. Boot to MENU_OLD **without** touching the name field: pick a tank
   color, tap confirm — establishes whether the default-name path
   proceeds (the §4.2 open question) independent of the IME.
2. Tap the name field: the Vita IME dialog should open (Lola's
   screen is the verdict). Type a name, press ENTER.
   Log checks: NOT FOUND count for getKeyboardRect == small (phase 1
   once, then zero); PUSH lines show the injected sequence — type-3
   records whose +0x0C raw codes spell the backspaces, the name, and
   a final type-1/type-3 pair for code 10.
3. Re-enter the field, cancel the dialog (CLOSE): no injection, the
   field keeps its previous text, no flood, no hang.
4. Full flow onward: confirm → opponent setup (same field machinery)
   → weapon select → match start (the standing playtest queue).

## 8. Open residuals

- MENU_OLD's `onEvent` body not isolated statically (§6) — only
  worth a runtime dispatch-hook run if a future symptom points at
  scene-side record handling; not needed for this fix.
- The engine height cell `H` used in the wrapper's rect conversion
  is assumed to be the 544 surface height; if the first instrumented
  build shows the widget shifting by a wrong amount, read `H` from
  the globals struct at runtime and adjust `top` so that
  `H − top = 218`.
- Whether the setup screen commits a name on Done (10) vs on focus
  loss: stock parity says Done is the commit gesture the Java side
  pairs with hiding the keyboard; the §7.6 run will show it in the
  PUSH/record trace and on Lola's screen.
- The (42, 339) logical tap in MENU_OLD #1 is unmapped (left column,
  no navigation) — likely a color/LAN row element; not load-bearing.
