# Bug 37 — SHOP→GAME switch never latches: "GAME" is not in the scene registry

## Verdict (pass 1, static + bootlog-26)

The switch machinery is healthy and the request is healthy. The request
**fails inside the engine's request function**: `request(mgr, "GAME")`
searches the scene registry, finds no scene object named "GAME", and
returns 0 **without setting the pending pointer** — so the frame-boundary
completer has nothing to arm, SHOP stays current, and SHOP's per-frame
update re-issues the request forever (3,019 times in bootlog-26).

Root cause, one level down: **no GAME scene object is ever constructed
and registered**. Registration is a constructor-side effect (register fn
`0x2a8b90`, 18 call sites = the scene constructors); the roster that
TITLE / MENU_OLD / OPTIONS_OLD / DEPOT / SHOP come from never gains a
"GAME" entry. Why GAME's constructor/registration never runs is the one
question pass 1 leaves open — §5 specifies the diagnostic build that
proves the registry state on-device and the pass-2 hunt for the gate.

The loader's 0x2a8e6a dispatch interposition is **cleared for this bug**
with evidence (§2.3). The keyboard flickers and the
`weaponScriptingScreenLockFlag` throw are both unrelated (§3).

## Symptom (bootlog-26.txt, build W, 7,334 lines)

- Scenes arm normally: TITLE (line 447) → OPTIONS_OLD (482) → TITLE (511)
  → DEPOT (536) → TITLE (711) → MENU_OLD (726) → SHOP (744). The SHOP
  request is immediately followed by the Bug 36 `CONDWAIT` line with a
  positive delta — SHOP arms and stays.
- Lola plays the shop (weapon selection works — her screen report,
  2026-10-14 12:05/12:08: "I can select weapons after picking color but,
  there is no next screen"). Her start tap lands as PUSH type=8/10 at
  (509,501) just before line 765.
- First `SCENE: switch requested -> "GAME"` at line 765, frame ≈3323
  (heartbeat 3300 precedes it; swaps=11). Requests continue to the end
  of the log (last frame 9448): **3,019 requests over ≈6,125 frames
  (≈0.49/frame)**. Engine presents keep climbing (9,275 by frame 9300);
  loader swaps freeze (45); the VIEWSTATE geometry in every request
  pair never changes. No file-IO lines occur anywhere in the spam era.
  Her taps keep registering (PUSH records) throughout.

## 1. THE REQUESTER

**Who:** the SHOP scene's per-frame update function — the function
spanning ~`0xff5f2`–`0x1002f4` in libengine (file offset; runtime
+0x99000000), driven every frame by the completer's tick dispatch
(§2.1) while SHOP is the current scene. It is the shop's timed-action /
layout-flow handler: it walks the scene's action-node list by name
(loop at `0xfffd8`, strcmp via PLT `0x7ca68` against a PIC-resolved
name), accumulates per-action timers (dt thresholds at `0x100086` /
`0x10024e`, step fn `0x100a28`), and fires scene requests when a flow
step completes.

**The GAME request site:** `0x1000f6` (`bl 0x2a8c3c`), inside the
match-start branch at `0x1000be`:

- Query `0x28d7b0(0x1b)` (engine query id 27) must return non-null,
  else the branch is skipped.
- `r8 = *[0x701DE0]` — the scene-manager singleton cell, the **same
  cell the frame tail loads its mgr from** (`0x170620`). One manager;
  the request goes to the right object.
- Name selection: current-scene-name getter `0x2a8db0(mgr)` is strcmp'd
  against PIC string `"GAME"` @ `0x6318ad`; the request name is picked
  from `"GAME"` / `"TITLE"` @ `0x62b3ce` / `"ONLINEMENU"` @ `0x631f2e`
  (the same function carries the LAN/online flow — cf. the rodata
  cluster `'LANMenu: GAME STARTING!\n'` beside `"SHOP"`).
- A second request site at `0x1002f0` in the same update body issues
  the mode-dependent follow-up request (`"ONLINEMENU"` @ `0x1002ea` /
  `"TITLE"`) under its own name check at `0x1001aa`.

**Re-fire condition:** not a retry loop in the switch machinery — the
request function itself contains no loop over attempts, and the
completer (§2.1) never re-asks. The repetition is **SHOP's update
polling its own un-advanced flow state**: the "match start" step fired
once (her tap), the flow now waits for the current scene to stop being
SHOP, and every update pass re-issues `request("GAME")` while that
remains false. Because the request silently fails (§2.2), the state
never advances: ~1 request per 2 frames for 6,125 frames. On stock,
the first request latches, the completer swaps scenes at the next
frame boundary, SHOP's update stops running, and the spam never
exists. The spam is a *symptom of the failed latch*, not a second bug.

(Contrast: SHOP's own name was requested once, from SHOP module code
at `0xb1894` — `request(mgr, "SHOP")` with `"SHOP"` @ `0x62b495` — and
latched immediately, §2.2.)

## 2. THE LATCH

### 2.1 How a switch normally arms

1. **Request** — fn `0x2a8c3c(mgr, name)`: linear search of the scene
   registry (entry-pointer array inline at VA `0x8EE910`, capacity
   255; state block at `0x8EED0C`: init flag +0, count +4 → count dword
   at `0x8EED10`). Each entry is a scene object: vtable at +0, **name
   chars inline at +4**, compared with PLT strcmp (`0x2a8c5e`).
2. **On match** (`0x2a8c68`–`0x2a8ca0`): if `[mgr+4]==0 && [mgr+0]!=0`,
   the outgoing scene is cached via map-insert helper `0x2a8cb8`
   (key = mgr's current-name string at mgr+8); the engine logs
   `"Switching to scene %s"` (fn `0x3d3480`, format @ `0x649ccd`);
   **`[mgr+4] := scene object` (the pending latch — this store at
   `0x2a8c9c` is the only writer of the pending slot)**; returns 1.
3. **On no match** (`0x2a8ca4`): returns 0. No log, no store, no side
   effects.
4. **Arm at frame boundary** — the frame tail (`0x170620`) calls the
   completer `0x2a8e2c` at `0x170788`, gated only on the latch byte
   `0x223aa4()==0` (byte at `0x701DF5`). Completer: if `[mgr+4]!=0` →
   outgoing scene `[vt+0x10]` teardown call, `[mgr+0] := pending`,
   `[mgr+4] := 0`, incoming scene `[vt+0x8]` init call, flag byte →
   `[mgr+0x14]`, `bl 0x223b90`. If pending is NULL, the completer
   tail-calls the **current scene's `[vt+0x14]`** — the per-frame
   tick, which is exactly the dispatch that keeps running SHOP's
   update (and its re-requests) in the spam era.

### 2.2 The contrast pair, instruction level

MENU_OLD→SHOP (worked, bootlog-26 line 744): `"SHOP"` **is** in the
registry (SHOP's constructor registered it — register fn census, §2.4),
so the request took the found path, set `[mgr+4]`, and the completer
armed SHOP at the next frame boundary. SHOP then stayed current for
6,000+ frames (its tick kept being dispatched through the same
completer that would have armed GAME).

SHOP→GAME (fails): `"GAME"` is **not** in the registry. All 3,019
requests took the `0x2a8ca4` exit: return 0, `[mgr+4]` never written.
Every downstream element is thereby starved, not broken: the
completer runs every frame (SHOP's tick proves it), the latch byte
`0x701DF5` is 0 (the completer is being called — same gate), the tail
machinery presents at 60 fps throughout.

Why the existing logs can't show this directly: the loader's SCENE
hook is an **entry** hook on `0x2a8c3c` — it logs the request string
and swaps count, never the return value, and engine stdout
(`"Switching to scene %s"`) is not routed to the bootlog at all (0
occurrences in bootlog-26, including for the arms that worked). The
found/not-found distinction has been invisible in every log so far;
§5's diagnostic build adds exactly that visibility.

### 2.3 The 0x2a8e6a interposition — CLEARED for Bug 37

Per the §4.4 preserve rule, evidence, not assumption:

- **Patch census** (loader source): the loader patches exactly —
  get_child trampoline (Bug 27/28), entry hook on `0x2a8c3c` (logging
  only), interposition on forwarder `0x2a8e6a`, present-count hook
  `0x3d6628`, push trace `0x3d4c8c`. Nothing patches the request fn's
  body, the completer `0x2a8e2c`, the register fn `0x2a8b90`, the
  append fn `0x2a8b4c`, or the registry cells.
- **Wrong slot anyway:** `0x2a8e6a` dispatches vtable **+0x18**. The
  latch path uses the request fn, the completer, and slots +0x8/+0x10/
  +0x14 — none of which pass through `0x2a8e6a`. The failure occurs
  strictly *before* any dispatch: with `[mgr+4]` never set, no +0x18
  call for GAME is ever generated.
- **Positive control in the same log:** the MENU_OLD→SHOP arm flowed
  through the interposition (tail mode-3 arm `0x1707f0` → `0x2a8e6a`)
  and latched. The interposition's replicated deref/NULL-check/
  slot-load behaved identically to stock there.

The §4.4 A/B obligation (interposition removed + pk.cfg deleted)
still stands on its own merits for the cleanup phase — but it is not
Bug 37's cause, and removing it would not change a single GAME
request's outcome.

### 2.4 Registry membership — who registers, and the GAME-shaped hole

Registration is constructor-driven:

- **Append fn `0x2a8b4c(obj)`**: one-time memset of the 255-pointer
  array, then `array[count++] = obj`.
- **Register fn `0x2a8b90(obj)`**: stores the shared scene vtable
  (`[obj+0] = PIC+8`, name SS-string header at +4/+8 zeroed), then
  calls append. Callers patch the real vtable, **name chars (inline at
  +4)**, and flags into the object immediately after (e.g. ctor at
  `0xff62c`: `strh 0x54 → [obj+8]` right after the call).
- **18 call sites** (BL census): `0x9b632, 0xaf53a, 0xc0b26, 0xd4660,
  0xfb716, 0xfca80, 0xff634, 0x1318d8, 0x1453a2, 0x14cb04, 0x14d2d0,
  0x14fd7a, 0x151784, 0x156282, 0x163d94, 0x164108, 0x1816d4,
  0x3944a6`. Names resolved in place for two: `"MOREGAMES"`
  (ctor caller `0x14fd7a`), `"OPTIONS_OLD"` (`0x156282`); the pattern
  is identical at the rest (TITLE / MENU_OLD / OPTIONS_OLD / DEPOT /
  SHOP / INTRO2 / ABOUT / LAN variants account for the roster the
  game actually arms).
- **No site registers "GAME".** The only code references to the
  `"GAME"` string (`0x6318ad`) anywhere in .text are the requester's
  own strcmp/request sites (`0x1000e4`, `0x1001b8`) — found by a PIC
  ldr/add sweep of the whole game-code range, the same sweep that
  finds `"SHOP"` at its ctor-adjacent request site (`0xb1890`) and
  `"TITLE"` at `0x140942`/`0x1409a2`. There is no pointer to the string
  in .data/.got/.data.rel.ro either (PIC-relative addressing is the
  engine's only style, verified across all scene names).

So a GAME scene object is never constructed in our runs. Whether its
constructor exists but is gated off, or is only reached through a
builder path our flow never takes, is the pass-2 question (§5.2) —
the registration *effect* is absent either way, and §5.1's build
proves it on-device before any fix is attempted.

## 3. THE GAME INIT FALLBACK — there isn't one; init is never reached

GAME init cannot "stop partway": with no scene object and no latch,
no init code runs at all. The two candidate fallback signals from
the brief both dissolve:

### 3.1 The setKeyboardVisibility flickers are SHOP-era widget noise

Bootlog-26 lines 2212–2254: four
`setKeyboardVisibility(true)` → `(false)` pairs, frames ≈4904–5125 —
**~1,600 frames after the spam began**, so they cannot be its cause.
Loader-side anatomy (jni_bridge.c:183–197, main.c keyboard_service):
the `(true)` handler only raises `kb_show_pending`; the `(false)`
handler logs unconditionally, clears pending, and calls
`kb_set_active(0)` — which logs only on change. In the flicker blocks
the pairs are **adjacent lines with no `sceImeDialogInit`, no
`KB: IME dialog OPEN`, no `isKeyboardActive -> 1` line anywhere in
the log (8 KB lines total, all flickers)**. The dialog never opens
because the same input drain that raises the flag clears it again
before keyboard_service's once-per-frame poll — i.e., the engine
toggles its own on-screen keyboard widget open→closed inside single
input batches, in response to Lola's taps at (610–633, 162–189) in
the shop (taps at the bottom, y≈479–501, do not trigger them). This
is the name/text-widget surface from Bug 34 being poked inside SHOP,
not a GAME-init attempt: a GAME-init keyboard toggle would be
preceded by a latched switch and followed by scene-load activity;
there is none.

### 3.2 `game.weaponScriptingScreenLockFlag` — init-era optional-key probe

- The throw appears **once** in bootlog-26 (line 420), in the init-era
  cluster (lines 397–434) alongside `bad_lexical_cast` ×10 and
  ptree_bad_path for `game.terrainZoomMode`, `moreGames.lastModified`,
  `moreGames.viewedCount`. Backtrace ret0=`0x990a01a3` = the Bug 27/28
  get_child trampoline region — i.e., the read goes through the
  loader's ptree layer, whose standing policy fakes only `<xmlattr>`
  queries and lets other missing keys throw; the engine catches and
  applies its default.
- The full key string lives at `0x6327d5` (short form
  `weaponScriptingScreenLockFlag` at `0x18572`); it is a config-tree
  probe for a Deluxe weapon-scripting screen lock toggle. It is read
  **once per boot**, never per GAME attempt (the spam era contains no
  THROW lines at all), and the on-disk configs (pk.cfg 1679 B,
  engine.cfg 613 B, pt.cfg 445 B) don't carry it — stock behaves the
  same way: absent key → throw → default. It gates a scripting
  sub-screen's lock state, not scene arming, and it sits *downstream*
  of the actual failure anyway: the arm never happens, so nothing the
  flag controls is ever consulted in the SHOP→GAME transition.

## 4. OTHER GATES — all clear

- **Assets / spritecache:** the spam era contains **zero file-IO
  lines** (last AAsset/datadir lines are boot-era). If the arm were
  waiting on GAME-era sprites, the engine would first have to build
  the scene — it never tries. The parked spritecache issue
  (asset_mgr.c `so.spritecache.tmp`, ready patch in
  `spritecache-fix-ready.md`) applies *after* a successful arm, at
  GAME scene init; it is not in play here.
- **Audio:** no FMOD error lines in bootlog-26 (only libfmod load
  lines 7–80). SHOP runs with the audio path live; nothing at the arm
  waits on audio init.
- **Timed-action executor:** SHOP's own timed actions demonstrably
  run (the requester *is* the timed-action handler, and the shop UI
  stays responsive). The completer's call gate is the single latch
  byte `0x701DF5`, shown 0 by the fact that SHOP's tick dispatches
  every frame.
- **Purchase/commit JNI:** no purchase/consume lines anywhere in
  bootlog-26. The shop exit is purely the name-based switch above —
  no commit call is made at the (509,501) start tap, so no stub
  answer (PURCHASED(2) or otherwise) is consulted on this path. The
  per-weapon lockedFlag question remains a *post-arm* playtest item,
  not a gate here.
- **Mode/module gates (Bug 32 machinery):** the tail's mode-3 pending
  check `0x3d6e30` reads the mode dword and returns module[+0x70]; the
  scene-load fn `0x16f9b0` (builder `0x2236a0`, registration loop
  `0x2c84ee`, completion dispatch via forwarder `0x2a8e76`) ran for
  the scenes that did arm. These gates decide *which module loads*,
  not whether a registered scene can latch — and GAME's absence is
  at the registry level, below them. They re-enter the story in §5.2
  as suspects for *why* GAME never registers.

## 5. THE FIX SPEC

### 5.1 Diagnostic build X (one variable: instrumentation only)

Extend the existing `0x2a8c3c` entry hook and add an exit hook;
change nothing else. Log:

1. **Request outcome** — exit hook at the request fn's shared exit
   (`0x2a8ca6`): `SCENE: request "%s" -> %d (pending=%p current=%p)`,
   r0 = return value, mgr+4 / mgr+0 sampled at exit. Throttle:
   first 5 per distinct name, plus every `"GAME"` request capped at
   20.
2. **Registry roster dump** — once per boot, at the first request
   whose name is `"SHOP"` or `"GAME"`: for i in
   `*(uint32_t*)(text_base + 0x8EED10)`: entry =
   `*(void**)(text_base + 0x8EE910 + 4*i)`; log
   `REGISTRY[%d] = "%s" (obj=%p)` with the name read from entry+4.
   (Runtime bases: text_base = 0x99000000 in bootlog-26, line 64.)
3. **Latch state per heartbeat during a GAME burst** — while GAME
   requests are active, the existing 300-frame heartbeat additionally
   prints `[mgr+0]`, `[mgr+4]`, and the latch byte at
   `text_base + 0x701DF5`.

Predicted result (which would confirm this analysis on-device):
roster contains TITLE/MENU_OLD/OPTIONS_OLD/DEPOT/SHOP/MOREGAMES/…
and **no "GAME"**; every GAME request returns 0; pending stays NULL;
latch byte stays 0. If instead "GAME" *is* in the roster, the bug
moves into the request fn's guard (`[mgr+4]==0 && [mgr+0]!=0`) or the
name compare — the same build's pending/current samples discriminate
that case too.

### 5.2 Pass 2 (static, before any fix build)

Find GAME's constructor and its registration gate:

- Census the 18 register-fn callers' objects (vtable + flags patched
  in after the call) to name the full roster statically; the GAME
  ctor — if it exists as a 19th path — should surface as a scene
  class whose registration call is *conditional* or lives outside
  this census (e.g., inside the scene-load builder `0x2236a0` or the
  `0x2c84ee` registration loop of `0x16f9b0`).
- Prime suspects for the gate: (a) the mode/module machinery — the
  requester's own name set is mode-dependent (`"GAME"` offline vs
  `"ONLINEMENU"` online), so GAME's registration may be conditioned
  on a mode/config value our environment answers differently;
  (b) a builder that stock runs from the Android activity flow and
  our loader's flow skips; (c) a config/ptree or CPJNIParse answer
  consulted at scene-module build time.

### 5.3 The fix (shape; exact site after 5.1/5.2)

Loader-side, root-cause at the gate, in order of preference:

1. If registration is gated on a condition the loader supplies
   (config key, JNI answer, mode value): correct that supply. The
   engine then constructs and registers GAME itself.
2. If a scene-module builder that registers GAME never runs in our
   flow: trigger the engine's own builder path (the `0x16f9b0` /
   `0x2236a0` machinery) the way stock does.
3. **Not acceptable:** fabricating a scene object in the loader,
   patching the registry by hand, or forcing `[mgr+4]` — the object,
   its vtable, and its name must come from the engine's own ctor.

Expected follow-ons once GAME registers and arms (already queued,
not part of this fix): spritecache rebuild at GAME init
(`spritecache-fix-ready.md`), the boost-uuid construction at match
start (Build V shim, first real exercise), per-weapon lockedFlag
playtest, then the §4.4 A/B and the cleanup build.

---
*Pass 1: no build. Evidence: bootlog-26.txt (build W) + libengine.so
disassembly (ELF VA == file offset; runtime text_base 0x99000000).
Screen state per Lola's report only, per her standing rule.*

### 5.1 RESULT (2026-10-14, Build X run, bootlog-27.txt) — CONFIRMED

Prediction held exactly. Roster dumps at first SHOP arm and at first
GAME request are identical: count=17 — LAN, ONLINEMENU, ABOUT,
BULLETIN, DELUXE, DELUXE_OLD, DEPOT, INTRO2, MENU, MENU_OLD,
MOREGAMES, OPTIONS, OPTIONS_OLD, SHEET, SHOP, TITLE, ZOOM BENCH —
**no "GAME", and nothing registers it later**. Working chain verified
through the same hook: INTRO2 -> TITLE -> MENU_OLD -> SHOP all return
1 with pending armed. Every GAME request returns 0, pending stays
NULL, current stays SHOP; LATCH heartbeats counted 2,398+ requests
during the sit, game_failed == game_reqs (100%). Lola's photo of the
run shows both loadouts full (10/10), "0 remaining" — selection was
complete; the advance trigger fired engine-side and found no scene.
Bug 37 mechanism is settled: **the GAME scene object is never
constructed**. Pass 2 proceeds per §5.2.

## 6. Pass 2 — the registration census (2026-10-14, static)

Disassembly cached at `engine-disasm.txt` (arm-vita-eabi-objdump -d
of libengine.so, 1,895,985 lines). Calls to the register fn
`0x2a8b90`: exactly **18 sites** — 0x9b632, 0xaf53a, 0xc0b26,
0xd4660, 0xfb716, 0xfca80, 0xff634, 0x1318d8, 0x1453a2, 0x14cb04,
0x14d2d0, 0x14fd7a, 0x151784, 0x156282, 0x163d94, 0x164108,
0x1816d4, and one far outlier 0x3944a6 — against a 17-scene runtime
roster. Symbol-based function attribution is useless (engine code
sits under a few giant boost symbols); work proceeds by raw address
and local structure.

First structural find: some sites are not construction sites at
all — e.g. 0x163d94 is the body of a 3-instruction wrapper
(`push {r7,lr}; mov r7, sp; bl 0x2a8b90` at 0x163d90), a per-scene
registration thunk whose caller does the real construction. Census
therefore splits into thunk sites (trace the thunk's callers) vs
inline sites (read the construction in place). Classification +
caller trace in progress.

## 7. Pass 2 findings (2026-10-14, static — supersedes §5.2's model)

- `0x2a8b90` is not a register fn: it is the scene BASE-CLASS CTOR.
  It writes a vtable at +0, the inline name "DEFAULT" at +4..+11
  (pool words at 0x2a8bb0 spell it), then calls the registry-link
  fn `0x2a8b4c`, which appends the object to the registry array
  (255 slots, flag+count control block) — the same registry the
  switch fn searches (roster-validated addresses).
- Byte-exact BL scan confirms exactly 18 call sites of the base
  ctor = 18 derived scene ctors. Name literals sit in each ctor's
  literal pool: site `0x1318d8` has "GAME" — that ctor (entry
  0x1318d4) is GAME's. It is called ONLY from the init fn at
  0x858b0, which constructs the static object at VA 0x7210E8
  (method validated: SHOP's computed object VA 0x7309B4 matches
  the runtime roster exactly).
- GAME's init fn IS in .init_array (entry 0x858b1, index 82),
  bracketed by working inits (46 below, SHOP at 103 above) — a
  loader early-stop cannot explain its absence. Its derived ctor
  is well-formed and parallel to SHOP's (vtable, name "GAME",
  flag byte at +0x141).
- Remaining static ambiguity (not chased further): GAME's init
  fn, after the ctor call, makes an indirect call through a GOT
  slot whose resolution resisted three PIC hand-computations;
  SHOP's equivalent step is a direct veneer call. If GAME's ctor
  ran, registration is unconditional inside the base ctor — so
  the runtime question is binary: did the ctor execute?

## 8. Build Y (2026-10-14) — the object probe

Diagnostics only (= X + probe, no behavior change). Because
init_array runs during so_load — before any post-load hook can
install — Y hooks nothing: scene_registry_dump() additionally
reads GAME's static object (tb + 0x7210E8): vtable word, name
bytes at +4, flag byte at +0x141, with SHOP's object as the
formed control. Formed GAME object + absent roster entry would
indict the registry; zeros indict init execution. Fires at both
roster triggers (first SHOP arm, first GAME request), so any run
that reaches the shop answers it. Build Y VPK: 654,386 bytes.

## 9. ROOT CAUSE + FIX (2026-10-14) — Bug 37 CLOSED pending Lola's run

Build Y's probe (bootlog-28) read GAME's static object as all zeros
at both roster triggers (SHOP control fully formed): **the ctor
never executes**. The cause was in OUR OWN loader, not the engine:
`lib/so_util/so_util.c` carried a diagnostic init-skip list,
`so_init_skip[] = { 82 }`, left over from isolating an early
startup crash and never removed. `so_initialize()` skips those
indices for libengine only — and index 82 IS the GAME scene init
(0x858b0). bootlog-28 lines 168-169 show it firing:
`init: [82/220] calling 0x990858b1` -> `SKIPPED (diagnostic skip
list)`. It had logged that line on every boot since the skip was
added; nobody was reading it. The engine, the registry, the
switch, and the shop were all innocent the whole time.

**Build Z** = Build Y with the skip removed (list now `{ -1 }`,
comments updated in-tree). Risks/unknowns: why #82 was flagged
crashing in its era is unrecorded; it predates many loader fixes,
so it may run clean now — Lola's run discriminates instantly (the
init trace dies at [82/220] if it faults). With GAME constructed,
the switch should arm and the match begin; the first queued
follow-on is the spritecache rebuild at GAME init
(`spritecache-fix-ready.md`, deliberately NOT in this build).

## 10. CONFIRMED (2026-10-14 13:18 PDT)

Lola, on Build Z: "HOLY SHIT ITS FINALY PLAYING!!!!" — the match
runs on her screen (her report is the verdict, per her standing
rule). Init #82 executes, GAME constructs and registers, the shop
switch arms. **Bug 37 CLOSED.** Queued follow-ons unchanged:
spritecache at GAME init, per-weapon lockedFlag playtest, audio
proof, Bug 34 IME test, §4.4 A/B, cleanup build.
