# Bug 36 — the condition-variable timeout (Build V → W)

## Symptom (bootlog-25.txt, build V)
Build V's urandom shim worked (`URANDOM: /dev/urandom served from kernel RNG`,
line 558). One line later the SHOP scene switch is requested (line 559), and
the process dies on the spot (line 561, the log's last):

```
EXIT: bionic assert failed: .../libcxxabi/src/abort_message.cpp:72 (abort_message):
terminating with uncaught exception of type
boost::wrapexcept<boost::condition_error>:
boost::condition_variable::do_wait_until failed in pthread_cond_timedwait:
Connection timed out
```

The coredump's captured engine log shows the same order —
`[alog][BlitWise Engine] Switching to scene SHOP` → the assert — so the wait
fires synchronously during SHOP scene construction, on the engine's main
thread, inside the same heartbeat window as the switch (~frame 1792).

## Verdict (two stacked ABI mismatches in the loader's own pthread layer)
1. **Clock mismatch:** the engine creates every boost/asio condition variable
   with a `CLOCK_MONOTONIC` condattr and passes **monotonic** abstimes
   (~10² s), but the loader's `bionic_pthread_cond_timedwait` discards the
   attr and compares the abstime against **realtime** (~1.79e9 s) — the
   deadline reads ~1.79 billion seconds in the past, so the wait takes its
   timeout exit instantly, without ever blocking.
2. **Errno mismatch (the one that makes it fatal):** the timeout value the
   loader returns is the VitaSDK/newlib constant `ETIMEDOUT` = **116**, but
   the engine's boost compares the result against bionic `ETIMEDOUT` =
   **110**. 116 is not recognized as a timeout; it falls into
   `do_wait_until`'s generic error path, which throws `condition_error`.
   Uncaught at SHOP → abort. (The rendered text still says "Connection timed
   out" because newlib's own strerror table maps 116 to that string.)

Either defect alone breaks the wait; together they convert a routine timed
wait into a process abort. Both live in `source/bionic_pthread.c`.

---

## 1. The wait site (engine side)

### The callers of the imported `pthread_cond_timedwait@plt` (0x7d5b4)
Exactly two call sites in libengine.so (objdump census of the full .text):
`0x1cc2b0` and `0x4a95e4`. Both sit inside byte-similar compiled copies of
boost's `condition_variable::do_wait_until` (two boost TUs linked into the
engine; the 0x4a95xx copy's neighborhood is `boost::thread`'s join machinery —
`do_try_join_until_noexcept` is the nearest preceding export — so one copy
serves `condition_variable::wait_until`, the other the thread-join waits;
both share the identical defect exposure, and the fix is below both).

Anatomy of the 0x1cc274 copy (the 0x4a95a8 copy is instruction-equivalent):
- `0x1cc2aa–0x1cc2b0`: `pthread_cond_timedwait(cond, mutex, abstime)` with the
  abstime pointer carried in from the caller (a `boost::detail::mono_time_point`).
- The companion `now()` helper at `0x1cc238` does `movs r0, #1` →
  `clock_gettime(1, …)` — **bionic CLOCK_MONOTONIC** — i.e. the deadline
  family is boost::chrono::steady_clock / mono_time_point, as boost.Thread is
  designed: all `wait_until`/`wait_for` deadlines are normalized to monotonic
  time points, and the cond is expected to be a MONOTONIC cond.
- Post-call: `cbz res` → waited-ok; `cmp res, #110` (`0x6e`) → equal returns
  "timeout" (r0=0, **no throw**); anything else → `0x1cc2f4` builds a
  `condition_error(res, "boost::condition_variable::do_wait_until failed in
  pthread_cond_timedwait")` (the string at VA 0x63e83e, loaded via the literal
  at 0x1cc330) and throws. **110 is hardcoded** — bionic's ETIMEDOUT.

### The conds are created MONOTONIC — the attr is set, then thrown away by us
Both cond-constructor sites in the binary call
`pthread_condattr_setclock(attr, 1)` (`movs r1, #1` = bionic CLOCK_MONOTONIC)
immediately before `pthread_cond_init`:
- `0xc45d6–0xc45e2` — boost::condition_variable's own constructor (boost TU).
- `0x1cefba–0x1cefc6` — `boost::asio::detail::posix_event::posix_event()`
  (asio's scheduler/timer event cond, same pattern).

Loader side, `bionic_pthread_cond_init` does `(void)attr` — the clock is
**never stored** (`bcond_internal_t` has no clock field), even though
`bionic_pthread_condattr_setclock` dutifully records the id in the attr word.
POSIX semantics the engine was compiled against: a MONOTONIC cond interprets
abstime against CLOCK_MONOTONIC. Our implementation always uses realtime.

### Which object waits at SHOP
What the evidence pins down: a `boost::condition_variable` timed wait
(`wait_until`/`wait_for`, or a `thread::try_join_until` on the same
machinery), deadline in monotonic time, executed synchronously during SHOP
scene construction on the engine main thread (dump's engine log + bootlog
ordering; no frame heartbeat intervenes). In stock flow this is a bounded
wait for load/worker completion that either signals or times out harmlessly
(the caller treats a timeout return as a normal outcome — see the `cmp #110`
path). Static evidence does not distinguish which of the two do_wait_until
copies fired, and it doesn't need to: both receive the same wrong answer
from the same shim. The psp2core dump (standard container; THREAD_INFO /
THREAD_REG_INFO present) was scanned for the copies' return addresses in
stack memory — the abort had already unwound the throwing frame, so the dump
corroborates the sequence but does not further attribute the thread.

## 2. The loader's pthread/clock audit

### Bindings (dynlib.c)
Every cond import of the engine resolves to the loader's own bionic layer
(`source/bionic_pthread.c`), never to newlib:
`pthread_cond_init/destroy/wait/timedwait/signal/broadcast` →
`bionic_pthread_cond_*`; `pthread_condattr_init/destroy/setclock` →
`bionic_pthread_condattr_*`. `clock_gettime` → `clock_gettime_soloader`
(bionic_compat.c). `nanosleep` → raw newlib (relative only — see §3).

### The implementation (bionic_pthread.c, `bionic_pthread_cond_timedwait`)
```c
struct timespec now;
clock_gettime(CLOCK_REALTIME, &now);            // (a) loader-internal call
target_us = abstime…;  now_us = now…;
delta_us = target_us − now_us;
if (delta_us <= 0) return ETIMEDOUT;            // (b) SDK constant
… sceKernelWaitSema(..., &to); …
return r < 0 ? ETIMEDOUT : 0;                   // (b) again
```
- (a) This `clock_gettime` is **newlib's**, inside the loader binary, with
  SDK numbering (SDK `time.h`: CLOCK_REALTIME=1, CLOCK_MONOTONIC=4). Its
  realtime is the RTC-derived true Unix epoch — cross-check: this run's own
  dump filename epoch 1792003420 ≈ the run's wall time. So the comparison
  clock reads ≈ **1.79e9**.
- The engine's abstime, by contrast, comes from the engine-facing
  `clock_gettime_soloader(CLOCK_MONOTONIC=1)` = `sceKernelGetProcessTimeWide()`
  — process uptime. At SHOP entry (fast warm-cache boot, frame ~1792) that is
  on the order of **10² s**, plus a wait budget of seconds.
- (b) `ETIMEDOUT` here is whatever the loader was compiled with:
  VitaSDK `sys/errno.h` line 91: **`#define ETIMEDOUT 116`**. Bionic (and the
  engine's hardcoded compare) use **110**.

### Does anything honor a MONOTONIC condattr?
Not in this stack: newlib's own pthread is never called by the engine (dynlib
intercepts every cond symbol), and the loader's bionic layer — the only
implementation in play — stores the attr's clock id and then ignores it
(`(void)attr` in cond_init; no clock field in the cond object; timedwait
hardcodes realtime). The engine did everything POSIX asks; the floor under
it didn't.

## 3. The conversion verdict, with numbers
At SHOP entry, for the fatal wait:
- abstime (monotonic) ≈ uptime + budget ≈ **1×10² s**
- comparison "now" (realtime) ≈ **1.792×10⁹ s**
- `delta_us ≈ −1.79×10⁹ s` → `delta_us <= 0` → immediate return of
  **116** → engine sees `116 != 110` → **throw** → uncaught → abort.

Even the waits that "worked" before SHOP were silently degraded: any timed
wait in TITLE/MENU took the same instant-timeout exit; only SHOP's caller
lets the resulting exception escape (or is the first to use the throwing
path with an uncaught handler). Note the asymmetry that hid this for so
long: **untimed** `pthread_cond_wait` has no abstime and no errno return to
misread, so it has worked correctly all along.

### Where else the same mismatches could bite
- **Same function, other callers:** libc++_shared.so imports the same cond
  set (`pthread_cond_timedwait`, `clock_gettime`, …) — `std::condition_variable`
  timed waits ride the identical shim and are fixed by the same change.
- **Errno 116 hazard, general form:** any loader-compiled code that returns
  `ETIMEDOUT` to bionic-compiled callers. Census of what `bionic_pthread.c`
  actually returns: EPERM 1, ENOMEM 12, EBUSY 16, EINVAL 22 — all identical
  in newlib and bionic; ETIMEDOUT is the **only** divergent value it hands
  back. (EDEADLK also diverges — newlib 45 vs bionic 35 — but nothing in the
  pthread shim returns it; flagged here so a future timedlock doesn't
  reintroduce the class.)
- **Acquitted by import census (nm -D on libengine.so):** the engine does
  NOT import `sem_timedwait`, `pthread_mutex_timedlock`, `clock_nanosleep`,
  or `pthread_cond_timedwait_relative_np` — there is no other abstime-based
  pthread wait in the binary to convert. `nanosleep` is imported and bound
  raw to newlib — it takes a *relative* duration (no epoch), so it is
  unaffected by both defects.
- Adjacent latent class (not Bug 36, noted for the audit trail): loader libc
  shims that publish errno values to engine code via the `__errno` cell use
  newlib numbering; engine code comparing against bionic constants has the
  same 116/110-style exposure wherever the numberings diverge.

## 4. Fix spec (Build W) — `source/bionic_pthread.c` only, plus banner
1. **Return bionic's timeout code.** `#define BIONIC_ETIMEDOUT 110` and use
   it at both timedwait exits (the deadline-past pre-check and the
   `sceKernelWaitSema` timeout mapping). Never return the SDK's `ETIMEDOUT`
   (116) across the dynlib boundary. This alone converts the SHOP abort into
   a (still wrong, but survivable) instant timeout return — it is the
   load-bearing half of the fix.
2. **Honor the condattr clock.** Add `int clock_id;` to
   `bcond_internal_t`. `bionic_pthread_cond_init` reads the attr (NULL →
   0/CLOCK_REALTIME; the attr word already carries the bionic id stored by
   `bionic_pthread_condattr_setclock`) and passes it into `cond_ensure`,
   which stores it at creation. Conds materialized lazily without an init
   call keep the POSIX/bionic default, realtime.
3. **Compare like against like.** In `bionic_pthread_cond_timedwait`,
   replace the newlib `clock_gettime(CLOCK_REALTIME, &now)` with
   `clock_gettime_soloader(ic->clock_id, &now)` (declared in
   bionic_compat.c, same binary): monotonic family →
   `sceKernelGetProcessTimeWide()` (the exact source the engine built the
   abstime from), realtime → the RTC-tick epoch path. Deadline math and the
   semaphore wait are otherwise unchanged — the wait must genuinely block
   for its intended duration and wake on signal/broadcast as today.
4. **Diagnostics (first ~8 timedwaits, then silent):**
   `CONDWAIT: clock=%d abstime=%lld.%09ld now=%lld.%09ld delta_us=%lld rc=%d`
   so the next bootlog proves the new arithmetic at the first SHOP wait.
5. **Verification readout:** banner W; bootlog shows the URANDOM line at
   SHOP entry followed by CONDWAIT lines with `delta_us > 0` (and/or a
   signal-wake rc=0), **no EXIT**, frames advancing past the SHOP switch;
   Lola's screen is the verdict: the shop renders. Regression watch: timed
   waits elsewhere now block for their real durations (stock behavior) —
   no other build deltas.
