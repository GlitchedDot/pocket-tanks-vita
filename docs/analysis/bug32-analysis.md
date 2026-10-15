# Bug 32 — TITLE is requested and switched in the engine log, but Lola's
# screen never leaves the loading screen

**Date:** 2026-10-13 (evening) · **Pass type:** analysis only, no builds ·
**Evidence base:** `bootlog-9.txt` / `log-9.txt` (build 2026-10-13-G run),
Lola's screen report of 2026-10-14T01:20:35Z ("just the loading screen
with the loading bar. still no main menu"), loader source in
`pockettanks-vita/source/` (main.c, jni_bridge.c), and the APK's
decompiled Java in `jadx-out/` (CPJNIHTTP.java).
**Standing rule applied:** logs establish engine state, never what's on
her screen. Her screen report outranks every claim below.

## What is verified (not theory)

- Bug 31 is closed: CLOCK readings climb, INTRO2 completes, `SCENE:
  switch requested -> "TITLE"` fires (bootlog-9 line 11811), and the
  game log prints `Switching to scene TITLE` at 18:14:22. The weapon
  packs then unlock on schedule (18:14:34 / 18:14:54 / 18:15:14).
- **No `onDrawFrame returned` line ever appears after frame 12.**
  bootlog-9 has exactly 13 frame returns (frames 0–12, all r=12288).
  In bootlog-7/8 the same call returned 12288 for 1,185 / 179
  consecutive frames and was logged every time. main.c logs the return
  only when `r != 0` — so after frame 12, either the call stopped
  returning, or it started returning 0 and became invisible to the log.
  The current instrumentation cannot distinguish those two cases.
  That ambiguity is itself a finding (see Discriminator).
- After the TITLE request, the bootlog segment (~185K chars, ~2,800
  lines) contains, in order: TITLE-scene asset activity exactly once
  each (`zoom/scroller.ttf` after a failed `languages/scroller-en.ttf`
  attempt — a normal fallback, the second open succeeds;
  `moregames/catalog.xml`; `bulletin/badge.xml`), two `pk.cfg`
  write/read pairs (the pack-unlock saves), the game log's three
  `fprintf` unlock lines — and **394 poll sets** of three JNI lookups:
  `getHTTPStatusCode(I)I`, `getResponseHeaders(I)[Ljava/lang/String;`,
  `getTransferStatus(I)I`, every one `NOT FOUND` on class `""`.
- The same three lookups poll ~17–18 sets per frame during frames
  1–12 (216 sets before the TITLE request). They are **new in build
  G**: bootlog-7 and bootlog-8 contain zero `getTransferStatus`
  occurrences. They started ticking once the clock advanced — i.e.
  some transfer manager is driven by the same delta time that was
  frozen before.
- Zero `INPUT:` lines after the TITLE request (Lola's two taps land in
  the frame-12 segment). input_poll runs in the loader loop *outside*
  onDrawFrame, so this is weak evidence only — she may simply not
  have tapped again.

## The JNI gap (code-verified in jni_bridge.c)

The three polled methods belong to
`com/blitwise/engine/jni/CPJNIHTTP` (jadx-out CPJNIHTTP.java). The
bridge stubs part of that class — `initGet`/`initPost` return −1,
`getData` returns NULL, `getBytesDownloaded` returns 0, `close` is a
no-op (jni_bridge.c method IDs 230–234, under the comment "CPJNIHTTP
stubs (update-check path must not block)") — but the three methods
the engine actually polls every tick were never added to the method
table:

- `getTransferStatus(I)I` — Java returns the transfer's status field:
  **0 = CP_HTTP_ERROR, 2 = CP_HTTP_IN_PROGRESS, 3 = CP_HTTP_SUCCESS**
  (CPJNIHTTP.CP_HTTP_STATUS).
- `getHTTPStatusCode(I)I` — the HTTP response code.
- `getResponseHeaders(I)[Ljava/lang/String;` — the header array.
- (`getEstimatedBytesTotal(I)I` is also missing, but does not appear
  in this log.)

Because GetStaticMethodID fails, the engine's glue gets a NULL method
ID on every poll (hence the per-poll NOT FOUND flood — the same
cache-miss artifact as the old getMusicFlag flood) and the calls fall
through to the bridge's default return of 0. For getTransferStatus
that 0 happens to equal CP_HTTP_ERROR — a terminal state on paper —
but the engine demonstrably keeps polling, so either its poll loop
does not treat the glue's 0 as terminal (e.g. it re-checks because
the method ID never caches, or its own enum/path differs), or the
polling is a per-tick manager that re-arms regardless. **Do not sell
"return 0 = ERROR, already correct" as settled — the log contradicts
"settled" 394 times.**

## Two competing readings of the post-TITLE segment

**(A) Frame 13 never returns.** All post-TITLE activity happens
inside the first onDrawFrame call in which TITLE is requested:
TITLE init loads its assets once, saves pk.cfg, and then spins
polling the HTTP transfer (update-check / bulletin fetch) waiting
for a completion that our bridge can never signal. Supporting: the
asset loads happen exactly once each (a ticking scene would not
re-load badge/catalog/fonts, but a stuck init would also only load
them once — this fits A cleanly); graphics_swap is never reached
again, so the panel keeps showing the last INTRO2 frame — which is
exactly Lola's report: finished loading bar, a few blinks during
the transition, then a steady loading screen. The 20-second-regular
pack unlocks are the objection: they look like a ticking timer —
but they are game-log writes that could equally be driven from
inside the same call's store-check sequence.

**(B) TITLE ticks, but its frames return 0 and draw nothing visible.**
onDrawFrame's return word changed with the scene (12288 was an
INTRO2/loading-phase value), so frames continue unlogged at ~18
poll sets/frame (≈22 frames ≈ the 52 s game-log window, matching
the unlock cadence), while TITLE's draw either renders behind a
persistent loading overlay or produces nothing the panel shows.
Supporting: the unlock timing regularity; the per-frame poll rate
before the switch. Objection: nothing in the segment marks a frame
boundary, and a normally-drawing TITLE should have replaced the
loading artwork on screen — it did not, per Lola.

## Discriminator — one instrumented run settles A vs B

Build H needs only logging changes plus the JNI fix below:

1. In main.c's frame loop, log **every change** of onDrawFrame's
   return value (`if (r != last_r) blog(...)`) and log the frame
   counter every 60 frames regardless of r. If frame numbers keep
   climbing after TITLE → reading B. If the counter freezes at 13
   while polls accumulate → reading A, proven.
2. Count `graphics_swap()` calls after the TITLE request (one blog
   line summarizing at capture end is impossible — log swap #1, #10,
   #100 after the switch instead). Swaps continuing while the
   screen stays on the loading artwork would additionally convict
   the draw content, not the present path (already ruled out for
   INTRO2 in bug31-analysis.md).
3. Log each forwarded touch in the same build and have Lola tap
   once ~30 s after the bar finishes: an INPUT line timestamped in
   the loader loop proves the loop is alive (B) or dead (A) with a
   single data point.

## Fix specified (regardless of which reading wins)

Same pattern as the verified getMusicFlag fix (method ID 267):

1. Register a real class token for
   `com/blitwise/engine/jni/CPJNIHTTP` in jni_bridge.c (distinct
   from the dummy class), so the engine's glue caches it.
2. Add stable method IDs: `getTransferStatus` → return **0
   (CP_HTTP_ERROR)** as a deliberate terminal answer for any
   transfer key (our initGet/initPost already refuse transfers
   with −1; ERROR is the honest terminal state for a transfer
   that will never exist), `getHTTPStatusCode` → 0,
   `getResponseHeaders` → an empty String array (not NULL — Java
   code iterating headers would NPE on stock; an empty array is
   the safe terminal value), `getEstimatedBytesTotal` → 0.
3. Expected effect: the NOT FOUND flood ends (method IDs cache
   after first success), and any engine path waiting on a terminal
   HTTP state receives one in the form its own Java defines.
   If reading A is correct and the spin is this poll, this is
   plausibly the Bug 32 fix itself. If reading B is correct, it
   is log hygiene only — the discriminator above tells us which
   before anyone claims a menu.

## What this pass does NOT claim

- That TITLE is or isn't drawing. Only Lola's screen can say what
  is visible, and she has said it: loading screen, finished bar,
  no main menu.
- That the HTTP poll is the cause rather than a fellow traveller.
  It is the only new, high-frequency, code-verifiable anomaly in
  the post-TITLE segment, and the only one with a specified fix —
  that earns it first place in the queue, not a verdict.


---

# Section 2 — The draw/render side (companion pass, 2026-10-13 evening)

**Scope:** what the parallel analysis above deliberately left open — what
the loading screen mechanically is, how TITLE's draw is dispatched and
where it renders, the spritecache question, and the blink mechanism.
Method: disassembly of `libengine.so` (ELF VA == file offset,
text_base 0x99000000) with calibrated PIC xref scans, the loader source,
and bootlog-9. This section **resolves the A-vs-B ambiguity** left open
above, using the return-value evidence.

## 1. The loading screen is the engine's Bumper — a static splash texture

- The picture Lola describes (Pocket Tanks logo + "Loading…") is
  **`bumper_512.png`**, a 512×512 engine splash asset with the logo and
  the word "Loading..." baked into the image (verified by direct
  inspection of the file in the extracted assets). A sibling
  `bumper_first_512.png` (same art + "First time loading - need a few
  extra seconds") exists but is never opened in this run — not a first
  run. Neither PNG contains a bar; the progress bar is a separate
  element of the loading display drawn with the bumper (its exact
  draw function was not isolated — see §6 gaps).
- The engine loads it itself, once, very early:
  bootlog-9 lines 1020–1021, `asset_open("bumper_512.png")` → success,
  during onCreate-era init, before "Initializing Game".
- The bumper has dedicated engine code: `redrawBumper` (fn 0x2232f0)
  and `reformatBumper` (fn 0x22325c), both operating on one global
  texture object (@0x8d8208) whose GL handle comes from fn 0x2ce7b2
  (negative = invalid; the error strings "attempting to redraw /
  reformat Bumper but texture handle is %d" sit in the same module).
  `reformatBumper`'s sole direct caller is the **scene-load entry**
  fn 0x16f9b0 (§3), i.e. the bumper is refreshed as part of every
  scene-load request.
- It is **not** INTRO2's content (INTRO2's nodes are earth/letters/
  creators texts/fader — none of which Lola has ever seen on screen in
  any build), **not** an engine overlay composited over a drawn scene
  in any way that matters (the bumper's black background is opaque —
  if it is drawn, it fully occludes), and **not** drawn by the loader
  (the loader ships no such art; its only present is the swap).

## 2. The present path — and why reading A is proven for this run

The engine presents **itself**, inside onDrawFrame:

- fn **0x3d6628** is the engine's present wrapper: it presets a global
  status word (@0x7078fc) to 0x3000, calls `eglSwapBuffers(display,
  surface)` — the display/surface being the loader's own, re-fetched
  every frame via `eglGetDisplay` / `eglGetCurrentSurface(EGL_DRAW)`
  at onDrawFrame entry (stored at G+0x1c / G+0x20) — and on failure
  stores `eglGetError()`'s result into the same word. Its failure
  branches compare against 0x3001 (EGL_NOT_INITIALIZED) and 0x300d
  (EGL_BAD_SURFACE), and the frame tail logs **"EGL swap failed"**
  (string @0x6581ee) when the word is not 0x3000 after the pump.
- **12288 is 0x3000 = EGL_SUCCESS.** (This replaces the "game's own
  native status word" description in bug31-analysis.md §3 — that was
  CPJNILib.java's declared type, not the value's meaning.) The JNI
  onDrawFrame (export at 0x3d39b4) stores 0x3000 into the word
  unconditionally at entry, and its single exit returns the word
  verbatim.
- A full-binary xref scan finds **exactly two writers/readers of the
  word**: the entry store in onDrawFrame and fn 0x3d6628. No code path
  writes 0 to it; the only values it can hold at return are 0x3000 or
  an EGL error code (0x3000–0x300e) — **all nonzero**.
- main.c logs every nonzero return. Therefore *any* onDrawFrame call
  that returns at all is logged. bootlog-9 logs exactly 13 returns
  (frames 0–12) and the logger demonstrably stays alive for ~2,800
  further lines. **Conclusion: frame 13 — the call inside which TITLE
  was requested — never returned** (within the captured window).
  Reading (A) holds for this run; reading (B)'s "frames return 0" is
  not merely unsupported, it is unreachable in this binary. (This also
  means build H's discriminator items 1–2 are partly pre-answered:
  returns cannot go invisible, and swaps stop because both swappers —
  the engine's present and main.c's `graphics_swap()` — sit downstream
  of the blocked call. A frame counter/heartbeat is still the cheapest
  positive confirmation.)

## 3. Draw dispatch: one virtual path for every scene; the bumper owns it while a load pends

Per frame (onDrawFrame, state field == 4): the object-update pump
(fn 0x3d3aa0 — the dispatch through which INTRO2's tick provably runs)
runs first; then the frame tail (fn region 0x170620) runs. The tail:

- checks for a pending scene request (fn 0x3d6e30) and, when one is
  reported, invokes the **scene-load entry fn 0x16f9b0** — which
  stores the requested name in a global, calls `reformatBumper`, and
  posts load events (fn 0x1f60a8 signal posts via fn 0x2236a0);
- dispatches the **current scene through a vtable forwarder**:
  fn 0x2a8e6a loads the current-scene pointer and tail-calls vtable
  slot +0x18 on it (a family of identical forwarders at
  0x2a8e6a–0x2a8ec2 covers slots +0x18…+0x34 — the manager's generic
  "call virtual X on current scene" trampolines; the switch fn
  0x2a8c3c updates the same current-scene pointer and does nothing
  else);
- then calls a pre-present fn (0x3d6608) and the present wrapper
  (0x3d6628).

So TITLE's draw — when reached — is the same virtual dispatch as
INTRO2's, through the same manager, with **no TITLE-specific path**.
What reaches the panel during the frame era is nevertheless the
loading display in every build so far: INTRO2's tick runs and its
step actions fire (Bug 31 addendum), yet its earth/letters art has
never appeared on Lola's screen — only the bumper + bar. The scene
manager's load/transition machinery (the same tail that invokes the
load entry) keeps the bumper as the presented content while a scene
load is pending, and in this port that state never clears.

## 4. Render targets: FBOs are one generic engine helper, not a TITLE divergence

libengine imports the OES framebuffer family; the loader's
`gl_wrappers.c` forwards them to vitaGL core entry points. A BL/BLX
caller scan finds the FBO calls in **exactly one place**: a generic
engine render-target function cluster at 0x3dc044
(`glGenFramebuffersOES`), 0x3dc7ac (`glBindFramebufferOES`),
0x3dc7d4 (`glCheckFramebufferStatusOES`) — shared texture/render-
target plumbing in the engine core module, not scene code. TITLE and
INTRO2 are the same breed of Zoom ZML scene through the same manager
and renderer; nothing gives TITLE a different render target, and
there is no evidence of a "TITLE renders into an FBO that never
composites" mechanism. TITLE's invisibility is a state problem
(§2 + §3), not a render-target problem.

## 5. Spritecache: never built in this port — a real defect, but not this bug

Complete catalogue for bootlog-9: **12 spritecache lines, all during
init, none after line 3,699** —

- `sprite_1024_960x544.spritecache` (lines 1194–1206),
  `hdsprite_1024_960x544.spritecache` (1211–1223),
  `iconssprite_1024_960x544.spritecache` (3687–3699). Each follows
  the same script: `fopen(path, "rb")` → ENOENT (the game log deletes
  the caches at every init); `asset_open` fallback → NULL; then the
  engine attempts the rebuild by opening
  `...spritecache.tmp` through `asset_open` (caller 0x3d8c47) → NULL.
- Why the rebuild can never succeed, loader side (`asset_mgr.c`):
  `AAssetManager_open` ignores its `mode` argument entirely
  (`(void)mode`), tries the assets root with `"rb"`, then the data
  root with `"r+b"` — and **`"r+b"` cannot create a missing file**.
  A brand-new cache file therefore can never be created through this
  shim. (Contrast: plain `fopen(..., "wb")` in the same cache
  directory works — the engine's art-pack stamp files
  `*.bbk_960x544_id` are written successfully at bootlog-9 lines
  1048–1186, immediately before the cache probes.) The engine
  proceeds without the caches and never retries within a run.
- Relevance to Bug 32: **none for TITLE.** The caches are sprite/icon
  atlases for in-game rendering; TITLE's art is per-file PNGs under
  `zoom/title/` (deluxe/gear/bulletin/playonline/games/share buttons),
  all preloaded successfully during init (bootlog-9 lines
  5965–7473 region), and TITLE's only post-switch file needs were the
  scroller font and the bulletin/moregames data (parallel analysis).
  Flag the cache-build failure as a **known defect waiting at the
  GAME scene**, with an obvious fix shape ( honour the open mode /
  allow creation for data-root asset opens ) — to be scheduled with
  gameplay work, not with Bug 32.

## 6. The blinks, mechanically

- Frames 0–12 are 13 presented frames spanning ≈35 s of wall time
  (game log: init complete 18:13:47 → TITLE switch 18:14:22), i.e.
  ≈2.7 s per frame at the log-throttled cadence. Each frame presents
  the loading display; the bar visibly drains during these frames as
  the (now clock-driven) load/job queue runs — the same delta-time
  advance that started the HTTP poll sets in build G. Thirteen
  re-presents of a mostly-static image, seconds apart, is precisely
  Lola's "blinks a few times every couple of seconds."
- The TITLE request fires **inside frame 13** (its SCENE line sits
  between frame 12's return marker at bootlog line 11733 and the
  post-switch segment; Lola's two taps at lines 11736/11745 land in
  frame 13's input_poll). Frame 13 never returns (§2), so neither
  the engine's present nor main.c's `graphics_swap()` is ever
  reached again. The panel holds frame 12's presented content —
  the bumper with the finished bar — indefinitely. That is the
  settled screen she reports, word for word.
- Not the mechanism: "presents continuing with byte-identical
  content." Presents **stopped**; the stillness is a frozen
  last-presented buffer, and the proof is the return-log silence
  (§2), not inference from the screen.
- Note the double present in healthy frames (the engine swaps
  inside onDrawFrame; main.c swaps again immediately after) — benign
  here, recorded so a future cleanup doesn't mistake it for a cause.

## Residual gaps (stated, not papered over)

- `redrawBumper` (0x2232f0) has no direct, branch, add-pc, or stored-
  pointer caller anywhere in the binary; it is presumably reached
  through the load module's internal dispatch or a computed target.
  Its existence, operand (the bumper texture object), and sibling
  `reformatBumper`'s caller are established; its call site is not.
- The exact function that draws the progress bar over the bumper was
  not isolated; the bar's existence and behaviour rest on Lola's
  reports plus the queue-drain correlation in this run.
- Vtable slot +0x18's semantic name (draw vs. enter vs. tick) is not
  proven; §3's dispatch claim is structural (a current-scene virtual
  is called from the present path), not nominal.

**Net draw-side verdict:** the panel is doing exactly what the engine
tells it. The presented content never stops being the Bumper loading
display because the scene manager's load state never clears, and
after frame 12 it is not even re-presented — frame 13 blocks inside
the TITLE-era work catalogued in the parallel analysis. Fix the
block (the CPJNIHTTP completion path specified above) and the same
present path that has worked all along has something new to show;
what Lola then sees remains, as ever, hers to report.

---

# Section 3 — inside the stuck frame (build H, bootlog-10)

Third pass: fingerprint the stuck frame from bootlog-10, identify the
TLS keys in its residue, and dissect the frame's tail path statically.
No build in this pass.

## 3.1 Runtime fingerprint of the stuck frame (bootlog-10)

Post-TITLE segment = lines 10671–16862 (6,192 lines), beginning with
`SCENE: switch requested -> "TITLE" swaps=11`. Composition:

- **`API: pthread_getspecific(key=8) -> 0x81405220` dominates:
  5,849 of 6,192 lines.** From ~line 11000 the segment is 967 key-8
  lines per 1,000 — a nearly pure getspecific(8) stream. The value
  never changes. Between unlock events: ~330 key-8 polls per ~6.5 s
  interval ≈ 50/s *as logged* — but blog() does two unbuffered
  sceIoWrite syscalls per line (bootlog.c), so the loop is
  logging-throttled; the true iteration rate is unknown and higher.
- Everything else in the segment is event-driven and bounded:
  53 mutex lock + 53 unlock pairs (clustered at events), 17 pk.cfg
  save pairs (`fopen "wb"` + `"rb"` — the unlock saves), 20 game-log
  fprintfs, 2 THROW pairs (both `ptree_bad_path`: "No such node
  (moreGames.viewedCount)", "No such node (bulletin.viewedId)"),
  one-time loads of `bulletin/badge.xml` (×2), `moregames/catalog.xml`,
  `zoom/scroller.ttf`.
- **Zero logged sleeps/waits post-TITLE** — with a caveat:
  usleep/nanosleep/sleep/sem_wait are bound in dynlib.c directly to
  newlib (unlogged), and `bionic_pthread_cond_wait` does not blog.
  A sleeping loop would be invisible in this trace; a cond_wait cycle
  would still show its mutex unlock/lock pair per wait, and only 53
  pairs exist — cond_wait is not cycling.
- `pthread_create` count in the **entire** bootlog-10 = **0**. The
  game is single-threaded in this port. Therefore the stuck frame's
  own loop is what services everything that keeps advancing: the
  18 pack unlocks at ~6.5 s cadence (game log 18:45:04 → 18:46:56,
  still running at log end). Per unlock cycle the trace shows:
  getspecific(key=14) ×2 around the pk.cfg write/read pair,
  getspecific(key=12), then the game-log mutex (0x999048e0) + fprintf.
- The fine structure of the residue (from line ~10739, ~68 lines
  after the switch, once TITLE setup begins): repeating bursts of
  {engine mutex 0x998f4e58 lock/unlock ×several, getspecific(8) ×4–8},
  i.e. a **poll loop whose body takes a lock, checks state through a
  TLS-reading helper several times, and retries** — not a hard spin
  (a hard spin would not interleave mutex work and timed services),
  and not a blocking wait (nothing blocks: timed work keeps firing).

Pre-TITLE distribution of key 8 for contrast: 1,188 reads across the
whole init/load era (asset and scene loading), only **12 reads across
all ~12 healthy INTRO2→TITLE frames (~1/frame)**, 0 during the
weapon-load window (lines 3400–3900). Key 8 is not a per-frame cost;
it is read by load-phase work — and by whatever the stuck frame is
doing ~50+ times per logged second.

## 3.2 The pthread keys

Creation map (bootlog-10): key 8 at line 191 (libc++_shared init);
keys 9, 10, 11 at lines 220/225/231 (libengine init_array); keys
12–16 at lines 500–504 (**inside libengine's JNI_OnLoad**); key 17 at
line 769; key 13 first set at line 923; key 15 at line 1038; key 18 at
line 4004.

- **Key 0** — loader-reserved (bionic_pthread.c reserves keys 0–7:
  `B_PTHREAD_KEYS_RESERVED 8`) because libengine uses key 0 directly
  without key_create: two thread-local singletons share it (a
  calloc(2049) thread struct and the JNIEnv* cell the shim seeds).
  No getspecific(key=0) lines exist anywhere in bootlog-10 — the
  engine's key-0 getters do not route through the logged shim path.
- **Key 8 — libc++abi exception-runtime TLS (high confidence it is
  the EH complex; the exact sub-key split vs key 18 is not fully
  resolvable statically).** Evidence: created during libc++_shared's
  static init, bracketed by mutexes at 0x98089650 / 0x98089678 — the
  libc++abi globals block that also holds the exception-globals key
  global (VA 0x89670) and its once-control (0x89674). Immediately
  get-or-created (getspecific → 0x0, setspecific 0x81405220, then
  repeated getspecific) — the `__cxa_get_globals` pattern. It is read
  in bursts around every throw (per-throw ±12-line windows show
  key 8 and key 18 side by side). **Ruled out:** (a) libc++'s
  `std::__thread_specific_ptr<__thread_struct>` key — its getter
  (`__thread_local_data()`, libc++_shared 0x63a14) has exactly three
  users in the whole .so (`std::notify_all_at_thread_exit`,
  `__assoc_sub_state::set_value_at_thread_exit`,
  `set_exception_at_thread_exit` — all one-shot registrations,
  proven by PLT + ARM-veneer call-site census); (b) FMOD — libfmod
  imports no pthread_getspecific at all; (c) libengine code — its
  only three direct getspecific sites are two get-or-create
  singleton getters reading key *holders* (keys 9–16) and
  `boost::detail::get_current_thread_data` (0x4a904c, Boost.Thread's
  own key). A full veneer census of libc++_shared shows
  pthread_getspecific is called from exactly two functions:
  `__cxa_get_globals_fast` (via ARM veneer 0x709b0) and the inlined
  getter inside `notify_all_at_thread_exit`. So the flood's caller
  reaches key 8 **through the libc++ exception-globals path**:
  the stuck loop's body passes through EH-runtime state several
  times per iteration (consistent with repeated try/catch or
  exception-state checks whose throws, if any, originate inside
  libc++ and are therefore invisible to the loader's engine-only
  `__cxa_throw` hook — only 2 hooked THROWs appear post-TITLE).
- **Key 18 — the second EH key**, created at line 4004, two lines
  after the process's first THROW, followed immediately by
  setspecific(key=18, fresh heap ptr) — the lazy half of the EH
  globals pair (cf. the Bug 29/30 finding of key-18 reads inside
  `__cxa_free_exception`).
- **Key 12 — an engine subsystem singleton** (one of five TLS keys
  created in libengine's JNI_OnLoad). Get-or-created at onCreate
  (value 0x81540568; the pointer is later swapped on the main thread —
  a "current context" cell). Read at every unlock event and
  throughout weapon/title loading (169 reads in the weapon-load
  window alone): the store/session singleton the unlock manager
  consults. Key 13 (0x815583f8) is a sibling singleton read just as
  hotly during loading; **key 14 (0x8153fd48) is the config/settings
  singleton** — read ×2 around every pk.cfg save. Key 17 is a
  scratch/current-op cell (values are per-call pointers, sometimes
  0x0). Keys 9/10/11 are libengine init-era singletons (the
  Boost.Asio/Thread generation).

## 3.3 Static dissection of the frame path — everything is bounded except one call

`onDrawFrame` (0x3d39b4, fully disassembled): presets the status
word to 0x3000; in state 4 it calls exactly two functions —
**pump 0x3d3aa0**, then **tail 0x170620** — then conditionally swaps
and returns the status word. (Section 2's present wrapper 0x3d6628
is called from inside the tail; onDrawFrame's own trailing swap is
the second, healthy-frame present.)

- **Pump 0x3d3aa0** (fully disassembled): under one global mutex it
  (1) walks a linked list of named objects, invoking a single global
  handler on entries whose flag byte is set and running their
  per-node callbacks, and (2) walks a refcounted deferred-destruction
  list, destroying entries whose count reaches 0 (callees 0x3d4c8c,
  0x3d5b30). Both loops terminate when their lists empty. No waits,
  no sleeps, no JNI.
- **Tail 0x170620** (fully disassembled): (1) the sound-flag
  synchronizer — the four getters (0x3d9eb4, getMusicFlag glue
  0x3da03c, 0x3d61b6, 0x3d61be) read twice each, changes pushed into
  a ptree via 0x28e764; (2) two small container-iteration loops
  (0x1706fe, 0x170718–0x170744); (3) **scene dispatch #1** via
  vtable forwarder 0x2a8e6a (slot +0x18) — the current scene's
  per-frame call; (4) pending-scene check 0x3d6e30 → if a request is
  pending, **scene-load entry 0x16f9b0**, then **dispatch #2** —
  the new scene's first +0x18 call; (5) pre-present 0x3d6608 →
  **present 0x3d6628** (eglSwapBuffers + eglGetError, no loops);
  (6) two helper calls (0x3d60f0/0x3d60d8 pair, 0x170870, 0x3d3bc4).
  No wait/retry loop exists anywhere in the tail itself; there is
  no TITLE-specific path and no render-target divergence.
- **Scene-load entry 0x16f9b0** (disassembled): calls reformatBumper
  (0x22325c), 0x2236a0, load-event posts (0x1f60a8 ×3), and ~25
  scene-setup calls. Its only loopbacks are two small iterations
  (0x16fa62, 0x16fd1e). Bounded; runs once per request.
- **Present wrapper 0x3d6628**: no loops. Not the spin.

Elimination: the TITLE switch was requested mid-frame (SCENE line at
swaps=11), the frame's present (swap 12) never happened, and the
present sits in the tail *after* dispatch #2. Dispatch #1 in this
frame is INTRO2's update — it had already completed 12 times. The
scene-load entry is bounded. **The never-returning call is
dispatch #2: the TITLE scene object's first vtable +0x18 call
(its enter/first-update), reached via forwarder 0x2a8e6a from tail
site 0x1707f0.** Everything the log shows after the switch is that
call's own work: `zoom/scroller.ttf` load, `moregames/catalog.xml`
and `bulletin/badge.xml` reads, the two ptree_bad_path throws
(moreGames.viewedCount, bulletin.viewedId), `CPJNIParse.
badgeGetNumber` stub → 0 — then the poll-loop residue begins.

## 3.4 What it is waiting on — leading hypothesis, and what would prove it

Not proven statically: TITLE's scene code is reached only through a
vtable, and its +0x18 body has not been isolated. The evidence
points at a **data-ready poll on the HTTP-backed bulletin/moreGames
state**:

- The two config nodes TITLE's setup throws on —
  `moreGames.viewedCount` and `bulletin.viewedId` — belong to the
  two payloads this engine fetches over HTTP (the more-games catalog
  and the bulletin/badge feed) through CPJNIHTTP. On stock Android
  the fetch resolves (success *or* failure) in Java and the native
  side proceeds. In build H our bridge answers: initGet stub →
  invalid handle, getTransferStatus → 0 (ERROR), getHTTPStatusCode
  → 0, getResponseHeaders → empty array, getData → NULL.
- Build G→H evidence fits: replacing NOT FOUNDs with those terminal
  answers killed the lookup flood and sped everything up (TITLE
  switch 36 s → ~15 s, unlock cadence 20 s → 6.5 s) — the poll is
  still running, just cheaper per iteration. If TITLE's first update
  loops until a "bulletin/catalog data settled" flag is set, and
  that flag is only set by a completion path our answers never
  trigger (e.g. it waits for a transfer object to reach SUCCESS, or
  for a Java-side callback that posts into the engine), the loop
  never exits — while each iteration pumps the engine's services,
  which is exactly how the store manager's ~6.5 s unlock cycle
  (key-12/key-14 singletons) keeps firing from inside the stuck
  frame.
- The per-iteration key-8 bursts fit a loop body that repeatedly
  re-reads/re-parses config through the C++ EH-adjacent path
  (ptree/locale machinery), not a condition-variable wait: there
  are no cond_wait cycles and no logged sleeps in the residue.
- Weaker alternatives not excluded: a poll on an audio/FMOD state
  (against it: the tail's sound sync reads only engine memory, and
  FMOD never touches pthread TLS), or a poll on a Java-callback flag
  in the key-0 JNI cell (no logged traffic either way).

## 3.5 Precise hook points for a frame-tail trace build

Gate everything below to **frames ≥ 12** (the TITLE-switch frame;
the frame counter already exists) and cap each hook's total lines
(~200) so the log stays readable:

1. **getspecific caller capture (the decisive one):** in the
  loader's bionic_pthread.c getspecific shim, when key == 8 and a
  loader global `trace_armed` is set (set it when the SCENE hook
  fires for "TITLE"), log the caller's return address (lr) —
  first 200 occurrences with counts per unique caller. One run
  names the flood's caller directly.
2. **Scene-dispatch bracket:** hook forwarder 0x2a8e6a (Thumb
  0x2a8e6a) — log scene object ptr, its vtable ptr, and the resolved
  +0x18 target on entry, and a matching exit line. The entry with
  no exit is the stuck call, and the target address is TITLE's
  +0x18 body for static follow-up.
3. **Tail brackets:** enter/exit logging on 0x3d6e30, 0x16f9b0,
   0x3d6608, 0x3d6628, and pump 0x3d3aa0 — confirms the bracket in
   one run even if (2) is inconclusive.
4. **CPJNIHTTP glue traffic:** in jni_bridge.c, log every
   CPJNIHTTP call (initGet/initPost/getData/getBytesDownloaded/
   getTransferStatus/getHTTPStatusCode/getResponseHeaders/close)
   with args, return value, and caller return address, frame-gated.
   If the stuck loop polls the transfer state, its exact expected
   value shows up as the call cadence + the branch taken on our
   answer; if the glue is *never* called inside the stuck frame,
   the HTTP gate hypothesis is dead and (1)+(2) carry the search.
5. If (2) identifies TITLE's +0x18 target: second build hooks that
   function's first-level callees (enter/exit, same gating) to find
   the loop head and the exact global/flag it tests.

**Bottom line:** the stuck frame is TITLE's first scene update,
spinning in a services-pumping poll loop (residue: libc++ EH-TLS
reads, key 8, in lock-step bursts) whose exit condition our layers
never satisfy — most likely the HTTP-backed bulletin/moreGames
data-ready state. Hook (1) names the spinning caller in a single
run; hooks (2)+(4) together confirm or kill the HTTP gate.

# Section 4 — the covered menu (build I, bootlog-11; pass 4, 2026-10-13 late)

Build I falsified §3's frame shape: the perma-stuck frame did not
recur. Heartbeats reach frame 420 (swaps 421, r=12288 throughout,
zero EGL swap failures in the whole log); DISPATCH brackets show
TITLE's update (target 0x183368, scene 0x997f7b68, vtable 0x996ed254)
entered/exited 99/99 before the 200-line cap (cap hit at line 16917,
≈frame 111; frames continue unlogged to 420). HTTP glue calls inside
TITLE frames: zero — the §3.4 HTTP-gate hypothesis is dead. SPIN's
only key-8 caller is 0x980703ec in libc++_shared, which this pass
places at the getspecific call site inside libc++abi's
__cxa_get_globals_fast (the function is local/stripped — it sits in a
dynsym gap between the std::bad_typeid exports @0x6d9b9–0x6d9d9 — so
the name rests on §3.2's two-caller veneer census, not a symbol).
Post-TITLE key-8 reads total 1,020 over ~420 frames (≈2.5/frame):
bounded EH-TLS background, not a flood. And Lola's eyes reframe the
bug: the main menu EXISTS and renders — it flashes in for a couple of
frames at a time under an almost-always-covering loading bumper.
Section 4 answers: what TITLE's update actually does (§4.1), how the
frame tail really sequences dispatch and present — which corrects
§3.3's linearized reading (§4.2), the bumper's composite rule (§4.3),
the observer-effect verdict (§4.4), and the minimal next build (§4.5).

## 4.1 TITLE's vtable +0x18 (file offset 0x183368) — a thin, bounded wrapper

Disassembly (this pass; file offsets, ELF VA == file offset):

```
0x183368  push {r0-r3, r4, r6, r7, lr}     ; r4 = scene
0x183376  bl   0x1833c8                    ; scene-local helper
0x18337c  bl   0x3d65e8                    ; core gate getter
0x183380  cbz  r0, 0x1833be                ; gate==0 -> pop, return
          ; full path:
          build zeroed 16-byte stack struct (double pattern 0x3ff00000 = 1.0)
0x1833a2  bl   0x3d6e50                    ; core call (see below)
0x1833a6  movs r0, #0; bl 0x3d6838         ; core call (0)
0x1833ae  bl   0x0de488
0x1833b2  bl   0x18351c / 0x1835e4 / 0x183648 / 0x183710   ; 4 sub-updates
0x1833be  pop; b.w 0x3d6608                ; tail-call pre-present forwarder
```

- The helper 0x1833c8 works the scene's scroller/layout: it brackets
  its body with the pair 0x2c8508/0x2c8520 on the object at
  scene+0x460. That pair (disassembled) is a conditional core-session
  begin/end, NOT a lock: begin stores a byte at obj+0x55=0 and asks
  the core, via forwarder 0x3d6dec, whether the sub-object handle at
  obj+0x10 is live (answer cached at obj+0x54); end, iff the cached
  byte is set, notifies the core via forwarder 0x3d6e10. Neither can
  block. Inside the bracket: Zoom-node calls on the node at
  scene+0x5c8 (0x2da79c, 0x2da85a(n,1), 0x2da8b0(n,2), 0x2da830(n,0),
  0x2da7dc, 0x2db88e, 0x2daf60, 0x2da8b8), FP math over doubles in
  the scene+0x40d8 block (+0x2f8/+0x328/+0x338), 0x2ddbd8 over
  scene+0x4340. Its return value is discarded by +0x18.
- The gate 0x3d65e8 is one of the uniform 0x3d6xxx core-glue
  forwarders: all of them PIC-load the engine globals struct (file
  offset 0x90475c), require [globals+0x28] == 3, load the core
  singleton pointer (cell @0x702bb0), and tail-jump into the core
  module. 0x3d65e8 lands on core fn 0x3dc1fe, which is three bytes of
  logic: `ldrb r0, [r0]; bx lr` — it returns core-object BYTE 0.
  So TITLE's (and INTRO2's) full update body runs only while
  engine-state == 3 AND core byte0 != 0. Note the corollary for
  reading build I's DISPATCH brackets: an enter/exit pair does NOT
  prove the full body ran — a gate-0 call returns right after the
  helper and still brackets cleanly. (That the full body runs at
  least sometimes is proven by Lola's eyes, not by the brackets:
  the menu she sees is rendered through this path.)
- The full path's core calls: 0x3d6e50 -> core 0x3dc95c, which is
  itself gated on the same byte0 and, when set, converts the double
  pair (0.0, 1.0) plus the stack struct to floats and calls PLT
  0x7f7d4 — the scene's render/viewport step. 0x3d6838(0) -> core
  0x3dc6c8: core+0x74 := -1, core+0x80 := arg. The final tail-call
  0x3d6608 -> core 0x3dc202: if byte0, core+0x74 := -1.
- The four sub-updates are the scene's timed-action machinery:
  sub1 (0x18351c) and sub3 (0x183648) walk the action-list pair at
  scene+0x154 (iteration cursor) / scene+0x158 (head). Each node
  carries a double time at +0x90 compared against the scene clock
  double at scene+0x458; sub1 processes nodes whose time is NOT
  after the clock, sub3 the complementary half. Processing resolves
  a target via 0x1f62d4(global, [node+0xa8]), reads params
  [node+0x98]/[node+0x9c], and applies through executor 0x2780a8.
  The walk advances via 0x183c88, a cursor-pop (head=[head]; at list
  end, cursor := the +0x158 head field and the walk stops). Sub2
  (0x1835e4) fires a single action from [scene+0x120] with times at
  +0x128/+0x130. Sub4 (0x183710) syncs a rect: getters
  (0x275e76/80/8a/94) on the object at scene+0x40e0 applied via
  0x2c7f02 to the node at scene+0x4f0.
- INTRO2's +0x18 (0x14611c) is the same skeleton with one
  sub-update: this is the generic Zoom-engine scene update, not
  TITLE-specific code.

**Verdict on Q1's second half:** nothing in +0x18's own body can
spin forever. Every loop is a cursor-advance list walk that
terminates at list end; there are no waits, no condition variables,
no JNI calls, and the bracketing pair cannot block. If the G/H frame
hung inside dispatch #2, the hang lives in a CALLEE or in the list
CONTENTS on that first call — ranked candidates: (1) the timed-
action walks, IF action processing (0x2780a8) re-enqueues an
already-due action into the same list during the walk (the cursor
is re-read each iteration, so a self-re-enqueueing repeating action
— e.g. a periodic store/bulletin check — would make one update call
process indefinitely while still, per iteration, doing real work:
that shape matches §3.1's service-pumping residue, including the
~6.5 s unlock cadence, better than any pure spin); (2) a deeper
callee (0x1f62d4 name lookup, or a core routine behind the
forwarders). Static analysis cannot separate (1) from (2), and
build I — identical code, identical assets, 99/99 clean returns —
proves the difference is runtime STATE, not the function. Any
write-up that names a specific in-body loop as the G/H culprit is
overclaiming; the honest statement is the ranked pair above.

## 4.2 The frame tail, corrected (refines §3.3's linear reading)

§3.3 read the tail (fn 0x170620) as a fixed sequence: dispatch#1,
load check, dispatch#2, present. Full disassembly this pass shows a
gated, modal structure instead. The sequence per onDrawFrame:

1. Clock gate fn 0x222df8 (a function of the 0x222xxx state module):
   if its enable byte (module+0x04) is 0, or the frame delta
   (now − module+0x50, now from the Bug-31 clock glue 0x3d8340,
   delta clamped to module+0x58) is under the rate threshold, it
   returns 0 and the tail aborts the frame entirely — no pump, no
   dispatch, no present.
2. Pump part 1: deferred-destruction iteration (0x224954/0x3d4c8c
   over a stack-local object) — §3.3's pump step (2).
3. Loop B: a stack-iterator drain (0x3da134 test / 0x224964 /
   0x2220a8 / 0x28d398), inside which scene slot +0x28 is dispatched
   through forwarder 0x2a8ea6 when module fn 0x223aa4 (byte getter,
   module+0x11) returns 0. (§3.3 folded this into "the pump".)
4. Sound-flag sync (§3.3 step 1): getters 0x3d9eb4 / 0x3da03c
   (getMusicFlag glue) / 0x3d61b6 / 0x3d61be / 0x3d65b8 compared
   against cached bytes at tail-state +0x08..+0x0B/+0x28; changes
   are pushed to the property tree (0x28e764/0x28e810).
5. Clock reads: two global doubles fed through 0x220278 and
   0x220a84 around a 0x223aa4-gated slot +0x28 dispatch (forwarder-
   adjacent fn 0x2a8e2c) — the scene-clock plumbing that also feeds
   §4.1's scene+0x458 clock.
6. THE DISPATCH BLOCK — entered only if ALL of:
   - cached display dims (tail-state +0x2C/+0x30) still equal the
     core's current dims (getters 0x3d60f0/0x3d60d8);
   - NOT (fn 0x2238b8() == 0 AND latch != 0), where fn 0x2238b8 is
     the byte getter for state-module +0x0C and the latch is
     tail-state byte +0x0C, rewritten at the end of every executed
     block (see below);
   - mode := fn 0x3d64f4() is 2 or 3. The mode query is a plain
     read: `ldr r0, [globals+0x28]; bx lr` — the mode IS the
     engine-state dword the forwarders also gate on.
   Mode 2 (steady state): dispatch#1 via forwarder 0x2a8e6a (tail
   site 0x1707cc); latch := fn 0x3d65a0(0), a constant-1 stub.
   NO engine present happens on a mode-2 frame.
   Mode 3 (switch/loading state): pending check 0x3d6e30; if a load
   pends, scene-load entry 0x16f9b0 runs; fn 0x223cf0 (a lazy
   elapsed-clock init — verified NOT a draw); dispatch#2 via the
   forwarder (tail site 0x1707f0); then, iff the core gate byte
   (0x3d65e8) is nonzero: 0x220b80, 0x0dcac8, pre-present 0x3d6608;
   present 0x3d6628; latch := present's boolean (1 iff EGL_SUCCESS).
   Any other mode: no dispatch, no engine present.
7. Dims are re-cached; if tail-state byte +0x03 is set, fn 0x170870
   and glue 0x3d3bc4 run (end-of-frame housekeeping).

Consequences that matter below: (a) the engine presents from the
tail ONLY on mode-3 frames; (b) §3.3's "dispatch#1 then dispatch#2"
never happens in one frame — they are the mode-2 and mode-3 arms;
the G/H stuck frame was necessarily a mode-3 frame (it ran the
load entry and dispatch#2), and build I's TITLE dispatches come
from one or both arms; (c) there are exactly three presenters of
the panel: the tail's mode-3 present, the bumper draw's own
present (§4.3), and the loader's graphics_swap() after every
onDrawFrame return, which re-presents whatever the back buffer
currently holds. Last writer wins.

## 4.3 The bumper's composite rule (Q2)

The bumper draw is fn 0x223120 ("drawBumper(progress)"), in the
0x222xxx–0x224xxx state/loading module, PIC anchor file offset
0x8d81d0 (bumper texture object at anchor+0x38 = 0x8d8208, the §2
address; progress cell at anchor+0x18 = 0x8d81e8):

- 0x223120 stores its float argument into the progress cell
  [0x8d81e8]; if the core gate byte (0x3d65e8) is set, it draws:
  0x3d6e50 core step, centering arithmetic from the dim getters
  (0x3d60f0/0x3d60d8 halved), node call 0x2ce600(bumperObj, …) with
  bar geometry computed from the stored progress (a table lookup +
  float divide/multiply — the loading bar's fill), 0x3d6f48; then
  pre-present 0x3d6608; and it PRESENTS BY ITSELF — epilogue tail
  `b.w 0x3d6628`. It is a self-contained "paint the loading screen
  now" routine, callable at any time, not a per-frame compositor.
- redrawBumper 0x2232f0: handle sanity check (0x2ce7b2 on the
  bumper object; invalid -> error log via 0x3d3480), then tail-call
  0x223120 with the STORED progress value. reformatBumper 0x22325c
  (recreate the bumper node for a new size) has exactly one caller:
  scene-load entry 0x16f9b0 at 0x16f9d2, which also posts the load
  events (fn 0x2236a0 at 0x16f9e0, and direct 0x1f60a8 broadcasts —
  0x1f60a8 is a bounded scene-graph event broadcast over a node
  tree, not a queue).

WHO CALLS THE REDRAW — exhaustive negative result (calibrated
skipdata sweeps, this pass; the same sweep recovers every known
caller elsewhere — reformatBumper <- 0x16f9d2, poster <- 0x16f9e0,
forwarder <- {0x1707cc, 0x1707f0}, scene-load <- 0x1707e6): BL/BLX
callers of redrawBumper and 0x223120: NONE. Branch (b.w) callers:
NONE (0x223120's only branch-in is redrawBumper's own tail at
0x223310). Absolute-pointer dwords for either function anywhere in
libengine.so or libc++_shared.so: NONE. Conclusion: the redraw is
reached ONLY through a runtime-computed indirect call (PIC base +
offset, or a callback pointer stored in a heap object at runtime).
Static xref cannot name the invoker; §4.5's entry hook will.

What the structure does establish: the bumper is the LOAD's
progress surface. The scene-load entry reformats it and posts load
events at every switch; progress updates are reported back through
the callback into 0x223120, which repaints (logo + "Loading..." +
bar at the stored progress) and presents immediately. On stock,
when the load's work drains, the switch completes (mode 3 -> 2),
progress callbacks stop, and the scene's rendering owns the panel;
nothing ever redraws the bumper again until the next load.

**Ranked mechanism for the cover (build I):**

1. (PROVEN components, INFERRED linkage — ranked first because
   every proven piece points here) TITLE's load never completes in
   this port, so its progress machinery never stands down. The
   load's payload includes the HTTP-backed bulletin/moreGames
   data: §3.1 watched the first TITLE frame load
   moregames/catalog.xml and bulletin/badge.xml and take two
   ptree_bad_path throws (moreGames.viewedCount, bulletin.viewedId);
   bootlog-11 still shows badge.xml re-opened x4 and the badge
   path polled (CPJNIParse.badgeGetNumber stub answers 0). On
   stock Android those payloads settle via the Java async fetch;
   here there is no fetcher — build H's terminal ERROR/empty
   answers and build I's silence (the glue is not even called in
   the frame era) both leave the load's completion condition
   unsatisfied. A load that never completes keeps producing
   progress repaints: each callback invocation repaints the bumper
   AND presents it, over whatever the scene presented last.
2. (PROVEN) Last-presenter-wins explains the flicker arithmetic.
   The scene presents at most once per mode-3 frame; the bumper
   presents on every progress repaint. The panel therefore shows
   the loading screen whenever the most recent present was a
   bumper repaint, and shows the menu exactly for the stretches
   where a scene present (plus the loader's per-frame re-present
   of the same buffer) is the latest write — Lola's "only shows
   for a couple frames then back to the loading screen," and her
   video (bumper up almost always, dots animating — the repaints
   are also what animates it). The same arithmetic explains build
   G/H's different look: once frame 13 stuck, no further scene
   presents occurred, so the last bumper repaint froze on the
   panel ("blinks" = the 13 presents that did land).
3. (Residual alternative, cannot be excluded statically) The
   indirect invoker is not the load-progress callback but a
   periodic repaint job (e.g. a spinner timer in the same module)
   whose enable condition is the same never-cleared loading
   state. §4.5's hook distinguishes 1 from 3 by caller LR and
   cadence in a single run; the FIX is the same family either way
   (retire the loading state / its repaint source once TITLE is
   up), so the ranking does not block the next build.

What clears the condition on stock, restated plainly: the load
FINISHES. Every element of the cover — mode pinned at 3, progress
repaints, the bumper's self-presents — is downstream of a scene
load whose completion signal this port never delivers.

## 4.4 The observer-effect question (Q3): H -> I deltas, judged

Build I differs from build H in exactly four functional ways:

(a) DISPATCH INTERPOSITION. A trampoline at the forwarder site
    replaces forwarder 0x2a8e6a's executed code on EVERY dispatch:
    the C helper dispatch_log_enter performs the forwarder's own
    contract (deref the cell, NULL-check, load vtable+0x18, return
    the target) and the exit stub preserves the target's return
    value while logging. Until the 200-line cap it also spends up
    to two blog() calls per dispatch. The cap was hit at ≈frame
    111; frames stayed healthy, unlogged, to 420.
(b) SPIN BOOKKEEPING in the pthread_getspecific shim: a caller-LR
    dedupe table with first-sight + power-of-two milestone lines.
    The baseline per-call API logging is unchanged (bootlog-11 logs
    every getspecific, exactly as bootlog-10 did); SPIN adds table
    work and rare lines inside a shim the engine calls constantly.
(c) HTTP TRACE WRAPPERS: logging around the CPJNIHTTP handlers,
    whose logic is unchanged. Build I logged ZERO HTTP lines —
    the glue is not called inside TITLE frames at all.
(d) ARMING GLOBAL: one byte store (g_title_trace_armed) in
    scene_trace_log at the TITLE switch. Inert.

Judgment: only (a) changes code that executes inside the stuck
call path — (c) is exonerated by the zero-call census, (d) is a
single store, and (b) touches no engine code (though its per-call
overhead inside the flood means it cannot be declared innocent of
TIMING effects, it changes no semantics). If any I-delta unstuck
the G/H frame, it is (a), via one or both of: (i) added latency
inside exactly the switch frames (two logging syscalls per
dispatch plus a C round-trip) interacting with an engine whose
frame gate, scene clocks, and timed actions all read the Bug-31
clock — milliseconds can move a timed action across a due
boundary and change what the first TITLE update's walk enqueues;
(ii) the helper re-implementing the dereference chain in C,
masking any subtle misbehavior of the stock six-instruction
forwarder under the loader's memory model (the stock forwarder,
disassembled this pass at 0x2a8e6a, is exactly as simple as the
helper assumes: ldr r0,[r0]; cbz -> bx lr; ldr r1,[r0];
ldr r1,[r1,#0x18]; bx r1 — so (ii) is a formality, listed for
completeness).

The honest competing explanation must stay on the table: NOTHING
in build I unstuck anything, because the runs faced different
persisted state. Build H's run completed and persisted all 18 pack
unlocks (17 pk.cfg save pairs inside the stuck frame, §3.1);
build I started from that saved state — post-TITLE key-14
(settings singleton) reads drop from ~34 (H) to 6 (I), pk.cfg
opens from ~17 to 6, and the unlock sequence never re-ran. If the
G/H block was first-run store/bulletin work queued behind TITLE's
first update (§4.1 candidate 1), build I simply never faced it.
This explanation is at least as plausible as (a)(i), and the two
are not separable from the existing logs.

**PRESERVE LIST — binding on the cleanup build:** the 0x2a8e6a
dispatch interposition (trampoline + dispatch_log_enter/exit-stub,
with its logging free to be compiled out but its CODE PATH
intact) MUST be carried into any diagnostics-stripped build until
an A/B build — identical to build I in every other respect, with
only the stock forwarder restored — either reproduces the G/H
hang or clears the interposition. It must not be stripped as
"just a tracer": it is the only H->I delta inside the stuck path.
SPIN bookkeeping and the HTTP wrappers may be removed freely on
their own merits, but NOT in the same build as the A/B or as each
other's removal if the result is to stay interpretable — one
variable per build. The persisted-state confound has a testable
consequence worth one line in the A/B run: delete pk.cfg before
the A/B run; if the hang returns only with a fresh pk.cfg, the
interposition is innocent and the store state was the variable.

## 4.5 Minimal next build — Build J (tracer, one variable)

Base: build I verbatim (dispatch interposition INCLUDED, per
§4.4). Add only:

1. Entry hooks on drawBumper fn 0x223120 and redrawBumper
   0x2232f0, same trampoline pattern as the dispatch hook: log
   caller LR, the progress argument (r0 float bits at 0x223120;
   the stored cell [0x8d81e8] at redraw), and the heartbeat
   frame/swaps counters; first 100 invocations, then
   power-of-two milestones. One run names the bumper's invoker —
   the single fact static analysis proved unreachable (§4.3).
2. Mode-change logging at the tail's dispatch block: log
   [globals+0x28], the fn 0x2238b8 byte, and the latch byte
   whenever the mode value changes (cheap; settles which arm —
   mode 2 or 3 — runs TITLE's frames in build J, and whether the
   mode ever leaves 3).
3. Nothing else. No fix attempt in J.

Ask Lola for the usual screen report (does the menu hold, flash,
or stay covered?) — her eyes remain the arbiters of the picture;
the log arbitrates the mechanism.

Decision tree after J: if the hook's caller LR is the load/job
machinery (expected under §4.3 mechanism 1), Build K is the FIX:
make TITLE's load complete — deliver the completion the engine
is waiting on for the bulletin/moreGames payloads (the precise
patch — a settled CPJNIParse/CPJNIHTTP failure-completion, or
satisfying the two views' data wait once their XML has parsed —
is shaped by J's caller evidence, not before). If the caller is
instead a periodic repaint job (mechanism 3), K retires that
job's enable once TITLE is active. Either way K keeps the §4.4
preserve list intact; the A/B for the interposition is a separate,
later build, run with a deleted pk.cfg per §4.4.

---

# Section 5 — what TITLE-era frames contain (pass 5, 2026-10-13, build J log)

Question: the bumper hooks of build J fired exactly 17 times, all
at frame=0 during the initial load, and never again in 30,091
lines — yet Lola's screen is the loading image almost always, with
the main menu flashing in for a couple of frames at a time. If
nothing draws the bumper, what is in the frames that reach the
panel? Verified from bootlog-13.txt firsthand this pass: BUMPER
lines = 17 (progress 10→100, callers 0x9916f027…0x9916f7cf, all
frame=0, swaps=0); TAIL mode-change lines = 0; DISPATCH enter =
100 (the logging cap); last heartbeat frame=360, r=12288,
swaps=361 (loader swaps only — the counter increments in main.c
after graphics_swap(), plus one eglSwapBuffers inside
graphics_init); "onDrawFrame r changed" appears exactly once (the
initial value). Method: full disassembly of the frame tail
0x170620–0x1708e0, onDrawFrame 0x3d39b4, drawBumper 0x223120,
reformatBumper 0x22325c, the bumper setup fn around 0x222f40, the
render fn 0x220b80 (call census over its full body), PIC-reference
scans for the bumper objects, a store-site census for the mode
cell, and the loader sources (main.c frame loop, egl_graphics.c).

## 5.1 The constant mode is 3 — and in mode 3 the tail renders and presents every frame

The mode dword lives at file offset **0x904782** ([globals+0x28],
globals base **0x90475a** — §4.1's 0x90475c was off by 2; the
getter 0x3d64f4's literal at 0x3d64fc is 0x52e262, and build J's
hook derives the same base live from the loaded image). The whole
globals struct is .bss (0x70da00 + 0x217f68 covers every
0x90xxxx address cited in this file): zero at process start,
written during init. Four independent proofs the value is **3**
for the entire TITLE era of build J:

1. Build J's tail_mode_check samples the cell pre-frame on every
   loader iteration AND inside dispatch_log_enter, logging any
   change; the first sample seeds silently. Zero TAIL lines over
   360+ frames means the value never differed from its frame-0
   value at any sample point — including mid-frame at dispatch.
2. drawBumper is itself mode-gated: its body (below, §5.2) runs
   only if gate forwarder 0x3d65e8 returns non-zero, i.e.
   mode==3 AND the core byte0 set. It DREW 17 times at frame=0 →
   the mode was already 3 during the initial load, before the
   frame loop started. The writer therefore runs in the init
   path, which is why no per-frame code contains the store.
3. Every core forwarder TITLE's update touches (clear color
   0x3d6e50, the bar-fill 0x3d6f48, the pre-present 0x3d6608,
   gate 0x3d65e8) hard-requires [globals+0x28]==3; with any
   other value TITLE's +0x18 would exit after its scroller
   helper and the tail's mode-3-only render/present block
   (below) could never run — yet presents demonstrably happen
   (r=12288 = the present wrapper's own status word, §1.2) and
   Lola sees menu frames at all.
4. The alternative constant, mode 2, is refuted by the flashes
   themselves: the mode-2 arm (below) contains no render call
   and no present — a constant mode 2 would show the frame-0
   image forever with zero menu frames, contradicting Lola.

The WRITER of the cell is the one fact this pass did not pin.
Negative results, all firsthand: the JNI glue cluster
0x3d6400–0x3d6800 contains no store to [globals+0x28] at all
(its only globals store is to +0x14, at 0x3d657c); a census of
all 588 Thumb `str/str.w [rN,#0x28]` sites in .text with
backward PIC resolution (240-byte windows, mov-propagation)
found no direct writer; fn 0x16ed00 (the init-transition call in
onDrawFrame) and the scene-switch fn 0x2a8c3c contain no such
store in their tracked PIC flow. The write is therefore made
through a register held across a long function (base computed at
a distant prologue, or reached via a struct pointer argument).
This is a bounded unknown, not a gap in the verdict: the value
is proven, and §5.5's build logs the value outright so no future
pass depends on the writer's identity.

### 5.1.1 The tail, exactly (0x170620–0x1708e0; corrects §4.2's paraphrase)

Per onDrawFrame call with JNI state==4 ([ctx+0x24]; onDrawFrame
0x3d39b4 first presets the status word to 0x3000, calls pump
0x3d3aa0, then the tail):

1. Sound-flag sync (0x17063c–0x1706f0). Note the +0x28 compared
   here is a field of the TAIL's own state struct, not the mode.
2. Clock gate 0x222df8: returns 0 → the frame ends here (no
   dispatch, no render, no engine present; onDrawFrame still
   returns, and main.c still performs its loader swap — §5.3).
3. Deferred-destruction loop (0x224954/0x3d4c8c).
4. Event-queue drain: per queued event, if module byte getter
   0x223aa4()==0, dispatch the current scene's **+0x2c** via
   forwarder 0x2a8ea6 with the event as argument. TITLE's +0x2c
   (0x183820) is the tiny key/event handler of §4 — +0x2c is
   onEvent, not a draw.
5. If fn 0x36f20c() != 0: dispatch the scene's **+0x0c** via
   0x2a8e14, then 0x222e74.
6. Clock feeds 0x220278/0x220a84/0xdc920; then, if
   0x223aa4()==0, helper **0x2a8e2c** — identified this pass as
   the scene-transition completer: if a pending scene sits in
   [mgr+4], it calls the outgoing scene's [vt+0x10], promotes
   pending to current ([mgr+0]), calls the incoming scene's
   [vt+0x8], sets [mgr+0x14]=1, calls 0x223b90. With no scene
   pending it TAIL-CALLS the current scene's **[vt+0x14]** —
   the per-frame tick (INTRO2's 0x145d08 of Bug 31; TITLE's is
   0x182a58). So +0x14 is dispatched every frame from here, not
   from the §4.2 dispatch block.
7. Dispatch-block gates (cached dims vs core getters; latch),
   then the mode read via 0x3d64f4:
   - **mode 3 arm**: pending check 0x3d6e30 — cbnz SKIPS the
     load entry; only on 0 does scene-load 0x16f9b0 run (the
     §3.3 paraphrase had this inverted). Then 0x223cf0 (lazy
     clock-epoch init), dispatch #2 = scene **+0x18** via
     0x2a8e6a, gate 0x3d65e8; if the gate byte is set:
     **render fn 0x220b80**, then **fn 0xdcac8** (a gated thunk
     to 0x214646 — a subsystem update, not a draw), then
     pre-present 0x3d6608. Present wrapper 0x3d6628 is then
     called UNCONDITIONALLY in this arm; its return byte becomes
     the latch.
   - **mode 2 arm**: dispatch #1 = +0x18 only; latch :=
     0x3d65a0(0) (=1). NO render, NO 0xdcac8, NO present.
   - Any other mode: no dispatch at all.
8. Dims re-cache; housekeeping if [state+3]: fn 0x170870 (which
   dispatches the scene's **+0x10** via 0x2a8e20 at 0x17087a)
   and 0x3d3bc4.

Back in onDrawFrame after the tail: if the status word changed
from 0x3000, log via 0x3d3480; then a TRAILING call through PLT
stub 0x7f510, gated on [ctx+0x2c]!=0 AND byte [ctx+4]==1 (its
GOT slot 0x67ffbc carries no name in the extracted .so; the two
stubs before it, 0x7f4f8/0x7f504, are the entry's surface
getters — calls with r0=0 and r0=0x3059/EGL_DRAW whose results
are cached at [ctx+0x1c]/[ctx+0x20]). §5.3 shows by parity that
whatever 0x7f510 is, it cannot be firing as a present in the
TITLE era.

### 5.1.2 There is no DRAW vtable slot

Forwarder family, fully mapped this pass (§4.2's +0x28 line was
wrong): 0x2a8e14→+0x0c, 0x2a8e20→+0x10, 0x2a8e6a→+0x18,
0x2a8e76→+0x1c, 0x2a8e82→+0x20, 0x2a8e8e→+0x24,
**0x2a8e9a→+0x28** (not 0x2a8ea6 — that one is +0x2c),
0x2a8ea6→+0x2c, 0x2a8eb4→+0x30, 0x2a8ec2→+0x34; plus
0x2a8e2c, the transition completer above (not a forwarder).
BL-caller census over .text: +0x1c ← NONE; +0x28 ← NONE;
+0x30 ← NONE; +0x34 ← NONE; +0x20/+0x24 ← scene-load entry
0x16fd28/0x16fd2e only (load-time build hooks); +0x0c ← tail
0x170754 (conditional); +0x10 ← tail housekeeping 0x17087a and
the completer (exit); +0x14 ← completer tail-call (tick);
+0x18 ← tail 0x1707cc/0x1707f0 (the two mode arms); +0x2c ←
tail event drain. TITLE's slots: +0x0c=0x1822b8, +0x10=0x182990,
+0x14=0x182a58, +0x18=0x183368, +0x1c=0x18377a, +0x20=0x18380c,
+0x24=shared stub 0x2a8bfe, +0x28=0x1837ae, +0x2c=0x183820,
+0x30=shared 0x2a8c06, +0x34=0x183840.

Answer to the pass's Q1 sub-question: **the scene's DRAW is not
dispatched through the vtable at all.** Drawing lives in the
tail itself — render fn 0x220b80, called every mode-3 frame
between dispatch #2 and the present, gated by the mode (arm
selection) and the core gate byte. In the constant mode 3,
TITLE's draw runs every frame.

## 5.2 By construction, a TITLE-era frame contains the stage render — and no bumper

A mode-3 frame's GL stream, in order: TITLE's +0x18 sets the
clear color (its "render step" is glue 0x3d6e50 → core
0x3dc95c → PLT 0x7f7d4 = glClearColor; the scene graph is NOT
drawn inside +0x18, §4.1), runs its timed actions; then the
tail's render fn 0x220b80 draws. 0x220b80 is a large
mode-aware orchestrator (0x188 stack frame, FP state saved,
five separate calls to the mode getter, the timed-action
executor 0x2780a8 at 0x221234) whose body is built from the
Zoom scene module (0x1faxxx/0x1fbxxx), node helpers
(0x2aaxxx/0x2acxxx), dims getters (0x3d60f0/0x3d60d8), and the
singleton getter 0x3d6178. Nothing in its call census touches
the bumper: no call to the bumper-texture handle getter
0x2ce7b2, no call to the bumper quad painter 0x2ce600, and the
PIC-reference scan finds no reference to the bumper texture
object (0x8d8208) or its anchor (0x8d81d0) anywhere in the
render.

The bumper has exactly three code paths, all in the loading
module (0x222xxx–0x223xxx), and none can paint a TITLE-era
frame:

- **drawBumper 0x223120** — the only painter of the bumper
  image. Full body this pass: store the progress float at
  [0x8d7dac+0x18]; gate 0x3d65e8 (mode==3 && core byte0), else
  skip to the present; set a black clear color via 0x3d6e50;
  paint the bumper texture as a centered quad via
  0x2ce600(this = texture object 0x8d8208, x, y) from the dims
  getters; compute the bar fill from the stored progress and
  the geometry block at 0x8d7ea6 and draw it via glue
  0x3d6f48 (itself mode==3-gated); pre-present 0x3d6608;
  tail-jump present 0x3d6628. It paints the texture DIRECTLY —
  it does not go through any scene graph or node. Build J:
  called 17 times, all frame=0, and never again. **The loading
  image is not drawn into any TITLE-era frame.**
- **The bumper NODE (global Zoom node at 0x8d63e0)** — bound to
  the bumper texture by reformatBumper 0x22325c and by the
  bumper setup fn (around 0x222f40: name set via 0x278fc4 at
  node−0x2, init 0x277d84(node, singleton 0x3d6178(), 0),
  texture bind 0x2cdd18(texObj, node, [node+0x90], …), bar
  geometry written to 0x8d7ea6, progress zeroed), and touched
  by accessor fn 0x223238 (0x2cdcd8(texObj) then tail
  0x277738(node)). PIC-reference scan: the node is referenced
  ONLY inside the loading module (0x222fa8, 0x223248,
  0x22328c) — not by the manager/loaders window
  (0x160000–0x180000), not by the forwarders, not by the Zoom
  module, not by the render. Nothing outside its own module
  attaches, shows, hides, or draws it. The "persistent
  full-screen bumper node in the scene graph" thread of the
  pass brief is pulled and comes up empty: even if the node
  sits in some graph, the function that paints the bumper on
  screen never uses it, and no TITLE-era code path references
  it.
- **redrawBumper 0x2232f0** — reads the stored progress cell
  and re-enters the same machinery; zero calls in build J
  (its hook never fired; §4.5's J spec confirmed in the log).

Conclusion of Q2: a presented TITLE-era frame, by construction,
contains the clear color plus whatever the stage render draws
(the menu, on Lola's evidence that menu frames exist). The
loading image in the displayed stream is NOT frame content. It
can only arrive at the panel as **buffer residue** — which is
Q3.

## 5.3 The stale buffer: double buffering, two swaps per iteration, one immortal buffer

Loader side (source, this pass): egl_graphics.c calls
**vglUseTripleBuffering(GL_FALSE)** before vglInit — the
surface has exactly **2 display buffers**. The loader's frame
loop (main.c) calls onDrawFrame, then **graphics_swap()** =
eglSwapBuffers on the same display/surface the engine uses.
Per loop iteration in the TITLE era the swap count is:

1. the engine's tail present (0x3d6628 → core swap), once per
   mode-3 frame — certain;
2. onDrawFrame's trailing gated PLT call (0x7f510) — IF it is a
   present and IF its gate ([ctx+0x2c]!=0 && [ctx+4]==1)
   holds;
3. the loader's graphics_swap() — certain, once per iteration
   (361 swaps at frame 360, +1 from graphics_init).

eglSwapBuffers semantics (standard EGL; vitaGL implements the
standard model): the current back buffer is queued for display
in FIFO order, the OTHER buffer becomes the draw target, and
buffer CONTENTS PERSIST across swaps — nothing is implicitly
cleared (the loader's config sets no EGL_SWAP_BEHAVIOR). When
the display queue empties, the panel keeps showing the last
queued buffer until another is queued.

Frame 0's load leaves BOTH buffers holding bumper images:
drawBumper presents 17 times against a 2-buffer surface, each
present painting bumper+progress into the then-current buffer
immediately before swapping. Both buffers end the load era
with finished-bar loading screens.

Now the parity argument. If the per-iteration swap count were
ODD (trailing call firing as a present, total 3), the draw
target would alternate buffers every iteration; the render
would overwrite the bumper in the second buffer within two
iterations, and the loading image would be gone by frame ~2 of
360. It is not gone. If the count is EVEN (total 2: tail
present + loader swap), the draw target is the SAME buffer
every iteration: buffer A receives every fresh menu render and
is queued by the tail present; the loader's swap then queues
buffer B — which no code has drawn into since frame 0 — and B
still holds the finished loading screen. The observation (360
frames of bumper dominance) selects the even-parity regime:
**the trailing PLT call is not firing as a present in the
TITLE era** (gate fields never both set on the in-loop path),
and the effective structure is exactly two swaps per drawn
frame, one of which always re-presents the untouched buffer.

## 5.4 The flashes, quantitatively

Per iteration, the display queue receives [A: fresh menu]
then, microseconds later on CPU time, [B: stale bumper]. The
display drains the queue in order at vsync (60 Hz): A is the
front buffer for one vsync slot (~16.7 ms — "a couple of
frames" when an occasional extra swap or a slow drain gives it
two), then B becomes front and HOLDS for the rest of the
iteration gap, because the queue is empty and nothing else is
queued until the next iteration. In build J's log-throttled
run the iteration period is hundreds of ms to seconds (30,091
log lines; heartbeat cadence), so the menu's duty cycle is
~16.7 ms per iteration — of the order of 1–5% of wall time,
with the bumper filling the remainder. That is precisely
Lola's report: "the main menu flickers… only shows for a
couple frames then back to the loading screen" — and it also
retrodicts build I's identical report under the same structure
(the §4.4 interposition preserve rule is unaffected: this
mechanism lives in the present path, not the dispatch path).

Corollaries, all consistent with the record: the menu flash
rate equals the loop iteration rate (not the vsync rate); the
flashes began exactly when the clock fix let TITLE start
rendering (build G onward — in G/H the same residue buffer
explains the "steady loading screen" once frame 13's hang is
set aside, since a hung frame stops queueing A but B was
already the held front buffer from the load); no EGL errors
appear because nothing in this path is an error — every swap
succeeds (r=12288 throughout).

### Ranked verdict

1. **(Proven) Mode ≡ 3; every TITLE-era frame dispatches
   TITLE's +0x18, renders the stage via 0x220b80, and presents
   via the tail.** The draw is the tail's own render call,
   mode-gated by arm selection — not a vtable slot; slots
   +0x1c/+0x28/+0x30/+0x34 are never dispatched by the manager
   at all, +0x2c is onEvent, +0x20/+0x24 are load-time only.
2. **(Proven) Nothing draws the loading image after frame 0.**
   drawBumper is its only painter and never fires again; the
   bumper node 0x8d63e0 is loading-module-private and is not
   what drawBumper paints (it paints the texture directly).
   TITLE-era frames contain the menu render, not the bumper.
3. **(Mechanism — fits every observation, one empirical link
   left for build K) The screen's bumper is a stale display
   buffer.** Double buffering (triple explicitly disabled),
   an even two effective swaps per iteration against a fixed
   draw target, leaves one buffer holding frame 0's finished
   loading screen, re-queued once per iteration by the
   loader's own graphics_swap immediately after the engine
   queues the fresh menu buffer. FIFO drain + hold-last gives
   the ~1-vsync menu flashes inside a bumper-dominated
   display.
4. **Ruled out this pass:** the §4 repaint war (already dead
   in J; §5.2 closes the node-graph variant of it); a mode-2
   steady state (no render/present path exists in that arm);
   an HTTP/data gate on the draw (no bumper or HTTP reference
   exists anywhere in the render's call census); the clock
   gate as the flash source (it aborts whole frames, which
   under §5.3 merely lengthens B's hold — it cannot create
   the alternation).

Residual unknowns, stated plainly: the mode cell's writer
(value proven; §5.1); the identity of onDrawFrame's trailing
PLT target 0x7f510 (parity proves it is not acting as a third
present, whatever it is); vitaGL's internal queue-wait policy
(affects flash duty cycle in an unlogged fast build, not the
mechanism). None of these blocks the next build.

## 5.5 Minimal next build (K — diagnostics only, no fix attempt)

Keeps the §4.4 preserve list intact (the 0x2a8e6a dispatch
interposition stays, byte for byte) and changes only logging:

1. **Log the mode value outright once** — tail_mode_check
   already reads the cell every frame; add a single
   unconditional line at the first sample ("TAIL: mode = %d
   (constant-value baseline)") instead of change-only logging.
   Closes the writer question by measurement.
2. **Bracket the render fn 0x220b80** with the same inline-hook
   pattern DISPATCH used for 0x2a8e6a: enter/exit lines with
   frame/swaps counters, capped at 200 then milestones. (The
   pass brief expected a "DRAW slot forwarder"; §5.1.2 shows
   the draw is this tail-called function, so the bracket goes
   on 0x220b80 itself.) Confirms the render runs every frame
   in the TITLE era and returns.
3. **Count present-wrapper calls**: increment a counter inside
   a hook on 0x3d6628 (or count in the existing trampoline
   style) and print it in the 60-frame heartbeat next to the
   loader swap count. The TITLE-era delta per frame is the
   empirical swap parity: engine presents/frame = 1 with
   loader swaps/frame = 1 ⇒ total 2, confirming §5.3's regime;
   any other number rewrites §5.3 before any fix is shaped.

Decision tree after K: if (1)–(3) confirm §5.3, the FIX build
(L) is a present-structure change, not an engine change — the
candidate shape is to stop the loader's unconditional
per-iteration graphics_swap (the engine already presents in
mode 3) or to make the loader swap only when the engine did
not present that iteration; the exact shape is chosen from K's
parity numbers, one variable, interposition preserved. If K
instead shows the render NOT running per frame (bracket
silent while dispatches continue), §5.1's arm reading is wrong
somewhere and the pass reopens at the tail — K is designed to
distinguish exactly these two worlds.
