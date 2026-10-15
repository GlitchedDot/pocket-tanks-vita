# Bug 29 diagnosis — bad_lexical_cast after weaplist_31.lua, Bug 27/28 patch applied

**Date:** 2026-10-13 · **Pass type:** diagnosis only (no VPK rebuilt/shipped)
**Evidence:** `bootlog.txt` + `log.txt` + `psp2core-…-eboot.bin.psp2dmp` from Lola's
2026-10-13 run of the weapon-unlock VPK; disassembly of the extracted
`libengine.so` (7,393,972 B — matches the bootlog datadir size, same build);
loader source `pockettanks-vita/source/main.c` + `ptree_fake.cpp`;
Bug 27/28 history in `~/memory/2026-10-07.md`.

## TL;DR

Bug 29 is **Bug 28's crash, not fixed** — not a new bug. The Bug 28 trampoline
policy ("fake empty ptree iff the missing node's path contains `<xmlattr>`,
original throw otherwise") rests on a false premise: that `<xmlattr>` reads are
always string/iteration reads. The weapon-data code reads a **date-typed
attribute through an `<xmlattr>` path** with `ptree::get<date>(path, default)`.
The node is missing, the path *does* contain `<xmlattr>`, so the trampoline
deliberately returns the fake empty ptree; `get_value<date>` then runs
`lexical_cast<date>("")`, whose inner `lexical_cast<short>` on the empty string
throws `boost::bad_lexical_cast` — which `ptree::get` does **not** catch (it
catches only `ptree_bad_path`) — and the process terminates. The Bug 28 analysis
confirmed the `get<date>` → `lexical_cast<date>("")` chain but never verified
the path string; today's coredump supplies that missing fact: the path being
processed at crash time contained an `<xmlattr>` segment.

## Q1 — Which function throws, on what value, via what chain?

**Throw site.** Bootlog `ret0=0x99098877` = return address after
`blx __cxa_throw@plt` at libengine **0x98876**, inside the shared
`wrapexcept<bad_lexical_cast>` throw helper at **0x98850** (allocates the
36-byte exception, constructs it via the ctor at 0x988ac, throws). The helper
has **29 callers** in the binary, so ret0 alone never identified the caller —
that requires the chain below.

**The cast machinery.** `lexical_cast<short>` exists in two instantiations
(two TUs), both bottoming out in one shared core converter:

- Cluster A: `lexical_cast<short>(string)` **0x98560** → signed parse 0x985d0 →
  core unsigned-short converter **0x9863c** → digit parser 0x987d0. Translator
  `get_value<short>` "F" at **0x980f4** wraps it (on failure calls the cluster's
  throw function 0x98530 → helper 0x98850).
- Cluster B (ptree neighborhood): twin translator 0xa6938 → twin cast 0xa69a4 →
  calls the **same core 0x9863c**.

The core converter 0x9863c builds a `std::locale` and installs a numpunct
facet — which is exactly the **~20 `pthread_getspecific(key=8)` calls plus the
libc++ mutex lock/unlock pairs** visible in the bootlog immediately before the
THROW (locale/facet TLS cache traffic). That bootlog signature *is* the
converter starting up.

**The date parsers.** The value being converted is a **date**. All three boost
gregorian parse flavors are present and all funnel date components through
`lexical_cast<short>`:

- `gregorian::from_delimited_string` / parse_date **0xa57a0** — splits one
  string into 3 tokens; token 0 → `constrained_value<1400..9999, bad_year>`,
  token 1 = month, token 2 → `constrained_value<1..31, bad_day_of_month>`,
  then the `gregorian::date` ctor. Its token conversions call the translator
  at **0xa5828**.
- A second date/time parser at **0xa58f0** (twin-translator calls 0xa59c6 /
  0xa59d0, `substr(0,6)` logic).
- A format-driven (`%d/%m/%y`) parser near **0x97c00** and a posix_time parser
  near **0x98e00**, both calling translator F (0x97cda / 0x97ce4 / 0x98ed6 /
  0x98ee0).

The exact sub-flavor isn't pinned down and doesn't matter: every flavor's
component parse ends in the same `lexical_cast<short>` core, and the failure
type is `bad_lexical_cast` in all of them.

**The value: the empty string `""`.** Established two ways: (1) Bug 28's
disassembly (2026-10-06) already confirmed this exact THROW is
`get<date>(path, default)` → `get_value<date>` → `lexical_cast<date>("")` →
inner `lexical_cast<ushort>("")`; (2) the only source of `""` in this flow is
the fake ptree — `ptree_fake.cpp` default-constructs its value
(`p->m_data = game_string(); // all zeros = valid empty string`).

**Call chain (static + coredump).** The psp2dmp was gunzipped and parsed
(THREAD_REG_INFO descriptors; game thread `thid 0x40010003`,
SP=0x8137ffa0). Code pointers recovered from its stack, cross-checked against
static disassembly:

1. Weapon-script evaluator, game function around **0x369d60**: builds a dotted
   path (string op 0x9d470 with separator `'.'` = 46), then at **0x369de2**:
   `bl get_child` (0xa4888) — path in r1, ptree in r0.
2. Result saved and passed at 0x369df2 into evaluator function **0x36a10c**
   (which reads the child's value via 0xa9ca8 and converts values — it converts
   a sibling child to double and compares against 1.0; the date read happens
   in its subtree, frames at **+0x36b0d9** in the dump).
3. `get_value<date>` → date parser → `lexical_cast<short>("")` → throw helper
   0x98850 → `__cxa_throw` → uncaught → libc++abi abort (bootlog EXIT).
   A date-cluster frame (+0x97a6d) is also present on the crashed stack.

## Q2 — Does it route through the patched get_child? Why doesn't the policy save it?

**Yes — three independent proofs:**

1. **Found-door return address on the crashed stack.** Stack slot
   `[0x813802ec] = 0x990a48bb` = libengine+**0xa48bb**, the return address of
   the string-destructor call at 0xa48b6 inside get_child's **normal exit**
   (0xa48b4). get_child returned a ptree through the door the trampoline's
   FAKE and FOUND outcomes both use. It did **not** take the genuine-throw
   exit at 0xa48d0.
2. **`"<xmlattr>"` path segments live on the stack.** Multiple live
   `std::string` objects in the crashed thread's stack contain exactly
   `<xmlattr>` (9 chars, SSO layout verified byte-for-byte at 0x8137ff90,
   0x81380135, …) — the path segments being walked when the process died.
3. **The exception type itself.** The throw branch produces
   `ptree_bad_path`, which `ptree::get(path, default)` catches. The observed
   exception is `bad_lexical_cast`, which can only arise *after* a ptree was
   successfully returned and its value converted. A genuine not-found cannot
   produce this crash.

**Why the policy fails.** The trampoline's rule — "path contains `<xmlattr>`
⇒ the caller wants an empty node to iterate" — is true for the Bug 27 zoom
case but false in general. This caller is the Bug 28 pattern (`get<date>` with
a default, expecting `ptree_bad_path` ⇒ default) whose path happens to contain
`<xmlattr>`. Path content cannot distinguish "iterate my attributes" from
"convert my value to a date"; the Bug 28 fix sorted the two Bug-28-era cases
by path and got a rule that misclassifies this one.

**Not a mechanics failure.** The Bug 28 register assumption is correct:
`mov r8, r1` at 0xa4898 sets r8 = path before the lookup, and the trampoline
demonstrably works (zoom config loads; the found-door address above proves
get_child executed its normal return in this flow). The failure is purely the
policy's scope.

**Alternatives considered and excluded:**

- *Present-but-empty real node:* a real child whose value is `""`/garbage
  would crash the stock Android build identically — this is shipping game
  data that loads fine on Android. The fake ptree is the only
  port-specific source of an empty value here.
- *Missing `weapscript.cfg` / `pk.cfg` (fopen errno=2, bootlog 3651/3659):*
  red herring. The APK ships **neither** file — they are runtime-created on
  Android too, so their absence is the game's normal first-run state; and
  the queried ptree in the crash chain is built by the weapon-script
  evaluator from the list data, not from those cfgs.
- *Weapon data / Lua file values:* `weaplist_31.lua` is the only weapon-list
  script opened, and the lists 1–30 load fine through the same code — the
  difference is this list's script querying a date attribute its own tree
  doesn't define (on Android: `ptree_bad_path` → default → no crash).

## Q3 — Minimal fix proposal

**Policy v2 for the trampoline: discriminate by CALLER, not by path.**
At the patch site the caller's return address is recoverable: get_child's
prologue (`push {r4,r5,r6,r7,lr}; add r7, sp, #12`) leaves **saved LR at
`[r7, #4]`**, and r7 is live and untouched at 0xa48b0 (the live LR register is
useless — it was overwritten by get_child's inner calls; the frame slot is
the reliable copy).

- Not-found + saved LR ∈ whitelist ⇒ return the fake ptree (normal exit).
- Not-found + any other caller ⇒ original throw at 0xa48d0 (stock semantics;
  `get<T>(path, default)` gets its `ptree_bad_path` and returns the default).
- The `<xmlattr>` path scan can be dropped entirely.

Whitelist starts with the Bug 27 zoom-config direct-call site. To populate it
with certainty, ship **one diagnostic build first**: same policy as today, but
the trampoline logs path + saved LR on every fake return (and/or on every
not-found). One run enumerates every site that asks for a missing node; the
zoom site fires early during config load, the weapon date site
(saved LR = text_base+**0x369de7**, the return after `bl get_child` at
0x369de2) fires at weaplist_31 — the whitelist is then just "the sites that
are direct `get_child` calls whose callers dereference the result", with the
weapon site deliberately left OUT so it throws.

Assembler ground truth for the new read (verified with `arm-vita-eabi-as` +
objdump this pass, per standing lesson): `ldr r3, [r7, #4]` = **0x687B**;
`cmp r3, r2` = 0x4293; the absolute-address compare can reuse the trampoline's
existing movw/movt style (saved LR carries the Thumb bit — compare against
`text_base + offset | 1`).

**Confidence: ~85%** this gets the game past weaplist_31. Basis: the chain
and the `""` input are confirmed (Bug 28) plus proven `<xmlattr>` routing
(today); `get<date>(path, default)` by construction catches `ptree_bad_path`.
Residual risk: if some *other* direct `get_child` caller was silently relying
on the fake for a non-`<xmlattr>` or `<xmlattr>` node, it will now throw —
but that failure is loud and self-identifying (see below), and the whitelist
grows by exactly one verified site per occurrence instead of corrupting typed
gets silently.

**Fallback if the weapon site turns out to have no `ptree_bad_path` handler**
(i.e. it is a raw `get_child` + manual `get_value` with no default-catch):
whitelist that site for the fake as today, and instead make the fake's value
parseable — set `ptree_fake`'s `m_data` to a valid date string. Attribute
*iteration* (the Bug 27 need) only touches the fake's children (none), never
its value; the value is only read by `get_value` on the node itself. This is
uglier (a `get<string>` on the node would return the date string instead of
`""`) so it's the fallback, not the plan.

**Next-run bootlog evidence that confirms the fix:**

1. Line ~53 still shows the patch APPLIED (new policy string).
2. **No `THROW: N5boost10wrapexceptINS_16bad_lexical_castEEE` anywhere**, and
   weapon loading continues **past** `weaplist_31.lua` into whatever follows
   (subsequent stage logs / menu), instead of EXIT.
3. If the diagnostic build runs first: fake-return log lines show the zoom
   site early and the weapon site at +0x369de7 — after policy v2, the weapon
   site's line must disappear (it takes the throw branch) while the zoom
   site's remains.
4. A *different* failure signature — `THROW` of `ptree_bad_path` terminating
   the process right after weaplist_31 — would mean the no-handler fallback
   case above; that outcome is itself decisive evidence and routes to the
   fallback fix, not back to path-based policy.

## Appendix — key addresses (libengine file offsets; runtime = text_base + off)

| What | Offset |
|---|---|
| `__cxa_throw` call in bad_lexical_cast throw helper | 0x98876 (helper 0x98850, 29 callers) |
| `lexical_cast<short>` cluster A / core converter / translator F | 0x98560 / 0x9863c / 0x980f4 |
| Twin translator cluster B | 0xa6938 / 0xa69a4 / 0xa6974 |
| gregorian delimited date parser | 0xa57a0 (translator call 0xa5828) |
| get_child (throwing) / patch site / normal exit / throw exit | 0xa4888 / 0xa48b0 / 0xa48b4 / 0xa48d0 |
| r8 = path set | 0xa4898 (`mov r8, r1`) |
| Weapon evaluator: `bl get_child` / evaluator fn | 0x369de2 / 0x36a10c |
| Coredump: game thread SP · fake-string evidence · found-door slot | 0x8137ffa0 · 0x8137ff90 · [0x813802ec]=+0xa48bb |
