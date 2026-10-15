@ cxa_throw_stub.s -- Bug 29: unwind-transparent __cxa_throw/__cxa_rethrow hooks.
@
@ Why this exists: the old C hooks (bionic_compat.c) sat in a real EBOOT
@ frame between libengine's throw stubs and libc++_shared's real
@ __cxa_throw. The eboot is not a registered module, so
@ __gnu_Unwind_Find_exidx returns 0/0 for eboot PCs and libunwind phase 1
@ cannot step over the hook's frame -- it never reaches any game frame
@ and every engine throw terminates as "uncaught" (bootlog-5 EXIDX2:
@ two healthy libc++ lookups, then pc=0x81004ede -> table=0 count=0).
@
@ These stubs fix that by leaving NO frame behind: save r0-r2 + LR,
@ call the C logging helper, restore r0-r2 + LR + SP bit-identically,
@ then TAIL-BRANCH to the real __cxa_throw/__cxa_rethrow (the same
@ g_real_* pointers the old C hooks forwarded to). When the unwinder
@ runs, the call stack is exactly as if the game had called the real
@ __cxa_throw directly.
@
@ AAPCS stack alignment: entry SP is 8-byte aligned (public interface).
@ push {r0,r1,r2,lr} (16B) and push {r0,lr} (8B) both preserve that for
@ the bl into the C logger.

    .syntax unified
    .thumb
    .text

    .global __cxa_throw_soloader
    .thumb_func
    .type __cxa_throw_soloader, %function
__cxa_throw_soloader:
    push {r0, r1, r2, lr}         @ save args + caller LR; SP stays 8B-aligned
    ldr  r3, [sp, #12]            @ r3 = saved LR (return into game's throw stub)
    bl   cxa_throw_log_from_hook  @ (ex, tinfo, dest, caller_lr); clobbers r0-r3, ip, lr
    ldr  ip, .Lreal_throw         @ ip = &g_real_cxa_throw
    ldr  ip, [ip]                 @ ip = real __cxa_throw (Thumb address, bit0 set)
    pop  {r0, r1, r2, lr}         @ restore args, LR and SP exactly as at entry
    cmp  ip, #0
    beq  .Lhang_throw             @ NULL backstop: old C hook spun forever
    bx   ip                       @ TAIL branch -- no hook frame exists from here on
.Lhang_throw:
    b    .Lhang_throw
    .align 2
.Lreal_throw:
    .word g_real_cxa_throw

    .global __cxa_rethrow_soloader
    .thumb_func
    .type __cxa_rethrow_soloader, %function
__cxa_rethrow_soloader:
    push {r0, lr}                 @ 8B: SP stays 8B-aligned (r0 unused by rethrow)
    bl   cxa_rethrow_log_from_hook
    ldr  ip, .Lreal_rethrow       @ ip = &g_real_cxa_rethrow
    ldr  ip, [ip]                 @ ip = real __cxa_rethrow
    pop  {r0, lr}                 @ restore LR and SP exactly as at entry
    cmp  ip, #0
    beq  .Lhang_rethrow
    bx   ip                       @ TAIL branch
.Lhang_rethrow:
    b    .Lhang_rethrow
    .align 2
.Lreal_rethrow:
    .word g_real_cxa_rethrow
