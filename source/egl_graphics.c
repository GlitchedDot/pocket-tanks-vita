#include "egl_graphics.h"
#include "utils.h"

#include <vitaGL.h>
// NOTE: vitaGL.h already declares the EGL API; do not include EGL/egl.h.

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLContext egl_context = EGL_NO_CONTEXT;

void graphics_init(void) {
    log_info("graphics_init: vglInit");
    vglUseTripleBuffering(GL_FALSE);
    vglInitWithCustomThreshold(0, PT_SCREEN_W, PT_SCREEN_H,
                               6 * 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_NONE);

    log_info("graphics_init: egl setup");
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY)
        fatal_error("eglGetDisplay failed");

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor))
        fatal_error("eglInitialize failed");

    EGLint cfgAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(egl_display, cfgAttribs, &config, 1, &numConfigs) || numConfigs < 1)
        fatal_error("eglChooseConfig failed");

    EGLint ctxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 1,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (egl_context == EGL_NO_CONTEXT)
        fatal_error("eglCreateContext failed");

    egl_surface = eglCreateWindowSurface(egl_display, config, 0, NULL);
    if (egl_surface == EGL_NO_SURFACE)
        fatal_error("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
        fatal_error("eglMakeCurrent failed");

    log_info("graphics_init: EGL ready (%d.%d)", major, minor);
}

void graphics_swap(void) {
    eglSwapBuffers(egl_display, egl_surface);
}
