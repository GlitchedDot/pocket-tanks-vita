// File-based boot logger for bring-up debugging.
#include "bootlog.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>

#include <stdarg.h>
#include <stdio.h>

#define BOOTLOG_PATH "ux0:data/pockettanks/bootlog.txt"

static SceUID blog_fd = -1;

void bootlog_init(void) {
    // Make sure the data dir exists so the log can be created even if the
    // user hasn't installed game data yet (that itself is worth logging).
    sceIoMkdir("ux0:data/pockettanks", 0777);
    blog_fd = sceIoOpen(BOOTLOG_PATH,
                        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    blog("=== Pocket Tanks Vita bootlog ===");
    if (blog_fd < 0)
        sceClibPrintf("[bootlog] WARN: cannot open %s (0x%08x); stdout only\n",
                      BOOTLOG_PATH, blog_fd);
    else
        blog("bootlog: writing to " BOOTLOG_PATH " (fd ok)");
}

void blog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    // Unbuffered file write: immediate, survives crashes.
    if (blog_fd >= 0) {
        sceIoWrite(blog_fd, buf, n);
        sceIoWrite(blog_fd, "\n", 1);
    }
    sceClibPrintf("%s\n", buf);
}

void bootlog_close(void) {
    if (blog_fd >= 0) {
        sceIoClose(blog_fd);
        blog_fd = -1;
    }
}
