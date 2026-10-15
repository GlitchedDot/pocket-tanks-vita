# Bug 35 — /dev/urandom entropy shim (Build V)

## Symptom (bootlog-24.txt, build U)
At game start (~frame 10265, immediately after Lola's final setup taps and one
last touch at (519,479)), the process exits:

```
EXIT: bionic assert failed: .../libcxxabi/src/abort_message.cpp:72 (abort_message):
terminating with uncaught exception of type
boost::wrapexcept<boost::uuids::entropy_error>: open /dev/urandom
```

Menu, setup (Bug 34 keyboard bridge), and scene flow all work; the match
itself never begins because starting it constructs a boost::uuids random
generator, whose entropy source cannot open its device.

## Diagnosis — the exact open path
- Strings in libengine.so: `/dev/urandom` @ VA 0x63EFBA, `open /dev/urandom`
  @ VA 0x63EFCE (.rodata; in this ELF, VA == file offset).
- The entropy constructor (Thumb, ~0x2AD9B8) forms the path address with
  movw/movt (0xEFBA / 0x63), loads flags `0x8002` (O_RDWR | O_CLOEXEC-class),
  and calls the imported open-family PLT (bl 0x81018). It tests the returned
  fd: on `< 0` it loads the `open /dev/urandom` string (0x63EFCE) and calls
  the wrapexcept/entropy_error constructor PLT (bl 0x7DB2C), i.e. it throws.
  There is no fallback entropy path — open failure is fatal by design.
- The engine imports BOTH `open` and `__open_2` (the NDK _FORTIFY variant),
  plus `read`, `close`, `fopen`, `fread`, `fclose`. It does NOT import
  `openat`, `lseek`, `getrandom`, or `getentropy` (verified with nm -D).
- Loader side: dynlib.c binds `open` -> `open_soloader` and `__open_2` ->
  `__open_2`, both in source/bionic_compat.c. Both normalize the path
  (normalize_path_soloader leaves a leading-`/` path untouched) and call
  newlib `open()`. On the Vita there is no `/dev/urandom`, so newlib fails,
  the wrapper returns -1, and boost throws. This is the only break in the
  chain — a missing device, not a logic bug.
- `read` and `close` were still bound RAW to newlib in dynlib.c (lines 495 /
  252), while `fread`/`fclose`/`fdopen` already route through bionic_compat
  wrappers. The shim therefore has to (a) special-case the open entry points,
  and (b) give `read`/`close` wrappers so the fake fd is honored end to end.

## Fix (Build V) — source/bionic_compat.c + dynlib.c bindings + banner
Established pattern reused: special cases live in the bionic_compat file
layer, next to the path normalization every open already flows through.

- Virtual device: opens of exactly `/dev/urandom` or `/dev/random` (checked
  after normalization, in `open_soloader`, `__open_2`, and `fopen_soloader`)
  return a virtual handle instead of touching newlib's file layer.
  - fd form: sentinel fd 0x1000+slot (16 slots), far above newlib's real fd
    space. dynlib now binds `read` -> `read_soloader` and `close` ->
    `close_soloader` (pass-throughs for real fds); `__read_chk` also routes
    through `read_soloader`. Reads fill the FULL requested length from
    `sceKernelGetRandomNumber` (psp2/kernel/rng.h; SceLibRng inside
    libSceLibKernel_stub, already linked), chunked at 1024 bytes, with an
    LCG fallback so a read can never fail the game. No EOF, ever — it behaves
    like the character device it impersonates. `close` frees the slot and
    returns 0. `write` to the fd (write_soloader) is a harmless sink.
    `access()` on the two paths returns 0.
  - FILE* form (insurance for any stdio consumer): `fopen_soloader` returns
    an fmemopen-backed FILE registered in a small table; `fread_soloader`
    intercepts registered streams and fills from the same RNG (so the stream
    never EOFs either), `fclose_soloader` unregisters it. (fgetc/getc stay
    raw-bound; no known consumer reads this device through them.)
  - `lseek`: not intercepted — the engine never imports it.
- One bootlog line on the first device open:
  `URANDOM: /dev/urandom served from kernel RNG`.
- Everything else is build U verbatim (keyboard bridge, instruments,
  conditional swap, interposition code path, KeyEvent registration,
  seen-flag + UP-position input fixes). Banner:
  `EH-FIX BUILD 2026-10-13-V: urandom shim`.

## Verification expected in bootlog-25
Banner V; the URANDOM line at the first game start; NO
`entropy_error` EXIT; frames advancing past ~10265 into the match.
Lola's screen remains the verdict: the match should actually begin, and
weapon select (if reached during match setup) is the per-weapon unlock test.
