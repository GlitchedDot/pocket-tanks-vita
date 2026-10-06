#include "utils.h"

#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdio.h>

int file_exists(const char *path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

void fatal_error(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_error("FATAL: %s", buf);
    sceKernelDelayThread(5 * 1000 * 1000);
    sceKernelExitProcess(0);
}
