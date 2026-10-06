#include "android_log.h"
#include "utils.h"

#include <stdarg.h>
#include <stdio.h>

int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    log_info("[alog][%s] %s", tag ? tag : "?", text ? text : "(null)");
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return __android_log_write(prio, tag, buf);
}
