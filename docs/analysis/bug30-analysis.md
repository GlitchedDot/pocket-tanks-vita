# Bug 30 — the first caught exception dies in its own cleanup free

**Date:** 2026-10-13 · **Pass type:** forensics only, no build ·
**Evidence base:** `bootlog-6.txt` / `log-6.txt` (build 2026-10-13-D run),
`psp2core-1791932903-0x0000e039bd-eboot.bin.psp2dmp` (gzip'd ELF core;
epoch 1791932903 = 2026-10-13 16:08:23 PDT — this run, verified),
loader source in `pockettanks-vita/source/`, `libc++_shared.so` and the
build-D eboot ELF for disassembly/symbols.

---

## TL;DR — verdict

**Bug 29's fix works completely. Bug 30 is an allocator-pair mismatch in
our own shim layer, exposed by the first successfully caught exception in
the port's history.**

`__cxa_allocate_exception` (in `libc++_shared.so`) allocates the exception
storage through the **`memalign`** import, which our dynlib table binds to
`memalign_soloader` — a homebrew aligner whose layout stores the raw
`malloc` pointer in the word just before the aligned payload.
`__cxa_free_exception` releases the same storage through the plain
**`free`** import → `free_soloader` → newlib `free()`, which expects a
dlmalloc size word in exactly that slot. It reads the stored raw pointer
(`0x831a5590`) as a chunk size, computes a "next chunk" at `0x0634ab28`,
and takes a genuine CPU data abort inside newlib `_free_r` while trying to
link it. The game, the catch handler, phase-2 restoration, and the
exception itself are all **exonerated by dump bytes** — the handler ran to
completion; the crash is in the ABI cleanup *after* it.

On stock Android this cannot happen: bionic's `memalign` returns pointers
that plain `free()` accepts by design. Our `memalign_soloader` never
provided that invariant (its source even notes "free() on the result
leaks; acceptable for game use" — written when nothing in the system
deterministically freed aligned allocations; libc++abi does).

**Fix (certain, one binding):** point the dynlib table's `memalign` entry
at newlib's own `memalign` (verified present in the VitaSDK libc:
`T memalign`, `_memalign_r`, `aligned_alloc`). dlmalloc `memalign` chunks
carry a valid header at `ptr-8` and are free()-able — the exact invariant
`__cxa_free_exception` relies on. See §5.

---

## 1. The fault (dump: thread, PC, nature)

**Thread:** the main game thread — `THREAD_REG_INFO` entry 0 (the thread
that ran the entire boot; its UID `0xe039bd` is in the dump filename and
`PROCESS_INFO`). The other three threads (SceCommonDialogWorker, vitaGL
Garbage Collector, SceGxmDisplayQueue) are parked at kernel-wait
addresses with `deadbeef` filler — untouched.

**PC / instruction:** `PC = 0x8108e6b8`, inside newlib **`_free_r + 0xd0`**,
instruction **`str r3, [r2, r3]`** — a store through `r2 + r3`.

**Registers (faulting thread, from THREAD_REG_INFO entry 0)** — the
dlmalloc free() fingerprint, all three key values present simultaneously:

| value | meaning |
|---|---|
| `0x831a55a0` | `mem` — the pointer passed to `free()` |
| `0x831a5598` | `mem2chunk(mem)` = mem − 8 |
| `0x831a5590` | the bogus "size" read from the size slot (mem − 4) |
| `0x0634ab28` | computed next-chunk address = `0x831a5598 + 0x831a5590` (mod 2³²) — the wild store target |

**Nature of the death — NOT the checker's doing, in two senses:**

1. `free_soloader` has **no fatal path**. Source (`bionic_compat.c`): on a
   bad header it only `blog()`s the tripwire line and then *unconditionally
   calls `free(mem)` anyway*. The bootlog's last line is the tripwire; no
   `EXIT:` line follows because the process never regained control.
2. The CPU fault is two frames below the checker, inside dlmalloc's
   consolidation/bin-link store in `_free_r`, storing through the wild
   address derived from the fake size. This is a genuine data abort and a
   genuine core dump — not a controlled stop.

**Call chain (stack return addresses + symbol-verified):**

```
parser catch handler (libengine fn 0x18a910, cleanup call site → ret 0x9918b2cb)
  → __cxa_end_catch            (libc++_shared 0x9806b335)
  → __cxa_free_exception       (libc++_shared 0x9806b15b; subs r0,#128; bl helper → ret 0x9806b165 on stack)
  → free@plt → free_soloader   (logs the HEAP tripwire)
  → newlib free → _free_r      (PC 0x8108e6b8 — data abort)
```

The two `pthread_getspecific(key=18)` calls between the last phase-2
EXIDX2 line and the HEAP line are `__cxa_begin_catch` / `__cxa_end_catch`
touching the EH globals — normal. The handler body itself (the LOOP
command rebuild) completed: control reached the handler's end-catch call,
which is where the fatal free hangs.

---

## 2. The chunk at 0x831a55a0 — it IS the exception allocation

Dump memory (PT_LOAD segment covering `0x831a4000–0x831a6000`), decoded
against `libc++_shared` disassembly:

**Allocator arithmetic in `__cxa_allocate_exception`** (0x6b130):
request = `(thrown_size + 135) & ~7` (i.e. object + 128-byte header,
rounded); returns `base + 128`. **`__cxa_free_exception`** (0x6b15a):
`free(object − 128)`.

**The memory, word by word** (base = aligned payload `0x831a55a0`):

| address | value | identity |
|---|---|---|
| `0x831a5588` | `0x4d` | raw chunk prev_size field |
| `0x831a558c` | `0xc1` | **raw chunk size = 0xc0 \| PREV_INUSE — sane dlmalloc header** |
| `0x831a5590` | `0x81102404` | raw chunk payload begins (= the pointer stored at `0x831a559c`) |
| `0x831a559c` | **`0x831a5590`** | **`memalign_soloader` bookkeeping: raw ptr at aligned−4 — the word `free_soloader` misread as a size** |
| `0x831a55a0` | `0` | __cxa_exception header +0 |
| `0x831a55a4` | `0x996e5910` | **exceptionType = wrapexcept<bad_lexical_cast> TI** (matches `__cxa_throw`'s `strd [r4,#4]` store layout) |
| `0x831a55a8` | `0x99098915` | exceptionDestructor (libengine) |
| `0x831a55ac/b0` | `0x98064321 / 0x98064245` | unexpected/terminate handlers (libc++) |
| `0x831a55bc` | `1` | handlerSwitchValue = catch clause 1 (the LOOP catch) |
| `0x831a55c8…` | `"…CLNG" class bytes` | _Unwind_Exception exception_class |
| `0x831a55d0` | `0x9806b1e9` | exception_cleanup fn |
| `0x831a55ec` | `0x831a5620` | adjustedPtr → the thrown object |
| `0x831a55f0` | `0x995b824d` | actionRecord (parser LSDA +1) |
| `0x831a55f4` | `0x995b8214` | languageSpecificData = parser fn LSDA (census value ✓) |
| `0x831a55f8` | `0x9918b27a` | catchTemp = the landing pad itself |
| `0x831a5620` | `0x996e5960` | **thrown object: wrapexcept vtable** (+ following string members) |

**Size cross-check (unique fit):** wrapexcept object = 40 bytes →
allocate request `(40+135)&~7 = 168` → `memalign_soloader(16, 168)` does
`malloc(168+16+4 = 188)` → dlmalloc chunk `0xc0` ✓ (the `0xc1` word) →
`aligned = align_up(raw+4, 16) = 0x831a55a0` ✓ → header at aligned,
object at aligned+0x80 = `0x831a5620` ✓ → `free_exception` frees
`0x831a5620 − 0x80 = 0x831a55a0` ✓ = the logged pointer. Every observed
number is accounted for; there are no free parameters left.

**When:** allocated on the main thread during `weaplist_31.weap` parsing,
microseconds before bootlog-6 line 4001 (the THROW line) — the throw stub
at wrapper `0x18c000` calls allocate → construct → `__cxa_throw`. It is
not the rebuilt LOOP command object (different allocation, never the one
freed here) and not an unrelated game allocation.

---

## 3. How malloc/free actually bind (the boundary, mapped)

| module | `malloc` | `free` | `memalign` |
|---|---|---|---|
| libengine | `malloc_soloader` | `free_soloader` | — (no UND import) |
| libc++_shared | `malloc_soloader` | `free_soloader` | **`memalign_soloader`** (UND `memalign@LIBC`) |
| eboot/newlib | dlmalloc underneath all the soloader wrappers | | |

So there is only **one** real heap (newlib dlmalloc) — but **two chunk
layouts** reach it: plain `malloc` chunks (header at −8/−4, free-able) and
`memalign_soloader` chunks (raw-pointer word at −4, **not** free-able).
The exception allocator is the one consumer that allocates through the
second layout and frees through the first. (Also in the table: `calloc`
and `realloc` bind straight to newlib — same heap, valid headers, not
implicated.)

Why it never fired before build D: every previous throw died in phase 1
(Bug 29) and never reached `__cxa_end_catch`/`__cxa_free_exception`.
Build D's run is the first time any exception in this port has ever been
caught *and cleaned up*.

---

## 4. Mechanism ranking

**(i) Allocator-boundary mixing — CONFIRMED (root cause).** As mapped in
§3 and proven byte-for-byte in §2: aligned-alloc via `memalign_soloader`,
plain-free via `free_soloader`/dlmalloc. The "corrupt header" is not
corruption at all — it is `memalign_soloader`'s own bookkeeping word,
read by a consumer that (correctly, for its own layout) expected a size.

**(ii) Phase-2 restoration damage — RULED OUT.** Everything phase 2 was
supposed to do, it did right: EXIDX2's second pass walks the correct
frames in order (RaiseException → __cxa_throw → throw helper →
try_lexical_convert frames `0x991908da/ce` → wrapper `0x9918c01c` →
parser `0x9918b0f2`); the exception header in the dump records the
correct handlerSwitchValue (1), actionRecord, LSDA pointer, landing pad
(`catchTemp = 0x9918b27a`), and adjustedPtr (the intact object, vtable
unclobbered at `0x831a5620`); `__cxa_free_exception` received the right
object and computed the right base. The single wrong value in the whole
sequence is the size word — which was never written by the unwinder; it
was wrong from the moment of allocation.

**(iii) Genuine game-side overflow — RULED OUT.** No splatter anywhere:
the word in the size slot is a structurally exact pointer (to the raw
chunk payload, whose own dlmalloc header is sane), the surrounding heap
is coherent exception machinery end to end, and the game code between
throw and free is just the catch handler, which completed. The
throw-time `heap-corrupt=no` is weak evidence by itself (that hook only
scans the tracked heap for the `0x0c0c0c0c` pattern) — the dump bytes are
what close this door. Note the checker's "(possible buffer overflow)"
hint text misdiagnoses this case.

---

## 5. The fix (certain and small — described, not built, per instructions)

**Change one dynlib binding** (`pockettanks-vita/source/dynlib.c`):

```c
{ "memalign", (uintptr_t)&memalign_soloader },
```
→ bind to newlib's own **`memalign`** (declare `void *memalign(size_t,
size_t);` from `<malloc.h>` if not already declared). Verified present in
the SDK's `libc.a` (`T memalign`, `T _memalign_r`, `T aligned_alloc`).

dlmalloc's `memalign` builds a real chunk whose header sits at `ptr−8`
with the offset encoded in `prev_size`, precisely so that `free(ptr)`
works — the invariant `__cxa_free_exception` (and any aligned-new/delete
pair in libc++) relies on. This also fixes the same latent hazard for any
other aligned allocation libc++ makes, not just exceptions.

Deliberately **not** proposed:
- Teaching `free_soloader` to sniff the legacy layout — heuristic pointer
  validation in a hot free path, unnecessary once no producer emits that
  layout (all allocations in a run come from the same binding).
- Making the tripwire abort before `free()` — it would only convert this
  crash into a tidier crash; the chunk would still be unfreeable.

**Success signature on the next run:** banner D (or its successor)
bootlog shows the weaplist_31 `THROW:` line, the EXIDX2 catch walk, the
two `pthread_getspecific(key=18)` calls — and then **no HEAP line**;
weapon loading continues through `weaplist_32`…`weaplist_41` and beyond.
That run would also be the first time the weapon-unlock stubs behind the
weapon phase become reachable.

---

## Appendix — evidence index

- bootlog-6.txt: banner line 55; THROW line 4001 (`heap-corrupt=no`);
  phase-1 + phase-2 EXIDX2 walks lines ~4005–4030; final line 4035 =
  the HEAP tripwire (no EXIT after it).
- Dump: `THREAD_REG_INFO` entry 0 (crashed main thread; chunk/mem/size/
  wild-address registers); `PROCESS_INFO` (UID 0xe039bd, PCKT00001);
  PT_LOAD `0x831a4000+0x2000` (the exception chunk, §2 table); stack at
  `0x8137d0d0` (return addresses `0x9806b165`, `0x9918b2cb`).
- Disassembly: `__cxa_allocate_exception` 0x6b130 (`adds r0,#135`,
  returns base+128), `__cxa_free_exception` 0x6b15a (`subs r0,#128`,
  return-after-call `0x6b164`), `__cxa_throw` 0x6b18c (header store
  offsets matching §2), newlib `_free_r` (fault at +0xd0,
  `str r3,[r2,r3]`).
- Source: `bionic_compat.c` `free_soloader` (tripwire logs then frees
  anyway), `memalign_soloader` (raw-pointer-at−4 layout; "free() leaks"
  comment), `malloc_soloader`; `dynlib.c` binding table (malloc/free/
  memalign → soloader wrappers; calloc/realloc → newlib direct).
- Symbols: libc++_shared UND `memalign@LIBC`, `free@LIBC`, `malloc@LIBC`;
  libengine has no memalign import. SDK `libc.a` exports `memalign`.
