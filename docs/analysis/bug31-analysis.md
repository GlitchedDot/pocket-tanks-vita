# Bug 31 — Engine runs, draws 1,184+ frames, but the game is not visible:
# the pre-menu scene (INTRO2) never exits

**Date:** 2026-10-13 · **Pass type:** analysis only, no builds ·
**Evidence base:** `bootlog-7.txt` (build 2026-10-13-E run, 28,111 lines),
`log-7.txt` (game log), loader source in `pockettanks-vita/source/`,
`libengine.so` (extracted/lib/armeabi-v7a/, ELF VA == file offset,
text_base = 0x99000000), and the APK's decompiled Java in `jadx-out/`.

## What is verified (not theory)

- The engine completes init: game log ends `Switching to scene INTRO2`
  (16:31:52) → `Initializing Game Complete` (16:31:53). All asset loading ends
  at bootlog line 10,231; the frame loop starts immediately after
  (`onDrawFrame returned 12288 at frame 0`, line 10,315) and reaches
  frame 1184 at the log's end (line 28,100).
- **Both remaining scenes are already resident.** The title scene's
  `title.xml`/`title.lua` and its button art load during the initial load
  (lines 6,299–7,473), and `intro2.xml`/`intro2.lua` at lines 8,329–8,341.
  Nothing the menu needs is ever requested later — asset opens stop entirely
  after line 10,231, and during the whole frame span the only recurring
  activity is mutex/getspecific traffic, two `getMusicFlag` polls per frame,
  and the draw calls themselves.
- **The engine spawns no threads.** `pthread_create` appears **0 times** in
  bootlog-7; all engine work (update + draw) runs inside `onDrawFrame` on the
  loader's main thread. There is no missing update thread.
- Per Lola's eyes and her video: the screen shows the logo/loading artwork,
  the bar fills, then **blinks forever**. The main menu never appears.

## Verdict ranking

### 1. (LIKELY) The stall is inside the engine's scene layer: INTRO2's exit condition never fires

The loader's loop is `input_poll(); onDrawFrame(); graphics_swap();` —
unconditional, same thread, same surface (see §A). Frames demonstrably reach
the panel (the bar animates/blinks — that is drawn content changing over
time). So the engine is drawing *some* pre-menu scene, correctly presented,
for 1,184+ frames — and the scene simply never hands off to the title.

The exit from a Blitwise Zoom intro scene is driven by the scene's own
script/timeline (`intro2.lua`, 132 bytes, obfuscated like the rest of the
zoom tree; not decoded in this pass). Its completion condition is unmet in
our environment. Sub-candidates, in order:

**(a) The script waits on an audio state that can never occur.** Audio in
this port is a stub desert by design: `sound.c` hard-fails OpenSL
(`slCreateEngine` → `SL_RESULT_FEATURE_UNSUPPORTED`, "no audio in v1"),
the game's own log records `FMOD create system error! (41)` at init, and
the Java music path (CPJNISound / Android MediaPlayer) is dummy-classed
(§B). If INTRO2's script waits for an intro jingle / mod playback state /
sound-ready flag, it waits forever while rendering its splash. This is the
only engine subsystem that is *known* to be both (i) involved at scene
level and (ii) completely non-functional in the port.

**(b) The scene exits on a touch that our input path never delivers.**
INTRO scenes in this engine commonly advance on tap. The loader's input
chain (`input_poll()` → fake MotionEvent → `N_onTouch`) has never been
exercised end-to-end in any log — no touch forward has ever been observed.
If INTRO2 is tap-gated and our synthesized MotionEvent is malformed,
mis-coordinated, or never sent, the scene sits forever. Cheap to test
(Lola taps during a run) and cheap to audit (log every forwarded touch).

**(c) A script timer fed by a delta-time source that misbehaves here.**
Weak after source inspection: the loader binds `gettimeofday`/`time` to
RTC-backed shims that return real advancing time (verified in
`bionic_compat.c`), `clock_gettime` and `clock` go to the SDK/newlib
implementations, and the bar's visible blink shows time-varying rendering.
No positive evidence for a frozen clock; kept only for completeness.

### 2. (RULED OUT as the gate) The `getMusicFlag` JNI miss

Closed with the engine's own disassembly plus the APK's Java source:

- The lookup targets **`com/blitwise/engine/jni/CPJNISound.getMusicFlag()Z`**
  (jadx-out `CPJNISound.java:64`) — a static boolean mirror of the game's
  music setting, consulted by the *Java MediaPlayer* music path
  (`playMod`, `onResume`). The engine pushes its setting down via the
  matching `setMusicFlag` glue and polls the getter to keep its settings
  tree in sync.
- Native getter glue = fn **0x3da03c** (0x7c bytes): FindClass (cached at a
  static), GetStaticMethodID (cached at a static), CallStaticBooleanMethod,
  normalize to 0/1. **The method ID is only re-asked every frame because
  our bridge returns NULL** — the cache never fills, so every poll re-logs
  `NOT FOUND`. The 2,376-line flood is a cache-miss artifact, not per-frame
  intent.
- Its 11 call sites in 8 functions are settings/music plumbing: e.g. fn
  **0x170620** copies four sound-flag bytes into a settings struct, calls
  the four flag getters (0x3d9eb4, 0x3da03c = getMusicFlag, 0x3d61b6,
  0x3d61be), and pushes *changes* into a ptree via fn 0x28e764. Nothing in
  any caller touches scene state, timers, or transitions.
- In this run the setter glue was never invoked at all (no `setMusicFlag`
  lookup appears in the log), so the flag would read `false` (the Java
  field default) even behind a perfect bridge, until the engine pushes its
  setting. Our dummy therefore returns the *same value* stock code would
  see at this point — it cannot be what holds the menu.

Implementing it properly is still worthwhile hygiene (kills ~2 log
lines/frame, lets the flag synchronizer converge, restores music-flag
semantics if the audio path is ever built), specified in §Fix below —
but it is **not** the Bug 31 fix and must not be sold as one.

### 3. (RULED OUT as primary) The present/swap path

- `main.c`'s frame loop calls `graphics_swap()` **unconditionally** every
  iteration, immediately after `N_onDrawFrame` — `eglSwapBuffers` on the
  vitaGL window surface (egl_graphics.c: `vglUseTripleBuffering(GL_FALSE)`,
  RGBA8, no depth/stencil).
- `onDrawFrame returned 12288` is the game's own `native int` status word
  (CPJNILib.java:135), constant across all 1,185 calls; the loader logs it
  only because it is nonzero. Presentation does not depend on it.
- There is no surface recreation anywhere to go wrong at the scene switch:
  the single EGL context/surface is created in `graphics_init()` *before*
  `onCreate`, the 960×544 size is set once via `onSurfaceChanged`, and
  context, surface, and thread never change afterward.
- Presents demonstrably work in the frame-loop phase: the bar Lola filmed
  progresses and blinks, i.e. successive presented frames differ. (Note:
  no frames at all are presented during the ~4-minute load — the loop
  starts only after `onCreate` returns — so "the bar progressed" refers to
  the drawn scene's own fill/idle animation after init, not mid-load
  rendering.)
- Residual caveat, stated for honesty: with the loader's per-line file-I/O
  logging, the effective frame rate is very low (frames run from 16:31:53
  until log capture; 1,185 frames over minutes ≈ a few fps). Even so, an
  intro timed in seconds had hundreds of frames and minutes of wall time
  to finish; slowness alone does not explain a permanent stall, and no
  swap errors appear anywhere in the log.

## §Fix — the one certain, small fix (specified, NOT built): CPJNISound statics in jni_bridge

In `jni_bridge.c`:

1. `FindClass("com/blitwise/engine/jni/CPJNISound")` returns a real
   registered class token (distinct from `falso_jni_dummy`), so the
   engine's glue caches it.
2. `GetStaticMethodID` on that class resolves `getMusicFlag` `()Z` and
   `setMusicFlag` `(Z)V` to stable fake method IDs (the engine caches
   them after first success — this alone ends the NOT FOUND flood).
3. Back them with a loader-side `static int music_flag = 0;`
   (0 = the Java field default; the engine's own setter glue is the only
   writer, exactly as on Android). `CallStaticBooleanMethod(getMusicFlag)`
   returns it; `CallStaticVoidMethod(setMusicFlag)` stores the argument.
4. While there: identify the sibling flag getters the synchronizer polls
   (glue fns 0x3d9eb4, 0x3d61b6, 0x3d61be) by the same xref method and give
   their class/methods the same treatment, so fn 0x170620's four-flag sync
   converges instead of spinning.

**Expected effect:** log noise drops by ~2 lines/frame; the engine's
sound-flag sync settles. **Not expected:** the menu appearing — see §2.

## Recommended next discrimination (one instrumented run)

1. Land the §Fix first (above) purely to clean the log.
2. Log the scene manager's switch requests: find the printer of
   `Switching to scene %s` in libengine, hook its entry in the loader,
   and log every requested scene name. If `title` (or the title scene's
   registered name) is ever requested, the fault is inside the switch;
   if it never is, the fault is INTRO2's exit condition — as ranked here.
3. In the same build, log every forwarded touch (`onTouch` action +
   coordinates) and have Lola tap the screen during INTRO2 — this
   directly convicts or clears sub-candidate (b).
4. If neither fires, the remaining suspect is (a): log the FMOD/OpenSL
   calls the engine makes during INTRO2's frames (it currently makes
   none that reach the bootlog after init — itself a hint that the
   script's audio wait, if any, is on the Java/CPJNISound side, whose
   calls our bridge should then log at the method-dispatch level).

---

# Addendum — INTRO2 exit condition, named exactly (2026-10-13, second pass)

**Pass type:** analysis only, no builds · **New evidence:** build F run
(`bootlog-8.txt`: exactly one scene requested all run — "INTRO2"; 84 INPUT
lines prove taps/buttons reach the engine and are ignored; the music-flag
bridge round-trips), full disassembly of the INTRO2 scene class and the
engine time globals in `libengine.so`, and the loader's dynlib bindings.

## Headline

**INTRO2's exit condition is a wall-clock timer, and nothing else.** The
scene is a native C++ timed sequence: it accumulates per-frame delta time
into a scene-local elapsed counter, fires eight scripted steps at fixed
thresholds (0 → 6.5 s), and the last step calls the scene switch with the
name **"TITLE"**. There is no audio predicate, no input predicate, and no
script/Lua gate anywhere in the chain. On stock Android the sequence
completes ~6.5 s after the scene starts. In our build the accumulated
delta is effectively zero every frame, so the elapsed counter never
crosses even the first non-zero threshold — the intro's opening step is
the only one that can ever fire, and the switch is never requested. The
single external input to the whole predicate is the engine's per-frame
delta, `timePassed` (int, milliseconds), which is derived from
`clock_gettime(CLOCK_MONOTONIC)` — the one clock binding in the loader
that has never been verified on hardware (dynlib.c binds it to the SDK
newlib implementation, whose MONOTONIC path is an RTC conversion).

## The mechanism (all addresses = libengine file offsets)

INTRO2 is a native class, not a script. Its exported statics resolve to:
`c_zoomSceneZML = "zoom\intro2.xml"`, `c_zoomSceneLayerId =
"intro2_scene"`, `c_faderId = "intro2_fader"`, `c_creatorsHeadlineId`,
`c_creatorsText1..3Id`, `c_logoEarth = "earthboing1500x750"`,
`c_logoEarthStencil`, `c_logoLetters = "letters1500x750"`, and
`c_stingFilename = "sfx\intro_sting.wav"`. The ZML only supplies layout
and node ids; the behaviour is compiled.

**Tick — fn 0x145d08** (per frame, virtual dispatch; no direct callers):

```
timeFrame      = 0.004                                   // scratch global
timeAccumulator += timePassed / 1000.0                   // timePassed: int ms
while (timeAccumulator > 0.004) {
    timeAccumulator -= 0.004;
    sequence(this, dt = 0.004);                          // fn 0x145e2c
    sub_update_1..5(this, dt = timeFrame);               // fns 0x145f00,
}                                                        // 0x145f78, 0x146038,
                                                         // 0x1460b0, 0x1460f2
```
Globals (`.bss`, named in dynsym): `theTime` @0x8d7d80, `timePassed`
@0x8d7d84, `oldTimePassed` @0x8d7d88, `timePassedInSeconds` @0x8d7ec0,
`timeFrame` @0x8d7ec8, `timeAccumulator` @0x8d7ed0.

**Sequence — fn 0x145e2c.** State: `step` (int @ this+0x104), `elapsed`
(double @ this+0x108). Each call adds dt, then, while
`elapsed > threshold[step]`, fires the step's action and increments
`step` (multiple steps can fire in one call). Threshold table (doubles
@ .rodata 0x6897d0) and actions (TBB jump table decoded by hand):

| step | fires at | action |
|------|----------|--------|
| 0 | > 0.00 s | fire-and-forget JNI-glue call fn 0x3d50e4(`this->[+0x518]`, −101); result discarded |
| 1 | > 0.25 s | set flag `this+0x130` = 1 (arms the logo animator, fn 0x145f00) |
| 2 | > 0.98 s | show ZML nodes `this->[+0x114]`, `this->[+0x118]` (fn 0x321992(node, 1)) |
| 3 | > 1.85 s | set flag `this+0x1f0` = 1; show node `this->[+0x11c]` |
| 4 | > 2.28 s | show node `this->[+0x120]` |
| 5 | > 2.50 s | set flag `this+0x2a8` = 1 |
| 6 | > 5.00 s | fn 0x14653c(this): start a 1.25 s tween on `this+0x2b8` (fade-out) |
| 7 | > 6.50 s | **fn 0x2a8c3c(sceneManager, "TITLE") — the scene switch** |

After step 7, `step` = 8 and the sequence is inert. The next scene's
registered name is the literal string **"TITLE"** (@0x62b3ce), requested
from fn 0x145ed4 — and build F's SCENE trace confirms it is never
requested in our build.

**Time source.** `timePassed` is produced by engine code from fn
**0x3d8340** = `clock_gettime(CLOCK_MONOTONIC, &ts)` converted as
`tv_sec*1000.0 + tv_nsec/1e6` → uint32 ms. Every scene-init that touches
the time base (fns 0x14d30c, 0xaf6a4, 0xc0c18, 0xfcac8, 0x394900) does the
same reset: `theTime = now_ms(); timePassed = 0; timeAccumulator = 0.0`.
The steady-state per-frame writer itself is reached through the frame
pump's virtual dispatch (onDrawFrame → fn 0x3d3aa0 registry loop) and was
not isolated by static xref — stated plainly so the next pass doesn't
re-hunt it; it is not needed for the fix (see below).

## Why the Lua/XML don't hold the answer

Both files use the Zoom obfuscation (alternating XOR 0x03/0x18 plus a
byte permutation; the XOR layer is fully confirmed — newlines, dash
banners and spaces decode in place — while the permutation resisted
complete automated recovery, best transforms ≈75% legible). Their
content is nonetheless bounded with certainty:

- `intro2.lua` (132 bytes) is **pure boilerplate**: it is byte-identical
  for 128 of 132 bytes to `options_old.lua`'s header (the shared Zoom
  Lua prologue — comment banners, "-- global variables --" / "-- local
  functions --" sections, legible in the XOR-decoded stream). It
  contains no statements and no completion logic.
- `intro2.xml` (1,576 bytes) supplies only the layout the native class
  drives by id (fader, creators headline/texts, earth/letters logo). The
  gate is the compiled threshold table above, not any XML timeline.

Sub-candidate (c) from the first pass (delta-time fault) is therefore
**promoted to the mechanism**, in a specific form: not a frozen clock
feeding a script, but a zero delta feeding this accumulator.

## Predicate inputs: stock vs ours

| input to the predicate | stock Android | our build |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` → fn 0x3d8340 (ms) | true monotonic ms, advances in real time | dynlib.c:250 binds the SDK **newlib** `clock_gettime`. Its MONOTONIC branch is *not* a monotonic counter: it calls `sceRtcGetCurrentClock` + `sceRtcGetTime_t` (RTC wall-clock DateTime → time_t, µs×1000 for nsec). By inspection it should advance, but it has **never been verified on hardware in this port**, and a constant/failed result reproduces the observed stall exactly (delta ≡ 0 for 1,184+ frames, even at the log-throttled few-fps rate where a real delta would be hundreds of ms per frame and would exhaust all 8 steps within ~13 frames) |
| `timePassed` (int ms/frame) | real per-frame delta | effectively 0 every frame (consequence of the above, or of the writer never being driven — the fix and discriminator below separate these) |
| audio state (OpenSL / FMOD / CPJNISound) | sting plays | **not an input**: no audio state is read anywhere in fns 0x145d08 / 0x145e2c / 0x2a8c3c. The sting (`sfx\intro_sting.wav`) is started fire-and-forget; step 0's JNI-glue result is discarded. The still-unimplemented CPJNISound instance methods (`freeWav`, `freeMod`, `stopMod`, `onResume`, `modVolume`) are never called by the intro path. FMOD error 41 is irrelevant here. **Sub-candidate (a) is dead.** |
| input (touch / gamepad) | tap skips nothing; intro is timed | reaches the engine (84 INPUT lines, build F) and is never read by the sequence — matches Lola's no-reaction tap test. **Sub-candidate (b) is dead.** |
| `getMusicFlag` | setting mirror | round-trips since build F; never read by the intro path. Dead (again). |

## Minimal loader change that satisfies the predicate

Give the engine a monotonic clock that cannot stall, at the exact seam
it consumes:

1. In `bionic_compat.c`, add `clock_gettime_soloader(clockid, tp)`:
   - `CLOCK_MONOTONIC` / `CLOCK_MONOTONIC_COARSE` /
     `CLOCK_PROCESS_CPUTIME_ID`: from
     `sceKernelGetProcessTimeWide()` (µs since boot — a genuine
     monotonic source; the same primitive newlib itself uses for its
     CPU-time branch): `tv_sec = us / 1'000'000`,
     `tv_nsec = (us % 1'000'000) * 1000`, return 0.
   - `CLOCK_REALTIME`: from the existing RTC-tick path already used by
     `gettimeofday_soloader` (µs since 0001-01-01 minus the Unix epoch
     offset), return 0.
   - anything else: return −1 with `errno = EINVAL`.
2. In `dynlib.c`, change the `clock_gettime` row to point at
   `clock_gettime_soloader` instead of newlib's `clock_gettime`.

~20 lines, one binding row. Scope is limited to libengine's imports
(the loader's own code links the libc function directly and is
unaffected); boost::asio's deadline timers, the other consumer, get a
strictly better clock.

**Expected runtime proof (already instrumented in build F):** the bootlog
gains `SCENE: switch requested -> "TITLE"` ≈6.5 s of accumulated frame
time after INTRO2 starts, followed by the game log's own
`Switching to scene TITLE`. **Discriminator if it still doesn't fire:**
the delta source is exonerated and the remaining explanation is that the
frame pump never invokes the tick (fn 0x145d08) in our build — the next
instrumentation would be a one-line log inside the tick's accumulator
loop printing `timePassed`/`timeAccumulator` once per second of frames.
Per Lola's standing rule, what the screen shows after such a build is
hers to report; this analysis claims mechanism only.

