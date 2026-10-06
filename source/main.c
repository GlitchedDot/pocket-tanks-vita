// Pocket Tanks (com.blitwise.ptankshd) PS Vita port — loader entry point.
#include "utils.h"
#include "egl_graphics.h"
#include "input.h"

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <stdio.h>
#include <string.h>

#define DATA_PATH "ux0:data/pockettanks/"
#define LOAD_ADDR_CPP    0x98000000
#define LOAD_ADDR_FMOD   0x98200000
#define LOAD_ADDR_ENGINE 0x99000000

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

static void *get_sym(so_module *mod, const char *name, int required) {
    void *p = (void *)so_symbol(mod, name);
    if (!p && required)
        fatal_error("Missing required native export: %s", name);
    if (!p)
        log_warn("Optional native export not found: %s", name);
    return p;
}

int main(void) {
    log_info("=== Pocket Tanks Vita v1 boot ===");

    // Overclock for headroom (same as other .so ports)
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    bionic_sF_init();

    // Data files must be installed by the user (see data_prep.sh)
    if (!file_exists(DATA_PATH "libengine.so")) {
        fatal_error("Data files not found. Install the game data to %s "
                    "(see data_prep.sh).", DATA_PATH);
    }

    // Load libraries in dependency order (so_util chains them by SONAME)
    log_info("loading libc++_shared.so");
    if (so_file_load(&mod_cpp, DATA_PATH "libc++_shared.so", LOAD_ADDR_CPP) < 0)
        fatal_error("Could not load libc++_shared.so");
    log_info("loading libfmod.so");
    if (so_file_load(&mod_fmod, DATA_PATH "libfmod.so", LOAD_ADDR_FMOD) < 0)
        fatal_error("Could not load libfmod.so");
    log_info("loading libengine.so");
    if (so_file_load(&mod_engine, DATA_PATH "libengine.so", LOAD_ADDR_ENGINE) < 0)
        fatal_error("Could not load libengine.so");

    // Relocate + resolve each module (engine last so deps are in the chain)
    so_module *mods[] = {&mod_cpp, &mod_fmod, &mod_engine};
    for (int i = 0; i < 3; i++) {
        so_relocate(mods[i]);
        so_resolve(mods[i], default_dynlib,
                   default_dynlib_size, 0);
        so_flush_caches(mods[i]);
        so_initialize(mods[i]);
        log_info("module %d relocated+resolved+initialized", i);
    }

    // Fake JNI environment
    jni_init();
    log_info("FalsoJNI initialized");

    // Input (touch + gamepad)
    input_init();

    // Graphics: vitaGL + GLES1.1 EGL context
    graphics_init();

    // Resolve native entry points
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

    // The native methods are static; pass a fake jclass (any pointer).
    // FindClass returns a strdup'd name which serves as the class token.
    void *clazz = jni->FindClass(&jni, "com/blitwise/engine/jni/CPJNILib");

    log_info("calling onCreate");
    N_onCreate(&jni, clazz);
    log_info("calling onSurfaceCreated");
    N_onSurfaceCreated(&jni, clazz);
    log_info("calling onSurfaceChanged(960, 544)");
    N_onSurfaceChanged(&jni, clazz, 960, 544);

    log_info("entering frame loop");
    int frame = 0;
    while (1) {
        input_poll();

        if (N_setAcceleration)
            N_setAcceleration(&jni, clazz, 0, 0.0f, 0.0f, 0.0f);

        int r = N_onDrawFrame(&jni, clazz);
        (void)r;

        graphics_swap();

        frame++;
        if ((frame % 600) == 0)
            log_info("frame %d", frame);
    }

    return 0;
}
