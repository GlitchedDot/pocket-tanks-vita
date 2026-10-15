#ifndef FMOD_TRACE_H
#define FMOD_TRACE_H

#include <stdint.h>

/* Patch libengine's GOT entries for the FMOD boot-chain calls so each
 * call is logged (args + result) before/after passing through to the
 * real libfmod function. Pure observation; behavior is unchanged.
 * engine_base = mod_engine.text_base. Call after the modules are
 * loaded and before onCreate. fmod_base lets the Create wrapper read
 * libfmod's cached CPU-feature mask (Build AX). */
void fmod_trace_install(uintptr_t engine_base, uintptr_t fmod_base);

/* FMOD_Debug_Initialize callback (Build AY): FMOD's own log lines,
 * forwarded to the bootlog. */
int pt_fmod_debug_cb(unsigned int flags, const char *file, int line,
                     const char *func, const char *message);

#endif
