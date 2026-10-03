// gpuprobe - a plain Android process (outside MobileGL) that measures whether the GPU keeps executing
// other contexts' work: a pbuffer GLES context clears + draws and glFinish()es every ~10 ms, printing any
// round trip over 50 ms with a CLOCK_REALTIME timestamp.  Usage: gpuprobe <seconds> [priority: low|med|high]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG 0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG 0x3103
#endif

static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    EGLint prio = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
    if (argc > 2 && !strcmp(argv[2], "high")) prio = EGL_CONTEXT_PRIORITY_HIGH_IMG;
    if (argc > 2 && !strcmp(argv[2], "low")) prio = EGL_CONTEXT_PRIORITY_LOW_IMG;
    setvbuf(stdout, NULL, _IOLBF, 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(d, NULL, NULL);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig c; EGLint n;
    eglChooseConfig(d, ca, &c, 1, &n);
    const EGLint pa[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_PRIORITY_LEVEL_IMG, prio, EGL_NONE};
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    if (x == EGL_NO_CONTEXT) { printf("no ctx 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(d, s, s, x);
    EGLint got = 0; eglQueryContext(d, x, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &got);
    const double t0 = now_ms(); double maxRt = 0; long iters = 0, slow = 0;
    printf("[probe] start %.0f prio=0x%x\n", t0, got);
    while (now_ms() - t0 < seconds * 1000.0) {
        const double a = now_ms();
        glClearColor((iters & 1) ? 1.f : 0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        const double rt = now_ms() - a;
        if (rt > maxRt) maxRt = rt;
        if (rt > 50) { ++slow; printf("[probe] glFinish %.0f ms ending at %.0f\n", rt, now_ms()); }
        ++iters;
        usleep(10000);
    }
    printf("[probe] END iters=%ld max=%.0f ms slow=%ld\n", iters, maxRt, slow);
    return 0;
}
