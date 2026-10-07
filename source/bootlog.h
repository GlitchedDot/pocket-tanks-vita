#ifndef PT_BOOTLOG_H
#define PT_BOOTLOG_H

// File-based boot logger. Opens ux0:data/pockettanks/bootlog.txt at startup
// and writes every line unbuffered (sceIoWrite = immediate), so the trail
// survives a crash or silent exit. Also mirrors to stdout (sceClibPrintf).

void bootlog_init(void);          // call FIRST in main(), before anything else
void blog(const char *fmt, ...);  // log+flush to file and stdout
void bootlog_close(void);

#endif
