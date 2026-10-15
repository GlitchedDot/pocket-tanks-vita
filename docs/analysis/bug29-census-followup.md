# Bug 29 — Census Follow-Up: the weapon-phase `bad_lexical_cast` is NOT the ptree/date crash

**Date:** 2026-10-13 · **Pass type:** analysis only, no builds ·
**Evidence base:** `bootlog-2.txt` (diagnostic-build run, 4,007 lines), `libengine.so`
(ELF VA == file offset; runtime text_base = 0x99000000), loader source in
`pockettanks-vita/source/`, weapon data in `build/pockettanks-data/assets/weapdata/`.
Supersedes the mechanism in `bug29-diagnosis.md` (kept for reference; its Q3 policy-v2
proposal is **moot** — do not build it).

---

## TL;DR — corrected mechanism

1. **The fake ptree is exonerated.** All 46 FAKE-DIAG handouts come from one call site
   (`text_base+0x323ac5`, path exactly `"<xmlattr>"`, bootlog-2 lines 2493–3253, during
   `zoom/default.xml` loading). Zero occur during weapon loading (lines 3872–4007).
   The patched `get_child` (0xa4888) is **not on the weapon crash path at all**.
2. **The predecessor's "weapon evaluator" is Zoom UI code.** Functions 0x369ce4 and
   0x36a10c — the core of the old Q1 chain — are the engine's **ZML (Zoom markup) file
   loader**: 0x36a10c reads `<xmlattr>.version`, `<xmlattr>.defaultTTFont`,
   `<xmlattr>.defaultLuaFile`, `<xmlattr>.originalFilename` and can log
   `"Error: Invalid ZML file version number [file(%f)…"`. The Oct 6 coredump the old
   diagnosis leaned on was a different crash (Bug 27); the chain built on it does not
   describe this one.
3. **The date hypothesis is dead, not just demoted.** The gregorian/`lexical_cast<short>`
   machinery is *unreachable* from the weapon loader: a direct-call BFS from the loader
   never reaches the date dispatcher (0x97aec), the date parser (0xa57a0), translator F
   (0x980f4), or the short-cast core (0x9863c). Their only callers are boost-internal.
4. **The weapon data never becomes a ptree.** `.weap` files are a custom text script
   format parsed by `weapscript.cpp` code (loader fn 0x18966c → pack parser fn 0x189b38
   → field parser fn 0x18a910). No XML, no property_tree anywhere in that path.
5. **The fatal cast consumes a raw string token from the weapon script** — the Q3
   hypothesis, refined. Exactly five `bad_lexical_cast` throw stubs are reachable from
   the weapon loader; the prime suspect (shortest path, depth 4) is the
   **`lexical_cast<T>` wrapper at 0x18c000, called from the `LOOP` command handler** of
   the weapon field parser (call site 0x18b0f0). The runner-up (depth 5) is a
   **luabind converter wrapper at 0x21ea9c**, reached via the Lua-hook weapon class —
   the "Lua layer delivers a bad value" variant. One more instrumented run (spec in §5)
   discriminates between them and prints the exact failing token.

---

## 1. Q1 — Which get_child does 0x369de2 reach, and what is the function at 0x323ac5?

### 1.1 The call at 0x369de2

It is a **direct `bl 0xa4888`** — the patched `get_child` itself. Verified in
disassembly of fn 0x369ce4:

```
369dd2: ldr r1, [pc, …]        ; -> "ZOOM"
369dd8: add r1, pc
369dda: bl  9d470              ; string_path ctor (path buffer sp+48, sep '.')
369dde: add r0, sp, #32        ; tree
369de0: add r1, sp, #48        ; path "ZOOM"
369de2: bl  a4888              ; <-- the patched get_child, directly
369de6: mov r2, r0             ; result -> arg for evaluator
369df2: bl  36a10c             ; evaluator
```

Immediately before it, the same function calls the copy-form wrapper **0xa482a**
at 0x369d92 (0xa482a = `out = tree.get_child(path)`: it calls 0xa4888 at 0xa4834 and
copies the result via 0xa9ca8). So the old diagnosis was right about the *addresses*
and wrong about *whose code this is* (see 1.3).

For completeness, the patched function itself is now fully identified from its own
disassembly: 0xa4888 does one internal find (0x9fe6c); on a miss it constructs
`boost::property_tree::ptree_bad_path` — its rodata literals resolve to
`"No such node"` and
`"../../../../../engine/cp_libs/boost/include/boost/property_tree/detail/ptree_implementation.hpp"`.
Patch site 0xa48b0 (`mov r5,r0; cbz r0,0xa48d0`) sits exactly between the find and
the return/throw split, as the loader's trampoline assumes.

### 1.2 The census caller, fn 0x323a24 — a Zoom/ZML attribute helper, not a funnel

- Its literal pool resolves to `"<xmlattr>"`, `"language"`, `"lua"`, `"file"`,
  `"function"` (plus the stack-canary GOT slot). It iterates a ptree's children
  comparing keys against the literal `"<xmlattr>"`, then calls `get_child` at
  **0x323ac0** with a path built from that same literal — return address 0x323ac4/5,
  i.e. the FAKE-DIAG caller, with path length 9 = `"<xmlattr>"` exactly, 46/46 times.
- It also calls the locale-based conversion helper 0x225f40 and the common
  script helper 0x9d40c — the same toolbox the ZML loader (0x36a10c) uses.
- It is **not** a ptree-internal funnel: a full .text census finds **30 direct call
  sites of 0xa4888 in 26 different functions** (callers include 0xa482a, 0x323a24,
  0x369ce4, the `get_value` helper at 0x2cfba0, and 22 others). Fn 0x323a24 is simply
  the only caller whose probe path is the bare literal `"<xmlattr>"`, and it runs
  while the Zoom UI loads — which is why the fake ptree's only customer is the zoom
  phase. On this evidence the 46 handouts are the helper probing for an attributes
  node the Vita-side ZML tree doesn't have; the UI tolerates the empty tree and
  carries on. Benign, and unrelated to the crash.

### 1.3 What 0x369ce4 / 0x36a10c actually are

Literal pools resolved this pass:

- **0x369ce4**: `"ZOOM"` (both path constructions), `"OnLoad"`, `"OnAwake"` — a Zoom
  UI node lifecycle function: fetch the `ZOOM` child, hand it to the evaluator,
  dispatch Lua lifecycle hooks.
- **0x36a10c**: `"<xmlattr>.version"` (read as **double** via helper 0x2cfba0 at
  0x36a184, compared against 1.0), the error string
  `"Error: Invalid ZML file version number [file(%f)…"` (0x653778),
  `"<xmlattr>.defaultTTFont"`, `"<xmlattr>.defaultLuaFile"`,
  `"<xmlattr>.originalFilename"`, child names `"Scripts"`, `"ScriptsDebug"`,
  `"ScriptingPeristence"` — this is the **ZML document loader**.

So the predecessor's chain (0x369d60 → 0x369de2 → 0x36a10c → `get_value<date>` →
`lexical_cast<short>`) collapses at its second step: 0x36a10c never calls the date
translator, the short-cast core, or any of the 29 bad_lexical_cast throw stubs at
short range — its reachable throw stubs are 0x993cc / 0x1ccdd4 / 0x22dd9c (depth 3,
ZML-TU conversions), none of which fire during the weapon phase (the zoom phase
completes; the throw happens 700+ log lines later, after `weaplist_31.lua`).

### 1.4 "Was the node found?" — the question dissolves

There is no weapon-phase `get_child` call to have an outcome. BFS over the direct
BL call graph from the weapon loader (fn 0x18966c, §2) reaches 876 functions;
**0xa4888 is not among them** — nor are 0x97aec, 0xa546c, 0x980f4, or 0x9863c.
The weapon loader cannot produce a FAKE-DIAG, a `ptree_bad_path`, or the date cast
through any direct call path. That is exactly what bootlog-2 shows: patch APPLIED
(line 53), zero FAKE-DIAG after line 3253, zero `ptree_bad_path`, one
`bad_lexical_cast` THROW (line 3998, ret0=0x99098877).

Why the throw's return address can't localize it by itself: the `__cxa_throw` call
sits inside a shared helper at 0x98850 (+0x26 → ret 0x98877), used by **29**
per-TU throw stubs (identical-code-folded `wrapexcept<bad_lexical_cast>` throwers).
ret0 therefore only proves "some bad_lexical_cast". The loader hook's frame walk
also yields nothing here (line 3999: ret1–ret5 all zero). Localizing the cast
requires logging the *stub* that entered the helper — see §5.

---

## 2. Q2 — Where the weapon data actually goes; what the data is

### 2.1 The real loader path (all addresses = libengine file offsets)

- **fn 0x18966c** (620 B) — pack-list loader. Enumerates the weapdata directory
  through the engine's asset TU (0x3d79e4 / 0x3d7fa4 / 0x3d80fc = openDir /
  getNextFileName / closeDir equivalents; our `asset_mgr.c` backs these with plain
  `fopen`/`fread` under `ux0:data/pockettanks/assets/` — bytes delivered **verbatim**,
  no transform). It keeps names ending in `.wep` / `.weap` (strcmp against those
  literals), sorts them, dedupes, and calls the pack parser once per file
  (call site 0x189838). Other callers: the weapon-scripting screen (0xead70,
  "Reload Weapon Scripts"), the pack shop UI (0xfcf98, 0xfd548, 0x100700, 0x10088c),
  and 0x16ed00. Surviving symbols corroborate the subsystem: source path
  `/Users/ben/bw/ptanks_hd/…/src/weapscript.cpp`, `WeapScript::s_currentScriptingPtr`,
  and `WEAPLIST_STRUCT` bound to Lua via luabind with int / string / double / bool
  properties.
- **fn 0x189b38** (3,048 B) — pack parser. A keyword dispatch over the pack header —
  literals in order: `VERSION:`, `WEAPON_INDEX:`, `FREE_PACK:`, `COMING_SOON:`,
  `NEW_PACK:`, `PRE-INSTALLED_PACK:`, `PACK_NUMBER:`, **`RELEASE_DATE:`** (ref site
  0x189eaa), `PACK_DESCRIPTION:`, `PACK_IDENTIFIER:`, `LUA_FILE:`, `BEGINGAME_LUA:`,
  `ENDGAME_LUA:`, `DOGAME_LUA:`, `LATEDOGAME_LUA:`, `BEGINROUND_LUA:`,
  `ENDROUND_LUA:` — then `WEAPON:` sections with `TRIGGER:`, `BULLET:`,
  `EXPLOSION:`, `SHRAPNEL:`, `DIRTBALL:`, `CRUISER:`, `FIRE:`, `SUPERBALL:`,
  `ZAPPER:`, `DIRTSLINGER:`, `MAGICWALL:`, `DIRTMOVER:`, `TRACER:`, `JUMPJETS:`,
  `LIGHTNING:`, `FOG:`, `DEBRIS:`. Header numbers are extracted with the tokenizer
  veneer 0x2884f4 (→ 0x288054, a string find/split helper) plus plain `atoi`
  (e.g. 0x189caa) — **no lexical_cast in the header path, and `RELEASE_DATE:` is
  stored, not converted** (the parser makes no call to any date function).
  Sub-parsers: tracer parser 0x184e74, weapon field parser 0x18a910 (call site
  0x18a5c4), and three Lua-hook weapon-class parsers (0xe31c4 / 0xf5370 / 0x106e30 —
  the `INIT_LUA:` / `BEGIN_LUA:` / `DO_LUA:` / `END_LUA:` / `LATEDO_LUA:` TUs).
- **fn 0x18a910** (2,612 B) — weapon field parser. `memcmp` dispatch on field names
  (`ANGLE:` @0x18aa82, `POWER:` @0x18aa94, `SOUND_EFFECT:` @0x18aaa6, `LUA:`
  @0x18aab8, `LOOP` @0x18aaca, `TIMEDELAY:`, `COMMAND:`, `TYPE:`, `XOFFSET2:`,
  `YOFFSET2:`, …). This is where the prime-suspect cast lives (§3).

### 2.2 The data files

- Inventory: lists 1–14 are `.wep` only; 15–30 are `.weap` only; **31–41 pair
  `.weap` + `.lua`**. List 31 is simply the first Lua-era pack in sort order —
  "crashes on list 31" means "crashes on the first pack whose processing goes past
  the `.weap` header into Lua-era content", not necessarily a unique defect in 31.
- The bytes are **stream-obfuscated text** (identical to stock Android — our asset
  shim performs no transformation; MD5s of the extracted files:
  `weaplist_31.weap` f58c13a8234fd6a124f399fc34d22ce4,
  `weaplist_30.weap` b30c57aae275496990c490a18a57f9d4,
  `weaplist_31.lua` fd035a3d9861f6d962edde7cae9f5205).
- Partial decode status (reported honestly): the byte layer is an alternating
  XOR key (0x03, 0x18) — an independent per-column fitness solve confirms it is
  optimal (99.9% printable on both files; trial periods 4 and 8 collapse to the
  same key) — and it exposes the format: a 16-dash Lua comment banner, fragments
  of list 31's header including `RELEASE_DATE: 20…` and running
  `PACK_DESCRIPTION:` prose. However, letter order *within* words is scrambled
  by a second, unidentified transposition layer on top of the XOR (a crib
  search for contiguous `RELEASE_DATE:` / `WEAPON_INDEX:` / `PACK_NUMBER:` /
  `VERSION:` / `WEAPON:` / `LUA_FILE:` finds no hit, and block-reversal /
  columnar / pair-swap candidates did not beat the base decode's digram
  fitness), so a byte-exact decode was **not** achieved this pass. The engine
  necessarily de-obfuscates in its own read path (the same bytes load fine on
  Android), so the failing token's exact text cannot be quoted yet — one
  reason §5 proposes logging it at runtime instead of more cryptanalysis.

### 2.3 Bootlog-2 timeline for the crash window

| Line | Event |
|---|---|
| 2487 | `zoom/default.xml` opens; FAKE-DIAG burst follows (2493–3253) |
| 3698–3699 | `weapscript.cfg` fopen → errno=2 (data dir, pre-loading) |
| 3706–3707 | `pk.cfg` fopen → errno=2 (data dir, pre-loading) |
| 3872 | weapon loading starts (`weapdata/weaplist.wep`) |
| 3914–3966 | lists 15–30 `.weap` open, one `getspecific(key=13)` each |
| 3968–3969 | `weaplist_31.weap` opens |
| 3971–3972 | `weaplist_31.lua` opens (the only weapon `.lua` ever opened) |
| ~3973–3976 | 2× mutex lock/unlock on 0x98089678 — an address in the 0x98000000-based **C runtime** mapping (cf. GOT-patch lines 487–488: real `__cxa_throw` = 0x9806b18d), i.e. the C library's locale lock |
| ~3977–3997 | exactly 20× `pthread_getspecific(key=8)` — libc++ locale TLS traffic: a burst of locale-based conversions |
| 3998 | `THROW: boost::wrapexcept<boost::bad_lexical_cast>` |
| 3999 | backtrace ret0=0x99098877, ret1–ret5 = 0 |
| 4007 | uncaught-exception EXIT |

---

## 3. Q3 — The raw-token hypothesis: ruled IN, refined; cfg files ruled out

### 3.1 The cast consumes a script token — via `try_lexical_convert` in the weapscript TU

The throw-stub census (29 stubs call helper 0x98850) plus BFS from fn 0x18966c
leaves exactly **five** reachable stubs. Ranked by path length:

| Stub | Depth | Path (call sites) | Assessment |
|---|---|---|---|
| **0x1908b4** | 4 | 18966c →(0x189838) 189b38 →(0x18a5c4) 18a910 →(0x18b0f0) **0x18c000** →(0x18c01a) 0x1908b4 | **Prime suspect** — see 3.2 |
| 0x21eb38 | 5 | 189b38 →(0x18a0b8) 0xf5370 →(0xf5a8c) 0x21dc78 →(0x21e650) **0x21ea9c** →(0x21eab8) 0x21eb38 | Runner-up — luabind converter (3.3) |
| 0x229390 | 10 | via 0xb4728 → … → 0x3b9cec → 0x229308 | generic UI chain; implausible at load time |
| 0x2f5404 | 10 | same chain → 0x2f2890 | same |
| 0x347904 | 12 | same chain → 0x322150 → 0x323860 (ZML TU) | same |

### 3.2 Prime suspect: the `LOOP` command's argument

Fn **0x18c000** (52 B) is a textbook `lexical_cast<T>` wrapper: it builds the
`{const char* in, T out}` parameter block and calls 0x190888 → **0x1908e4**, whose
shape (strlen of input, stream extractor call to 0x99140, success flag returned)
is `boost::conversion::detail::try_lexical_convert<std::string, T>`; on `false`
it calls throw stub 0x1908b4 at 0x18c01a. It has exactly **one** call site in the
field parser: **0x18b0f0**, inside the block dispatched by the `memcmp` against
`"LOOP"` at 0x18aaca (branch at 0x18aad8 → block at 0x18b0cc). That block:

1. tokenizes the rest of the line (0x2884f4 @0x18b0e8),
2. **lexical_casts the token** (0x18c000 @0x18b0f0) — throw on failure,
3. builds a command object: type tag **18** stored at [obj+16], several
   context pointers, and a malloc'd int set from `atoi(token)` (@0x18b128).

So the leading mechanism is: **while parsing a weapon definition in
`weaplist_31.weap`, the script parser hits a `LOOP` command whose argument token
is empty or non-numeric for this conversion, `try_lexical_convert` reports
failure, and the wrapper throws `bad_lexical_cast`, which nothing catches.**
The ~20 locale-TLS hits before the THROW fit a run of stringstream-based
conversions (successful ones, then the failing one). Note the cast fires *after*
the `.lua` open purely because `LUA_FILE:` sits in the pack header, ahead of the
weapon sections in the file — the `.lua` open is a milepost, not necessarily a
participant.

Open sub-question this pass could not settle: why would an identical byte stream
parse on stock Android? Possibilities, in order: (a) the token is genuinely
different in list 31's script dialect (Lua-era packs) and the Android build's
parser is a *different build* of the engine that tolerates it — the Vita runs
this exact libengine, so this would mean the shipped Android APK's engine
differs, which is checkable against the APK's own libengine if another copy
exists; (b) the exception **is** thrown on Android too but is caught per-pack,
and our EH path fails to deliver it to the catch (see §5, step 0 — the loader
and field parser do carry full `__gxx_personality_v0` LSDA tables, so a catch
clause may exist); (c) our asset layer alters the token stream (e.g. read
chunking interacting with the game's de-obfuscation) — least likely given
`fread` passthrough, but the §5 input log settles it directly.

### 3.3 Runner-up: the luabind conversion path

Wrapper **0x21ea9c** (60 B) → converter **0x21eb68**, which calls luabind detail
functions (0x1cba9c / 0x1cbaf4), → throw stub 0x21eb38. It is reached from the
pack parser via one of the Lua-hook weapon classes (0xf5370 → 0x21dc78). This is
the "a Lua-side value converts badly" variant of the hypothesis: if list 31's
Lua file is *executed* during load (INIT_LUA hooks) and a luabind property
conversion receives a string that won't convert, the same exception results.
Caveat: luabind dispatch is heavily indirect (virtual/function-pointer calls),
so the direct-BL graph *understates* this path's reachability; it cannot be
ranked below the LOOP path on graph distance alone. §5's instrumentation
covers both.

### 3.4 The missing cfg files: ruled out

- **`pk.cfg`** — zero code references in libengine. The string exists only in a
  static filename table (@0x63ad0a, adjacent to the weaplist names). The game
  probes it with plain `fopen` from the app data dir (bootlog-2 lines 3706–3707,
  `ux0:data/pockettanks//pk.cfg`, errno=2) *before* weapon loading and simply
  proceeds. It feeds nothing.
- **`weapscript.cfg`** — exactly one reference, in fn 0x196c30 (@0x196cf2), the
  weapon-scripting settings path (same TU cluster as `weapscriptlog.txt`).
  Also fopen'd from the data dir (lines 3698–3699), also tolerated — 31 lists
  load afterwards. The APK shipping neither file is therefore expected
  behavior on stock Android too, not a Vita-side defect.

### 3.5 The date hypothesis: ruled out (for the record)

- `get_value<date>` dispatcher 0x97aec has exactly one caller: fn 0x941c8, inside
  boost::gregorian's own TU (its neighbors are
  `constrained_value<simple_exception_policy<unsigned short,1400,9999>, bad_year>`
  symbols). No game code calls it.
- Date fn 0xa546c is referenced only from a thunk at 0xa487c inside the ptree TU
  (immediately before `get_child`); parser 0xa57a0 is called only from 0xa546c.
- BFS from the weapon loader reaches none of 0x97aec / 0xa546c / 0xa57a0 /
  0x980f4 / 0x9863c / stubs 0x98530 / 0xa6974.
- `RELEASE_DATE:` in the pack header is stored as a string by fn 0x189b38, which
  contains no date or lexical_cast call at all.

---

## 4. What this changes vs. `bug29-diagnosis.md`

- "Weapon-script evaluator ~0x369d60 → get_child 0x369de2 → evaluator 0x36a10c →
  `get_value<date>` → `lexical_cast<short>`" — **withdrawn**. 0x369ce4/0x36a10c
  are the ZML loader; the date/short-cast cluster is unreachable from both the
  ZML loader's weapon-era namesakes and the actual weapon loader.
- "The patch's fake branch hands an empty tree to a date read" — **withdrawn**.
  The fake branch's only customer is fn 0x323a24 during zoom loading.
- "Policy v2 (caller whitelist)" — **moot for this crash**; do not implement.
  The Bug 27/28 trampoline itself is unaffected and should stay as-is.
- The Oct 6 coredump underpinning the old chain was the Bug 27 crash
  (PC 0x990942c6; `.psp2dmp` epoch = Oct 6 22:27 PDT), not this bug.

---

## 5. Q4 — Minimal next step: one surgical instrumentation build (no fix yet)

Three tiny hooks, same trampoline technique as the FAKE-DIAG build. Total
added logging fires rarely; nothing in the patch policy changes.

**Step 0 (offline, no build):** decode the LSDA action tables for callsites
0x189838 (in 0x18966c), 0x18a5c4 (in 0x189b38), and 0x18b0f0 (in 0x18a910) and
check for a catch clause covering `bad_lexical_cast` / `wrapexcept<…>` /
catch-all. If a catch exists on the path, the real bug is exception delivery
across our loader/libengine boundary (typeinfo matching), and the fix moves to
the loader's EH handling — skip to that. If no catch exists, proceed:

**Hook A — name the thrower (the discriminator).** At the shared throw helper
**0x98850** entry, log the incoming LR (return address into the calling stub)
plus 2–3 r7-frame return addresses, e.g.
`LEXCAST-THROW: stub_ret=0x%08x f1=0x%08x f2=0x%08x`.
Mapping: return into 0x1908b4 ⇒ LOOP-token cast (§3.2); into 0x21eb38 ⇒
luabind path (§3.3); into 0x98530/0xa6974 ⇒ date cast (would resurrect the old
theory); anything else ⇒ new lead, identified by the same table of 29 stubs.
(The existing `__cxa_throw` hook can't do this: by the time it runs, the return
address is inside the shared helper — bootlog-2 line 3999's zeroed backtrace is
the symptom.)

**Hook B — capture candidate A's input.** At wrapper **0x18c000** entry, log
`r0` as a C string: `LEXCAST-IN A: "%s"`. One call per LOOP command, so volume
is trivial. This prints the exact token the conversion chokes on.

**Hook C — capture candidate B's input.** At wrapper **0x21ea9c** entry, log
its string argument the same way (`LEXCAST-IN B: …`). Confirm the argument
register from its prologue when writing the trampoline.

**Reading the result:**
- A fires with an empty/garbled token ⇒ compare against the token stream for a
  pack that loads (the same log lines will appear for packs 15–30 if they
  contain LOOP commands; if they contain none, that itself is the difference).
  A follow-up fix would then be a one-site patch at 0x18c01a (skip the throw,
  treat failure as 0) — *only* after the token is known to be benign-shaped.
- B fires ⇒ the defect is in what list 31's Lua returns/registers; inspect
  `weaplist_31.lua` around the corresponding property.
- Neither A nor B logs but Hook A names a different stub ⇒ follow that stub's
  single caller (each stub has exactly one direct caller — the cast body).

**Not proposed:** policy v2; any change to the fake-ptree trampoline; more
cryptanalysis of the `.weap` obfuscation (Hook B prints the plaintext token
for free).

---

## Appendix — censuses used above

**Direct call sites of patched `get_child` 0xa4888 (30 sites / 26 functions):**
0xa44ae, 0xa4834 (wrapper 0xa482a), 0xb49ec, 0xd41be, 0xf2730, 0xf278c, 0x13d6be,
0x14f89a, 0x150130, 0x164520, 0x1e02a6, 0x1e0602, 0x1e69fa, 0x1e849a, 0x1e9276,
0x1e9808, 0x1ea6e2, 0x1eac78, 0x1eb0d2, 0x1ebbe4, 0x28ee0c, 0x2cf762, 0x2cf81e,
0x2cfba4 (fn 0x2cfba0), 0x2d05b0, 0x2d066c, **0x323ac0** (census caller),
**0x369de2** (ZML loader), 0x36c4bc, 0x371628.

**Throw stubs calling helper 0x98850 (29), each with its single direct caller:**
0x98530←0x980f4 (date translator F), 0x990c4←0x99010, 0x993cc←0x99044,
0xa6974←0xa6938 (date translator B), 0xb5eb0←0xb4558, 0xc0778←0xc0200,
0xf0af8←0xf0aa8, **0x1908b4←0x18c000** (LOOP cast), 0x1cba00←0x1c318c,
0x1cc4fc←0x1c48b0, 0x1cc64c←0x1c4ac4, 0x1ccdd4←0x1c4d44, 0x1dc5c0←0x1da540,
0x1ddc50←0x1dbb28, **0x21eb38←0x21ea9c** (luabind conv), 0x22664c←0x226620,
0x228484←0x22841c, 0x229390←0x229308, 0x229544←0x2294c4, 0x22dd9c←0x22dd14,
0x25ad9c←0x25851c, 0x2e10c8←0x2df818, 0x2e1184←0x2df842, 0x2e1240←0x2dfe48,
0x2e129c←0x2dfe84, 0x2f5404←0x2f2890, 0x31aaf8←0x314e48, 0x347904←0x3478c8,
0x4abc04←0x4abbc8.

**Method note:** call graph built from a raw Thumb BL/BLX-immediate scan of
`.text` (5,011,784 B) with callers attributed via the `.ARM.exidx` function map;
literal strings resolved via the `ldr [pc] / add rd, pc` PIC idiom. Indirect
(virtual / function-pointer) calls are invisible to this graph — noted where it
matters (§3.3).

---

## Addendum — §5 Step 0 result (2026-10-13, offline LSDA decode): A CATCH EXISTS. Instrumentation build NOT made; the bug is exception delivery.

The three callsites' LSDA action tables were decoded from `.ARM.exidx` /
`.ARM.extab` (hand-rolled prel31 + Itanium LSDA parser; entry addresses and the
personality pointer cross-checked against `readelf --unwind`, which agrees:
personality = PLT 0x7c270 = `__gxx_personality_v0` for both generic entries).

### Callsite 0x18b0f0 (fn 0x18a910, the LOOP → wrapper 0x18c000 call) — CATCH FOUND

- LSDA at extab+0x10 (0x5b8214): header lpstart=omit (offsets relative to fn
  start), tt_enc=0x00 with type entries as pcrel-indirect words, type-table
  base 0x5b825c, 9 callsites.
- The site covering the call (offsets 0x7e0 / ret−1 0x7e3) is
  **start=0x7de, len=0xc, landing pad = fn+0x96a, action=3**.
- Action chain for action=3: **[tt_index 1, cleanup]**. Type entry 1
  (@0x5b8258, raw 0x0014a154) points at GOT slot **0x7023ac**, which carries
  `R_ARM_GLOB_DAT` against **`_ZTIN5boost16bad_lexical_castE`** —
  `arm-vita-eabi-c++filt` demangles it as **"typeinfo for
  boost::bad_lexical_cast"** (the typeinfo object itself is defined in
  libengine at 0x6e58c4, .data.rel.ro). Type entry 2 is 0 (catch-all), used by
  the unrelated site at 0x9fc (chain [2]).
- Landing-pad corroboration (capstone @0x18b27a = fn+0x96a): `mov r5, r0;
  cmp r1, #1; bne …; blx <PLT>` (the `__cxa_begin_catch` pattern), after which
  the handler **rebuilds the same type-18 (LOOP) command object with default
  values** and rejoins the fall-through path — i.e. the game was designed to
  tolerate exactly this throw, treating a failed LOOP-token conversion as a
  default LOOP command.

### Callsite 0x18a5c4 (fn 0x189b38) — no catch in this frame

LSDA at extab+0xc (0x5b817c), tt_enc=0xff (no type table at all — no catch
clauses anywhere in this function; its nonzero landing pads are cleanup-only,
action=0). The site covering offset 0xa8c is start=0xa36, len=0x1b2,
**lp=0, action=0** — no landing pad. Moot anyway: the exception is caught one
frame below, in 0x18a910, before it can propagate here.

### Callsite 0x189838 (fn 0x18966c) — no catch in this frame

Compact personality-1 extab (@0x5b814c): header word + one opcode word + a
single 0x00000000 word — no exception descriptors, no landing pads in this
frame. Also moot, for the same reason.

### Conclusion

The `bad_lexical_cast` thrown by wrapper 0x18c000 at 0x18b0f0 **is supposed to
be caught** by fn 0x18a910's handler (the thrown `wrapexcept<bad_lexical_cast>`
matches `catch (bad_lexical_cast)` via its public base). On stock Android it
is; in the Vita run it terminates as uncaught (bootlog-2 lines 3998–4007).
Per §5's branch, the defect is therefore in **exception delivery / typeinfo
matching across the loader boundary**, not in the token, the data, or any
patch policy. Candidate loci for the follow-on pass (not decided here):
(a) our loader's handling of the `R_ARM_GLOB_DAT` typeinfo slot at runtime
0x997023ac — if it is not filled with libengine's own typeinfo address
(text_base+0x6e58c4), the personality's comparison can never match;
(b) the base-class walk from the thrown `wrapexcept<…>` typeinfo to
`bad_lexical_cast` (the wrapexcept typeinfo is a weak object in libengine,
0x6e59b8 area) failing under our loader's symbol resolution;
(c) LSDA base handling — this LSDA omits lpstart, so the unwinder must supply
the function start in the 0x99000000 mapping for the site lookup to land on
0x7de at all. The §5 hooks (A/B/C) were **not** built, per the Step 0 rule.


---

## Addendum 2 — EH forensics round 3 (2026-10-13): the discovery door IS Find_exidx; the failure is at/below the GOT slot or at frame 0. Fix NOT yet clear — instrumentation spec below, no build made.

**New runtime evidence (bootlog-4, banner build 2026-10-13-B):** banner present
(line 55), run still terminates with the same uncaught `bad_lexical_cast` after
weaplist_31, and there are **zero `EXIDX:` lines** in the whole log. The trace
wrapper logs (via blog(), budget 24) for any masked pc ≥ 0x99000000, so the
wrapper was never entered with a libengine pc during the entire run, including
the fatal unwind. FAKE-DIAG fired its usual 46 times through the same blog()
channel, so the channel itself is proven live.

### What round 3 established (static, from the extracted libc++_shared.so)

1. **Find_exidx is the ONLY discovery door in this build.** Full UND import
   list of libc++_shared.so (110 symbols) contains exactly one unwind-related
   entry: `__gnu_Unwind_Find_exidx`. `dl_iterate_phdr` is **not imported**;
   there is no `__register_frame_info`-style import; there is no `.eh_frame`
   section and the cursor's info path is EHABI-only. The dl_iterate_phdr and
   DWARF doors are ruled out completely — stop looking there.
2. **The 0x6e8bc call site is live, and the earlier pass's attribution was
   right.** Chain, all direct calls verified in disassembly:
   `__cxa_throw` (0x6b18c) → `_Unwind_RaiseException` (0x6df7c) → cursor-init
   (0x6e258, caller at 0x6dfae) → `setInfoBasedOnIPRegister` (0x6e4e8, call at
   0x6e290) → `findUnwindSectionsByPC` (0x6e8a4, call at 0x6e518) → `blx
   __gnu_Unwind_Find_exidx@plt` at **0x6e8bc** — the sole call to that PLT stub
   (0x3b2e0) in the entire .so. `findUnwindSectionsByPC` stores
   {table, count} verbatim into the info sections and returns
   `(table != 0) && (count != 0)`; on false, setInfo sets the cursor's
   invalid byte [this+0x180] and `step()` (0x6e446, whose first act is testing
   that byte) returns 0 — phase 1 ends as END_OF_STACK without ever querying a
   second frame. `getInfoFromEHABISection` (0x6e8e8) then does its own binary
   search (helper 0x6eacc) over the returned section; nothing else in the
   library consults any other table.
3. **Every loader-side link that feeds the slot verifies on paper:**
   - libc++_shared.so HAS PT_ARM_EXIDX [0x70fb8, +0x41c8), fully inside
     PT_LOAD#1 [0, 0x821a4); `_so_load`'s recording logic (pre-bias capture,
     per-segment containment, bias = text_base) yields exidx_base =
     0x98070fb8, size 0x41c8 for mod_cpp. The containment fix's impl is
     correct against this layout (verified by hand-simulation for the
     frame-0 pc 0x9806b1d4: module contains pc, binary search lands on
     __cxa_throw's own entry, count > 0).
   - The JUMP_SLOT is GOT offset **0x87fe0** (runtime 0x98087fe0); the PLT
     stub's address math (0x3b2e8 + 0x4c000 + 0xcf8) lands exactly there, and
     so_resolve writes `text_base + r_offset` = the same address.
   - main.c runs so_relocate + so_resolve on mod_cpp with the default_dynlib
     table that maps `"__gnu_Unwind_Find_exidx"` →
     `gnu_Unwind_Find_exidx_soloader` (dynlib.c:206); so_resolve_with_dummy
     is never called anywhere, so no dummy-clobber pass exists. bootlog-4
     shows the full relocate/resolve/initialize sequence completing OK for
     all three modules.
   - `__cxa_throw`'s own exidx entry is a normal compact-pr1 entry; its
     personality path inside libunwind is self-relative and does not depend
     on loader state beyond relocations already exonerated in round 2.
   - The loader's __cxa_throw hook is innocent: it logs and forwards to the
     real 0x9806b18d (noreturn). The fatal throw provably enters
     libc++_shared's runtime (the hook fires via libengine's patched GOT
     slot, 0x99702f00).

### The narrowed mechanism — two mutually exclusive runtime explanations

Phase 1 never obtained unwind info for any libengine frame. Either:

- **R1 — the GOT slot does not contain our wrapper at throw time.** The PLT
  call lands somewhere that yields 0 (or the call chain into it is not what
  the file shows), so `findUnwindSectionsByPC` fails at frame 0
  (pc 0x9806b1d4, inside __cxa_throw — a call the current trace cannot see
  because of its ≥0x99000000 gate), the cursor is marked invalid, and phase 1
  terminates immediately. This also retro-explains why NO build variant
  (pre-containment, containment, banner) ever changed the crash: the walk
  has never left frame 0 in any of them.
- **R2 — frame 0 succeeds, the walk dies at frame 0/1 inside libunwind**
  despite a correct Find_exidx answer (getInfo/personality/step layer).
  The binding is fine; the defect is in how this libunwind build consumes
  the sections under our memory layout.

The containment fix itself stands — it repairs a real defect (under the old
impl an engine-pc query would have been answered with libc++'s tail entry) —
but it sits downstream of the actual failure and may never have executed.

### Exact instrumentation for the next build (diagnostic only; no behavior change)

1. **Ungated Find_exidx trace.** In `gnu_Unwind_Find_exidx_soloader`, drop
   the `masked >= 0x99000000` gate: log the first 32 calls of ANY pc as
   `EXIDX2: pc=0x%08x -> table=0x%08x count=%d`.
2. **Load-time readback in main.c**, immediately after so_resolve(mod_cpp):
   `blog("EH-PROBE: cpp exidx_base=0x%08x size=0x%x GOT[Find_exidx]=0x%08x
   wrapper=0x%08x match=%d", mod_cpp.exidx_base, mod_cpp.exidx_size,
   *(uint32_t *)0x98087fe0, (uintptr_t)&gnu_Unwind_Find_exidx_soloader, …)`
   (declare the wrapper extern in main.c, as dynlib.c already does).
3. Reading the result:
   - Probe shows `match=0` → **R1 confirmed**; the logged slot value's
     identity (0, plt0_stub, __ret0, an eboot address) names the culprit
     pass directly. Fix is then in the resolve path for this one slot.
   - Probe shows `match=1` and EXIDX2 shows a frame-0 hit
     (pc=0x9806xxxx, table=0x9807xxxx, count>0) but no 0x99xxxxxx call →
     **R2 confirmed**; next probe moves inside libunwind (runtime-patch log
     points at setInfo's exits, fn 0x6e4e8 at runtime 0x9806e51c/0x9806e52e).
   - Probe shows `match=1` and EXIDX2 shows **no calls at all** → the PLT
     call never executes; the running unwinder is not the code disassembled
     here — re-open module identity (a second copy of the runtime).
