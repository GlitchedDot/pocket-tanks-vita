#ifndef MUSIC_H
#define MUSIC_H

/* CPJNISound music channel: the engine asks for music tracks by asset
 * name (loadMod/playMod/stopMod/freeMod/modVolume). The tracks are
 * AAC-in-MP4 (.m4a) files in the game's assets/music/. This module
 * resolves and demuxes them; playback (sceAudiodec -> BGM port) is
 * layered on top of the parsed frame table. */

int  music_load(const char *name);   /* 1 = parsed ok, 0 = failed */
void music_play(void);
void music_stop(void);
void music_free(void);
void music_volume(int v);
int  music_frame_count(void);
long music_frame_offset(int idx, int *size_out);

#endif
