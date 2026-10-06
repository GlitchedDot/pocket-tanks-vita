// Wrappers mapping OES-suffixed GLES1 entry points (as imported by the game)
// to vitaGL's non-suffixed equivalents.
#include <vitaGL.h>

void glBindFramebufferOES(unsigned int target, unsigned int framebuffer) {
    glBindFramebuffer(target, framebuffer);
}
void glBlendEquationOES(unsigned int mode) {
    glBlendEquation(mode);
}
unsigned int glCheckFramebufferStatusOES(unsigned int target) {
    return glCheckFramebufferStatus(target);
}
void glDeleteFramebuffersOES(int n, const unsigned int *framebuffers) {
    glDeleteFramebuffers(n, framebuffers);
}
void glFramebufferTexture2DOES(unsigned int target, unsigned int attachment,
                               unsigned int textarget, unsigned int texture, int level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
void glGenFramebuffersOES(int n, unsigned int *framebuffers) {
    glGenFramebuffers(n, framebuffers);
}

// Declared in EGL/egl.h but missing from vitaGL.h; present in libvitaGL.a.
extern EGLSurface eglGetCurrentSurface(EGLint readdraw);
EGLSurface pt_eglGetCurrentSurface(EGLint readdraw) {
    return eglGetCurrentSurface(readdraw);
}
