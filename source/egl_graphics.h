#ifndef PT_EGL_GRAPHICS_H
#define PT_EGL_GRAPHICS_H

#define PT_SCREEN_W 960
#define PT_SCREEN_H 544

// vitaGL init + GLES1.1 EGL context, made current on this thread.
void graphics_init(void);
// Swap the game draws.
void graphics_swap(void);

#endif
