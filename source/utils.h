#ifndef PT_UTILS_H
#define PT_UTILS_H

#include <psp2/kernel/clib.h>

#define LOG_TAG "PockettanksVita"

#define log_info(fmt, ...) \
    sceClibPrintf("[" LOG_TAG "][I] " fmt "\n", ##__VA_ARGS__)
#define log_warn(fmt, ...) \
    sceClibPrintf("[" LOG_TAG "][W] " fmt "\n", ##__VA_ARGS__)
#define log_error(fmt, ...) \
    sceClibPrintf("[" LOG_TAG "][E] " fmt "\n", ##__VA_ARGS__)

int file_exists(const char *path);
void fatal_error(const char *fmt, ...);

#endif
