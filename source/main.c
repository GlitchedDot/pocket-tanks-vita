// Pocket Tanks (com.blitwise.ptankshd) PS Vita port — loader entry point.
#include "utils.h"
#include "bootlog.h"
#include "egl_graphics.h"
#include "input.h"

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <kubridge.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_PATH "ux0:data/pockettanks/"

/* from bionic_pthread.c (no header; defined there) */
int bionic_pthread_setspecific(int key, const void *value);
/* from so_util (not in header; defined in lib/so_util/so_util.c) */
uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);
#define LOAD_ADDR_CPP    0x98000000
#define LOAD_ADDR_FMOD   0x98200000
#define LOAD_ADDR_ENGINE 0x99000000

/* Bug 27: encode a Thumb-2 b.w (unconditional wide branch). */
static void encode_bw(uint16_t *out, uintptr_t from, uintptr_t to) {
    int32_t offset = (int32_t)to - (int32_t)(from + 4);
    uint32_t u = (uint32_t)offset;
    int S = (u >> 24) & 1;
    int I1 = (u >> 23) & 1;
    int I2 = (u >> 22) & 1;
    int imm10 = (u >> 12) & 0x3FF;
    int imm11 = (u >> 1) & 0x7FF;
    int J1 = !(I1 ^ S);
    int J2 = !(I2 ^ S);
    out[0] = (uint16_t)(0xF000 | (S << 10) | imm10);
    out[1] = (uint16_t)(0x9000 | (J1 << 13) | (1 << 12) | (J2 << 11) | imm11);
}

/* Bug 28: encode 16-bit Thumb conditional branch (T1).
 * cond: 0=eq, 1=ne, 2=cs/hs, 3=cc/lo. from/to are byte addresses.
 * Verified against arm-vita-eabi-as: bcs.n -> 0xD2xx, bcc/blo.n -> 0xD3xx,
 * bne.n -> 0xD1xx, beq.n -> 0xD0xx, with imm8 = (to-(from+4))>>1
 * (halfword-scaled; e.g. bcs.n from 8 to 0x10 -> 0xD202). */
static void encode_b_cond(uint16_t *out, uintptr_t from, uintptr_t to, int cond) {
    int32_t imm8 = ((int32_t)to - (int32_t)(from + 4)) >> 1;
    out[0] = (uint16_t)(0xD000 | ((cond & 0xF) << 8) | (imm8 & 0xFF));
}

/* Bug 28: encode 16-bit Thumb unconditional branch (T1, b.n).
 * Verified: b.n +2 from 0 -> 0xE001 (imm11 = (to-(from+4))>>1). */
static void encode_b_n(uint16_t *out, uintptr_t from, uintptr_t to) {
    int32_t imm11 = ((int32_t)to - (int32_t)(from + 4)) >> 1;
    out[0] = (uint16_t)(0xE000 | (imm11 & 0x7FF));
}

/* Bug 28: fixed 16-bit Thumb data-processing encodings used by the
 * get_child path-check trampoline. Every halfword verified against
 * arm-vita-eabi-as ground truth (see /tmp/enctest/t.s disassembly):
 *   mov r3, r8        -> 0x4643
 *   ldrb r2, [r3]     -> 0x781A
 *   mov r1, r2        -> 0x4611
 *   lsls r2, r2, #1   -> 0x0052
 *   lsrs r2, r1, #1   -> 0x084A
 *   adds r1, r3, #1   -> 0x1C59
 *   ldr r1, [r3, #8]  -> 0x6899
 *   ldr r2, [r3, #4]  -> 0x685A
 *   cmp r2, #9        -> 0x2A09
 *   ldr r0, [r1]      -> 0x6808
 *   cmp r0, r3        -> 0x4298
 *   push {r3}         -> 0xB408
 *   ldr r0, [r1, #4]  -> 0x6848
 *   pop {r3}          -> 0xBC08
 *   ldrb r0, [r1, #8] -> 0x7A08
 *   cmp r0, #0x3E     -> 0x283E
 *   adds r1, r1, #1   -> 0x3101
 *   subs r2, r2, #1   -> 0x3A01
 *   mov r5, r0        -> 0x4605
 */

/* Bug 27: encode a Thumb-2 cbz/cbnz (T1). from/to are byte addresses,
 * rn is 0-7, is_cbnz selects cbnz (1) vs cbz (0).
 * Verified against arm-vita-eabi-as: cbnz r0,+10 from 0 -> 0xB928, etc. */
static void encode_cbnz(uint16_t *out, uintptr_t from, uintptr_t to, int rn, int is_cbnz) {
    int32_t imm32 = (int32_t)to - (int32_t)(from + 4);
    int i = (imm32 >> 6) & 1;
    int imm5 = (imm32 >> 1) & 0x1F;
    out[0] = (uint16_t)(0xB100 | (is_cbnz << 11) | (i << 9) | (imm5 << 3) | (rn & 7));
}

/* Bug 27: encode Thumb-2 movw (T3). rd 0-15, imm16 0-0xFFFF.
 * Verified: movw r5,#0x1234 -> f241 2534; movw r5,#0x1800 -> f641 0500. */
static void encode_movw(uint16_t *out, int rd, uint32_t imm16) {
    int i = (imm16 >> 11) & 1;
    int imm4 = (imm16 >> 12) & 0xF;
    int imm3 = (imm16 >> 8) & 0x7;
    int imm8 = imm16 & 0xFF;
    out[0] = (uint16_t)(0xF240 | (i << 10) | imm4);
    out[1] = (uint16_t)((imm3 << 12) | ((rd & 0xF) << 8) | imm8);
}

/* Bug 27: encode Thumb-2 movt (T3). rd 0-15, imm16 0-0xFFFF.
 * Verified: movt r5,#0x98ff -> f6c9 05ff. */
static void encode_movt(uint16_t *out, int rd, uint32_t imm16) {
    int i = (imm16 >> 11) & 1;
    int imm4 = (imm16 >> 12) & 0xF;
    int imm3 = (imm16 >> 8) & 0x7;
    int imm8 = imm16 & 0xFF;
    out[0] = (uint16_t)(0xF2C0 | (i << 10) | imm4);
    out[1] = (uint16_t)((imm3 << 12) | ((rd & 0xF) << 8) | imm8);
}

/* Bug 27: real empty ptree builder (ptree_fake.cpp). */
void *ptree_fake_init(void);

int _newlib_heap_size_user = 256 * 1024 * 1024;

static so_module mod_cpp, mod_fmod, mod_engine;

// Native entry points (Java_com_blitwise_engine_jni_CPJNILib_*)
static void (*N_onCreate)(void *env, void *clazz);
static void (*N_onSurfaceCreated)(void *env, void *clazz);
static void (*N_onSurfaceChanged)(void *env, void *clazz, int w, int h);
static int (*N_onDrawFrame)(void *env, void *clazz);
static void (*N_setAcceleration)(void *env, void *clazz, int enabled, float x, float y, float z);
static void (*N_onPause)(void *env, void *clazz, int b);
static void (*N_onResume)(void *env, void *clazz);

extern so_default_dynlib default_dynlib[];
extern const int default_dynlib_size;
extern void bionic_sF_init(void);
// Bug 17: exception-throw interposers (implemented in bionic_compat.c).
void __cxa_throw_soloader(void *ex, void *tinfo, void (*dest)(void *));
void __cxa_rethrow_soloader(void);
void cxa_throw_hook_init(void *real_throw, void *real_rethrow,
                         void *cur_exception_type);

// Bug 17: overwrite one of the module's GOT slots (JUMP_SLOT/GLOB_DAT) for
// an imported symbol with our replacement, returning the previous target.
// Used for __cxa_throw/__cxa_rethrow: the game binds those from its own
// libc++_shared.so via DT_NEEDED, which wins over our fallback table, so
// direct GOT patching post-resolution is the only interception point.
static void *patch_got_import(so_module *mod, const char *symname, void *replacement) {
    for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i]
                                            : &mod->relplt[i - mod->num_reldyn];
        int type = ELF32_R_TYPE(rel->r_info);
        if (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT)
            continue;
        Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        if (sym->st_shndx != SHN_UNDEF)
            continue;
        if (strcmp(mod->dynstr + sym->st_name, symname) != 0)
            continue;
        uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
        void *real = (void *)*slot;
        blog("main: GOT patch %s: slot=%p real=%p -> hook=%p",
             symname, (void *)slot, real, replacement);
        kuKernelCpuUnrestrictedMemcpy(slot, &replacement, sizeof(replacement));
        return real;
    }
    blog("main: GOT patch %s: import not found", symname);
    return NULL;
}

// Per-initializer logger for so_initialize: pinpoints which static
// initializer crashes (see bootlog).
static void init_logger(int idx, int total, uintptr_t addr) {
    if (idx <= -1000)
        blog("init: [%d/%d] SKIPPED (diagnostic skip list) %p", -1000 - idx, total, (void *)addr);
    else
        blog("init: [%d/%d] calling %p", idx, total, (void *)addr);
}

static void *get_sym(so_module *mod, const char *name, int required) {
    void *p = (void *)so_symbol(mod, name);
    if (!p && required) {
        blog("get_sym: REQUIRED export missing: %s", name);
        fatal_error("Missing required native export: %s", name);
    }
    if (!p)
        blog("get_sym: optional export not found: %s", name);
    else
        blog("get_sym: %s -> %p", name, p);
    return p;
}

// List the data dir early: if her game data is missing/misplaced we see it
// immediately instead of guessing.
static void log_data_dir(void) {
    blog("datadir: listing " DATA_PATH);
    SceUID dfd = sceIoDopen(DATA_PATH);
    if (dfd < 0) {
        blog("datadir: sceIoDopen FAILED: 0x%08x", dfd);
        return;
    }
    SceIoDirent de;
    int count = 0;
    while (sceIoDread(dfd, &de) > 0) {
        blog("datadir: [%s] size=%d", de.d_name, (int)de.d_stat.st_size);
        count++;
        if (count > 60) { blog("datadir: ... (truncated)"); break; }
    }
    sceIoDclose(dfd);
    blog("datadir: %d entries total", count);

    const char *need[] = {
        "libengine.so", "libfmod.so", "libc++_shared.so",
        "libsqlcipher.so", "assets", NULL
    };
    for (int i = 0; need[i]; i++) {
        char p[256];
        snprintf(p, sizeof(p), DATA_PATH "%s", need[i]);
        blog("datadir: %-16s %s", need[i],
             file_exists(p) ? "present" : "MISSING");
    }
}

int main(void) {
    bootlog_init();
    blog("main: entry (Pocket Tanks Vita, instrumented build)");

    // Overclock for headroom (same as other .so ports)
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    blog("main: clocks set 444/222/222/166");

    bionic_sF_init();
    blog("main: bionic stdio init done");

    log_data_dir();

    // Data files must be installed by the user (see data_prep.sh)
    if (!file_exists(DATA_PATH "libengine.so")) {
        blog("main: FATAL - libengine.so not found in " DATA_PATH);
        fatal_error("Data files not found. Install the game data to %s "
                    "(see data_prep.sh).", DATA_PATH);
    }
    blog("main: libengine.so present, loading libraries");

    // Load libraries in dependency order (so_util chains them by SONAME)
    int rc;
    rc = so_file_load(&mod_cpp, DATA_PATH "libc++_shared.so", LOAD_ADDR_CPP);
    blog("main: so_file_load libc++_shared.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libc++_shared.so load failed"); fatal_error("Could not load libc++_shared.so"); }

    rc = so_file_load(&mod_fmod, DATA_PATH "libfmod.so", LOAD_ADDR_FMOD);
    blog("main: so_file_load libfmod.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libfmod.so load failed"); fatal_error("Could not load libfmod.so"); }

    rc = so_file_load(&mod_engine, DATA_PATH "libengine.so", LOAD_ADDR_ENGINE);
    blog("main: so_file_load libengine.so -> %d", rc);
    if (rc < 0) { blog("main: FATAL - libengine.so load failed"); fatal_error("Could not load libengine.so"); }

    // Bug 25: libengine.so's internal file-open helper takes the fopen path
    // only when filename[0] == '/'. On Android the prefs path is absolute,
    // but our "ux0:..." paths fell into the dead asset path, so every prefs
    // write failed ("cp_fwrite failed"). libengine.so lives in the data dir
    // (never updated by a VPK reinstall), so patch the loaded image in
    // memory instead: file offset 0x3d3366 sits in the executable PT_LOAD
    // (p_offset=0), so runtime addr = text_base + 0x3d3366.
    // Patch: cmp r0, #47 (0x282f) -> cmp r0, #117 (0x2875).
    {
        uint16_t *pp = (uint16_t *)(mod_engine.text_base + 0x3d3366);
        uint16_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, pp, sizeof(before));
        if (before == 0x282f) {
            uint16_t after = 0x2875;
            kuKernelCpuUnrestrictedMemcpy(pp, &after, sizeof(after));
            so_flush_caches(&mod_engine);
            blog("main: [libengine] runtime patch APPLIED: cmp r0,#47->#117 at %p (0x%04x->0x%04x)",
                 (void *)pp, before, after);
        } else {
            blog("main: [libengine] runtime patch SKIPPED at %p (halfword=0x%04x, want 0x282f)",
                 (void *)pp, before);
        }
    }

    // Bug 27: libengine.so's throwing get_child (boost::property_tree) throws
    // ptree_bad_path "No such node (<xmlattr>)" when the zoom UI config
    // queries attributes of an XML element that has none. The game expects an
    // empty ptree (it iterates attributes / reads them with defaults).
    //
    // Bug 28: the Bug27 trampoline (return fake for ALL missing nodes) broke
    // ptree::get<T>(path, default): get() catches ptree_bad_path to return
    // the default, but our fake made get_value<T>() do lexical_cast<T>(""),
    // throwing bad_lexical_cast (uncaught) -> terminate. Confirmed via
    // disassembly: the throw is lexical_cast<ushort> inside boost::gregorian
    // date parsing, reached from get<date>(path, default) on a missing node.
    //
    // Fix: path-based policy in the trampoline. Only "<xmlattr>" queries get
    // the fake empty ptree (the XML-attribute iteration pattern). ALL other
    // missing-node queries take the ORIGINAL throw path (text_base+0xa48d0,
    // re-derived from the pre-patch `cbz r0,<throw>`) so get-with-default
    // keeps working.
    //
    // Register facts (verified from disassembly): at the site, r0 = find_child
    // result (NULL if not found), r8 = path_type& (callee-saved, set at
    // 0xa4898 `mov r8,r1`, preserved by find_child, passed to the throw-path
    // message builder at 0xa48de). The path is a boost string_path wrapping
    // the game's 12-byte string: byte[0] even -> short (len=byte[0]>>1, data
    // at str+1); byte[0] odd -> long (len=[str+4], data=[str+8]).
    //
    // Site: file offset 0xa48b0 in the executable PT_LOAD (p_offset=0),
    // so runtime addr = text_base + 0xa48b0.
    // Original: 4605 b168 (mov r5, r0; cbz r0, <throw @ 0xa48d0>)
    // Patched:  b.w <trampoline> (trampoline in the RX patch arena, <16MB away)
    //
    // Trampoline (94 bytes; every halfword verified against arm-vita-eabi-as):
    //   T+0:  cbnz r0, FOUND            ; r0 != NULL -> r5 = r0
    //   T+2:  mov r3, r8                ; 0x4643, r3 = path
    //   T+4:  ldrb r2, [r3]             ; 0x781A, r2 = m_value[0]
    //   T+6:  mov r1, r2                ; 0x4611, save byte
    //   T+8:  lsls r2, r2, #1           ; 0x0052, carry = bit0 (long?)
    //   T+10: bcs LONG
    //   T+12: lsrs r2, r1, #1           ; 0x084A, r2 = len (short)
    //   T+14: adds r1, r3, #1           ; 0x1C59, r1 = data (short)
    //   T+16: b SCAN
    //   T+18: LONG: ldr r1, [r3, #8]    ; 0x6899, r1 = data (long)
    //   T+20: ldr r2, [r3, #4]          ; 0x685A, r2 = len (long)
    //   T+22: SCAN: cmp r2, #9          ; 0x2A09, need >= 9 bytes
    //   T+24: blo NOTFOUND
    //   T+26: movw r3, #0x783C          ; r3 = "<xml" (LE 0x6C6D783C)
    //   T+30: movt r3, #0x6C6D
    //   T+34: LOOP: ldr r0, [r1]        ; 0x6808
    //   T+36: cmp r0, r3                ; 0x4298
    //   T+38: bne ADVANCE
    //   T+40: push {r3}                 ; 0xB408, save "<xml"
    //   T+42: movw r3, #0x7461          ; r3 = "attr" (LE 0x72747461)
    //   T+46: movt r3, #0x7274
    //   T+50: ldr r0, [r1, #4]          ; 0x6848
    //   T+52: cmp r0, r3                ; 0x4298
    //   T+54: pop {r3}                  ; 0xBC08, restore "<xml"
    //   T+56: bne ADVANCE
    //   T+58: ldrb r0, [r1, #8]         ; 0x7A08
    //   T+60: cmp r0, #0x3E             ; 0x283E, '>'
    //   T+62: beq ISXMLATTR
    //   T+64: ADVANCE: adds r1, r1, #1  ; 0x3101
    //   T+66: subs r2, r2, #1           ; 0x3A01
    //   T+68: cmp r2, #9                ; 0x2A09
    //   T+70: bhs LOOP
    //   T+72: NOTFOUND: b.w THROW       ; THROW = text_base + 0xa48d0
    //   T+76: ISXMLATTR: movw r5, #lo16(&empty_ptree)
    //   T+80: movt r5, #hi16(&empty_ptree)
    //   T+84: b.w NORMAL                ; NORMAL = text_base + 0xa48b4
    //   T+88: FOUND: mov r5, r0         ; 0x4605
    //   T+90: b.w NORMAL
    // Only r0-r3 are clobbered (caller-saved); r8 is preserved (read-only).
    {
        uintptr_t site = mod_engine.text_base + 0xa48b0;
        uintptr_t normal = mod_engine.text_base + 0xa48b4;
        uintptr_t throw_path = mod_engine.text_base + 0xa48d0;
        uint32_t before = 0;
        kuKernelCpuUnrestrictedMemcpy(&before, (void *)site, sizeof(before));
        if (before == 0xb1684605u) {
            void *empty_ptree = ptree_fake_init();
            if (!empty_ptree) {
                blog("main: [libengine] Bug27/28 patch SKIPPED: empty ptree alloc failed");
            } else {
                // Allocate trampoline in the RX patch arena, within b.w range.
                uintptr_t tramp = so_alloc_arena(&mod_engine, 0x1000000, site, 128);
                if (tramp) {
                    uint16_t tcode[47];
                    uintptr_t ept = (uintptr_t)empty_ptree;
                    // Offsets (in halfwords) for branch targets
                    const uintptr_t O_FOUND = 44, O_LONG = 9, O_SCAN = 11,
                                        O_LOOP = 17, O_ADV = 32, O_NOTFOUND = 36,
                                        O_ISXML = 38;
                    encode_cbnz(&tcode[0], tramp + 0, tramp + O_FOUND*2, 0, 1);
                    tcode[1] = 0x4643;  // mov r3, r8
                    tcode[2] = 0x781A;  // ldrb r2, [r3]
                    tcode[3] = 0x4611;  // mov r1, r2
                    tcode[4] = 0x0052;  // lsls r2, r2, #1
                    encode_b_cond(&tcode[5], tramp + 10, tramp + O_LONG*2, 2);  // bcs LONG
                    tcode[6] = 0x084A;  // lsrs r2, r1, #1
                    tcode[7] = 0x1C59;  // adds r1, r3, #1
                    encode_b_n(&tcode[8], tramp + 16, tramp + O_SCAN*2);  // b SCAN
                    tcode[9] = 0x6899;   // LONG: ldr r1, [r3, #8]
                    tcode[10] = 0x685A;  // ldr r2, [r3, #4]
                    tcode[11] = 0x2A09;  // SCAN: cmp r2, #9
                    encode_b_cond(&tcode[12], tramp + 24, tramp + O_NOTFOUND*2, 3);  // blo NOTFOUND
                    encode_movw(&tcode[13], 3, 0x783Cu);  // movw r3, #"<xml"
                    encode_movt(&tcode[15], 3, 0x6C6Du);  // movt r3
                    tcode[17] = 0x6808;  // LOOP: ldr r0, [r1]
                    tcode[18] = 0x4298;  // cmp r0, r3
                    encode_b_cond(&tcode[19], tramp + 38, tramp + O_ADV*2, 1);  // bne ADVANCE
                    tcode[20] = 0xB408;  // push {r3}
                    encode_movw(&tcode[21], 3, 0x7461u);  // movw r3, #"attr"
                    encode_movt(&tcode[23], 3, 0x7274u);  // movt r3
                    tcode[25] = 0x6848;  // ldr r0, [r1, #4]
                    tcode[26] = 0x4298;  // cmp r0, r3
                    tcode[27] = 0xBC08;  // pop {r3}
                    encode_b_cond(&tcode[28], tramp + 56, tramp + O_ADV*2, 1);  // bne ADVANCE
                    tcode[29] = 0x7A08;  // ldrb r0, [r1, #8]
                    tcode[30] = 0x283E;  // cmp r0, #0x3E ('>')
                    encode_b_cond(&tcode[31], tramp + 62, tramp + O_ISXML*2, 0);  // beq ISXMLATTR
                    tcode[32] = 0x3101;  // ADVANCE: adds r1, r1, #1
                    tcode[33] = 0x3A01;  // subs r2, r2, #1
                    tcode[34] = 0x2A09;  // cmp r2, #9
                    encode_b_cond(&tcode[35], tramp + 70, tramp + O_LOOP*2, 2);  // bhs LOOP
                    encode_bw(&tcode[36], tramp + 72, throw_path);  // NOTFOUND: b.w THROW
                    encode_movw(&tcode[38], 5, (uint32_t)(ept & 0xFFFFu));  // ISXMLATTR
                    encode_movt(&tcode[40], 5, (uint32_t)((ept >> 16) & 0xFFFFu));
                    encode_bw(&tcode[42], tramp + 84, normal);  // b.w NORMAL
                    tcode[44] = 0x4605;  // FOUND: mov r5, r0
                    encode_bw(&tcode[45], tramp + 90, normal);  // b.w NORMAL
                    kuKernelCpuUnrestrictedMemcpy((void *)tramp, tcode, sizeof(tcode));
                    // Patch site with b.w to trampoline
                    uint16_t bcode[2];
                    encode_bw(bcode, site, tramp);
                    kuKernelCpuUnrestrictedMemcpy((void *)site, bcode, sizeof(bcode));
                    so_flush_caches(&mod_engine);
                    blog("main: [libengine] Bug27/28 patch APPLIED: get_child not-found -> <xmlattr>?fake:throw (tramp=%p)",
                         (void *)tramp);
                } else {
                    blog("main: [libengine] Bug27/28 patch SKIPPED: no arena space in range");
                }
            }
        } else {
            blog("main: [libengine] Bug27/28 patch SKIPPED at %p (word=0x%08x, want 0xb1684605)",
                 (void *)site, before);
        }
    }

    // Relocate + resolve each module (engine last so deps are in the chain)
    so_module *mods[] = {&mod_cpp, &mod_fmod, &mod_engine};
    const char *names[] = {"libc++_shared", "libfmod", "libengine"};
    for (int i = 0; i < 3; i++)
        blog("main: [%s] text_base=%p num_init=%d", names[i],
             (void *)mods[i]->text_base, mods[i]->num_init_array);
    so_init_logger = init_logger;
    for (int i = 0; i < 3; i++) {
        blog("main: [%s] so_relocate...", names[i]);
        so_relocate(mods[i]);
        blog("main: [%s] so_resolve...", names[i]);
        so_resolve(mods[i], default_dynlib, default_dynlib_size, 0);
        blog("main: [%s] so_flush_caches...", names[i]);
        so_flush_caches(mods[i]);
        blog("main: [%s] so_initialize...", names[i]);
        so_initialize(mods[i]);
        blog("main: [%s] relocated+resolved+initialized OK", names[i]);
    }

    // Bug 17: interpose the game's exception-throw entry points so the
    // bootlog names every exception it throws (the fatal one currently dies
    // mid-unwind on 0x0C-corrupted memory before we learn its type).
    {
        void *real_throw =
            patch_got_import(&mod_engine, "__cxa_throw", __cxa_throw_soloader);
        void *real_rethrow =
            patch_got_import(&mod_engine, "__cxa_rethrow", __cxa_rethrow_soloader);
        void *cur_ex_type = get_sym(&mod_cpp, "__cxa_current_exception_type", 0);
        cxa_throw_hook_init(real_throw, real_rethrow, cur_ex_type);
        blog("main: throw hooks installed (throw=%p rethrow=%p)",
             real_throw, real_rethrow);
    }

    // Fake JNI environment
    blog("main: jni_init...");
    jni_init();
    blog("main: jni_init done");

    // JNI_OnLoad: Android calls this when the .so loads; the game stashes
    // the JavaVM* in a global used by its thread-local JNIEnv* getter
    // (jvm->GetEnv). Without it, that getter dereferences a NULL VM*
    // and the game crashes inside onCreate.
    {
        typedef int (*jni_onload_fn)(void *vm, void *reserved);
        jni_onload_fn onload =
            (jni_onload_fn)get_sym(&mod_engine, "JNI_OnLoad", 0);
        if (onload) {
            blog("main: calling JNI_OnLoad...");
            int v = onload((void *)&jvm, NULL);
            blog("main: JNI_OnLoad returned %d (0x%x)", v, v);
        }
    }

    // Pre-seed pthread key 0 for the main thread (see bionic_pthread.c for
    // the full story): the game's JNIEnv* getter expects [cell] == JNIEnv*,
    // but its simple thread-struct getter may claim key 0 first with a
    // zeroed cell. Seed it ourselves so onCreate's first getter call
    // finds a valid JNIEnv*.
    {
        void *cell = calloc(2049, 1);
        if (cell) {
            *(void **)cell = (void *)&jni;
            bionic_pthread_setspecific(0, cell);
            blog("main: pre-seeded pthread key 0 with cell %p (JNIEnv* %p)",
                 cell, (void *)&jni);
        }
    }

    // Input (touch + gamepad)
    blog("main: input_init...");
    input_init();
    blog("main: input_init done");

    // Graphics: vitaGL + GLES1.1 EGL context
    blog("main: graphics_init...");
    graphics_init();
    blog("main: graphics_init done");

    // Resolve native entry points
    blog("main: resolving JNI entry points...");
    N_onCreate = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onCreate", 1);
    N_onSurfaceCreated = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onSurfaceCreated", 1);
    N_onSurfaceChanged = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onSurfaceChanged", 1);
    N_onDrawFrame = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onDrawFrame", 1);
    N_setAcceleration = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_setAcceleration", 0);
    N_onPause = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onPause", 0);
    N_onResume = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onResume", 0);
    PT_onTouch = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onTouch", 0);
    PT_onGamepadButton = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onGamepadButton", 0);
    PT_onGamepadAxis = get_sym(&mod_engine,
        "Java_com_blitwise_engine_jni_CPJNILib_onGamepadAxis", 0);
    blog("main: entry points resolved");

    // The native methods are static; pass a fake jclass (any pointer).
    // FindClass returns a strdup'd name which serves as the class token.
    void *clazz = jni->FindClass(&jni, "com/blitwise/engine/jni/CPJNILib");
    blog("main: FindClass -> %p", clazz);

    blog("main: calling onCreate...");
    N_onCreate(&jni, clazz);
    blog("main: onCreate returned");

    blog("main: calling onSurfaceCreated...");
    N_onSurfaceCreated(&jni, clazz);
    blog("main: onSurfaceCreated returned");

    blog("main: calling onSurfaceChanged(960, 544)...");
    N_onSurfaceChanged(&jni, clazz, 960, 544);
    blog("main: onSurfaceChanged returned");

    blog("main: entering frame loop");
    int frame = 0;
    while (1) {
        input_poll();

        if (N_setAcceleration)
            N_setAcceleration(&jni, clazz, 0, 0.0f, 0.0f, 0.0f);

        int r = N_onDrawFrame(&jni, clazz);
        if (r != 0)
            blog("main: onDrawFrame returned %d at frame %d", r, frame);

        graphics_swap();

        frame++;
        if ((frame % 600) == 0)
            blog("main: frame %d - still alive", frame);
    }

    blog("main: main returning (UNEXPECTED - loop should never exit)");
    bootlog_close();
    return 0;
}
