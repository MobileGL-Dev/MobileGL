/* mgprobe - preflight gate for running a GL conformance suite against MobileGL
 * from a bare adb-shell process (no APK, no Activity).
 *
 * Verifies, for one backend and one surface type, that MobileGL can hand out a
 * GL 3.3 core context and that pixels read back correctly - both from the
 * default framebuffer and from a user FBO. Run this before burning hours on a
 * CTS run; it catches a broken device/library pairing in about a second.
 *
 *   mgprobe --backend DirectGLES|DirectVulkan --surface pbuffer|imagereader
 *           [--lib /path/to/libMobileGL.so]
 *
 * Exit status: 0 if a context came up and FBO readback is correct, non-zero
 * otherwise. Default-framebuffer readback is reported but does NOT gate, because
 * DirectVulkan is known to return zeros there while FBO readback is sound.
 *
 * Build (NDK, arm64):
 *   $NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android26-clang \
 *       -O1 -o mgprobe mgprobe.c -ldl -llog -landroid -lmediandk
 */
#include <android/native_window.h>
#include <dlfcn.h>
#include <media/NdkImageReader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;

#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30A2
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_RENDERBUFFER 0x8D41

typedef EGLDisplay (*P_getDisplay)(EGLNativeDisplayType);
typedef EGLBoolean (*P_initialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (*P_bindAPI)(EGLenum);
typedef EGLBoolean (*P_chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean (*P_getConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLSurface (*P_createWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
typedef EGLSurface (*P_createPbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLContext (*P_createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*P_makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint (*P_getError)(void);

typedef const unsigned char *(*P_glGetString)(unsigned int);
typedef void (*P_glGetIntegerv)(unsigned int, int *);
typedef void (*P_glClearColor)(float, float, float, float);
typedef void (*P_glClear)(unsigned int);
typedef void (*P_glFinish)(void);
typedef void (*P_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void *);
typedef unsigned int (*P_glGetError)(void);
typedef void (*P_glGenTextures)(int, unsigned int *);
typedef void (*P_glBindTexture)(unsigned int, unsigned int);
typedef void (*P_glTexImage2D)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
typedef void (*P_glTexParameteri)(unsigned int, unsigned int, int);
typedef void (*P_glGenFramebuffers)(int, unsigned int *);
typedef void (*P_glBindFramebuffer)(unsigned int, unsigned int);
typedef void (*P_glFramebufferTexture2D)(unsigned int, unsigned int, unsigned int, unsigned int, int);
typedef unsigned int (*P_glCheckFramebufferStatus)(unsigned int);
typedef void (*P_glViewport)(int, int, int, int);
typedef void (*P_glGenRenderbuffers)(int, unsigned int *);
typedef void (*P_glBindRenderbuffer)(unsigned int, unsigned int);
typedef void (*P_glRenderbufferStorage)(unsigned int, unsigned int, int, int);
typedef void (*P_glFramebufferRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);

static void *g_lib;
static void *S(const char *n) { return dlsym(g_lib, n); }

static void on_image(void *ctx, AImageReader *r) {
    (void)ctx;
    AImage *img = NULL;
    /* Drain the queue, or the producer blocks once maxImages are in flight. */
    if (AImageReader_acquireNextImage(r, &img) == AMEDIA_OK && img) AImage_delete(img);
}

#define DIM 256

static int near8(unsigned got, int want, int tol) {
    int d = (int)got - want;
    return d <= tol && d >= -tol;
}

int main(int argc, char **argv) {
    const char *backend = "DirectGLES";
    const char *surface = "pbuffer";
    const char *libpath = "libMobileGL.so";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) backend = argv[++i];
        else if (!strcmp(argv[i], "--surface") && i + 1 < argc) surface = argv[++i];
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) libpath = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--backend DirectGLES|DirectVulkan]"
                            " [--surface pbuffer|imagereader] [--lib path]\n", argv[0]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    /* MobileGL parses its config from an ELF constructor, so the backend must be
     * selected before the library is mapped. */
    setenv("MOBILEGL_BACKEND_TYPE", backend, 1);
    printf("mgprobe backend=%s surface=%s lib=%s\n", backend, surface, libpath);

    int useWindow = !strcmp(surface, "imagereader");
    ANativeWindow *win = NULL;
    AImageReader *reader = NULL;
    if (useWindow) {
        if (AImageReader_newWithUsage(DIM, DIM, AIMAGE_FORMAT_RGBA_8888,
                                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                          AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                                      4, &reader) != AMEDIA_OK || !reader) {
            printf("FAIL AImageReader_newWithUsage\n");
            return 3;
        }
        AImageReader_ImageListener l = {NULL, on_image};
        AImageReader_setImageListener(reader, &l);
        if (AImageReader_getWindow(reader, &win) != AMEDIA_OK || !win) {
            printf("FAIL AImageReader_getWindow\n");
            return 3;
        }
    }

    g_lib = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        printf("FAIL dlopen: %s\n", dlerror());
        return 4;
    }

    P_getDisplay eglGetDisplay_ = (P_getDisplay)S("eglGetDisplay");
    P_initialize eglInitialize_ = (P_initialize)S("eglInitialize");
    P_bindAPI eglBindAPI_ = (P_bindAPI)S("eglBindAPI");
    P_chooseConfig eglChooseConfig_ = (P_chooseConfig)S("eglChooseConfig");
    P_getConfigAttrib eglGetConfigAttrib_ = (P_getConfigAttrib)S("eglGetConfigAttrib");
    P_createWindowSurface eglCreateWindowSurface_ = (P_createWindowSurface)S("eglCreateWindowSurface");
    P_createPbufferSurface eglCreatePbufferSurface_ = (P_createPbufferSurface)S("eglCreatePbufferSurface");
    P_createContext eglCreateContext_ = (P_createContext)S("eglCreateContext");
    P_makeCurrent eglMakeCurrent_ = (P_makeCurrent)S("eglMakeCurrent");
    P_getError eglGetError_ = (P_getError)S("eglGetError");

    if (!eglGetDisplay_ || !eglInitialize_ || !eglChooseConfig_ || !eglCreateContext_ || !eglMakeCurrent_) {
        printf("FAIL missing core EGL exports\n");
        return 5;
    }

    EGLDisplay dpy = eglGetDisplay_(EGL_DEFAULT_DISPLAY);
    EGLint vmaj = 0, vmin = 0;
    if (!eglInitialize_(dpy, &vmaj, &vmin)) {
        printf("FAIL eglInitialize err=0x%x\n", eglGetError_ ? eglGetError_() : 0);
        return 6;
    }
    if (eglBindAPI_ && !eglBindAPI_(EGL_OPENGL_API)) {
        printf("FAIL eglBindAPI(EGL_OPENGL_API) err=0x%x\n", eglGetError_ ? eglGetError_() : 0);
        return 7;
    }

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, useWindow ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE};
    EGLConfig cfg = 0;
    EGLint ncfg = 0;
    if (!eglChooseConfig_(dpy, cfgAttribs, &cfg, 1, &ncfg) || ncfg < 1) {
        printf("FAIL eglChooseConfig n=%d err=0x%x\n", ncfg, eglGetError_ ? eglGetError_() : 0);
        return 8;
    }

    EGLSurface surf;
    if (useWindow) {
        EGLint vis = 0;
        if (eglGetConfigAttrib_ && eglGetConfigAttrib_(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vis) && vis)
            ANativeWindow_setBuffersGeometry(win, DIM, DIM, vis);
        surf = eglCreateWindowSurface_(dpy, cfg, (EGLNativeWindowType)win, NULL);
    } else {
        const EGLint sa[] = {EGL_WIDTH, DIM, EGL_HEIGHT, DIM, EGL_NONE};
        surf = eglCreatePbufferSurface_(dpy, cfg, sa);
    }
    if (surf == EGL_NO_SURFACE) {
        printf("FAIL create%sSurface err=0x%x\n", useWindow ? "Window" : "Pbuffer",
               eglGetError_ ? eglGetError_() : 0);
        return 9;
    }

    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext_(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        printf("FAIL eglCreateContext(3.3 core) err=0x%x\n", eglGetError_ ? eglGetError_() : 0);
        return 10;
    }
    /* MobileGL requires draw == read and rejects EGL_NO_SURFACE. */
    if (!eglMakeCurrent_(dpy, surf, surf, ctx)) {
        printf("FAIL eglMakeCurrent err=0x%x\n", eglGetError_ ? eglGetError_() : 0);
        return 11;
    }

    P_glGetString glGetString_ = (P_glGetString)S("glGetString");
    P_glGetIntegerv glGetIntegerv_ = (P_glGetIntegerv)S("glGetIntegerv");
    P_glClearColor glClearColor_ = (P_glClearColor)S("glClearColor");
    P_glClear glClear_ = (P_glClear)S("glClear");
    P_glFinish glFinish_ = (P_glFinish)S("glFinish");
    P_glReadPixels glReadPixels_ = (P_glReadPixels)S("glReadPixels");
    P_glGetError glGetError_ = (P_glGetError)S("glGetError");
    P_glGenTextures glGenTextures_ = (P_glGenTextures)S("glGenTextures");
    P_glBindTexture glBindTexture_ = (P_glBindTexture)S("glBindTexture");
    P_glTexImage2D glTexImage2D_ = (P_glTexImage2D)S("glTexImage2D");
    P_glTexParameteri glTexParameteri_ = (P_glTexParameteri)S("glTexParameteri");
    P_glGenFramebuffers glGenFramebuffers_ = (P_glGenFramebuffers)S("glGenFramebuffers");
    P_glBindFramebuffer glBindFramebuffer_ = (P_glBindFramebuffer)S("glBindFramebuffer");
    P_glFramebufferTexture2D glFramebufferTexture2D_ = (P_glFramebufferTexture2D)S("glFramebufferTexture2D");
    P_glCheckFramebufferStatus glCheckFramebufferStatus_ = (P_glCheckFramebufferStatus)S("glCheckFramebufferStatus");
    P_glViewport glViewport_ = (P_glViewport)S("glViewport");
    P_glGenRenderbuffers glGenRenderbuffers_ = (P_glGenRenderbuffers)S("glGenRenderbuffers");
    P_glBindRenderbuffer glBindRenderbuffer_ = (P_glBindRenderbuffer)S("glBindRenderbuffer");
    P_glRenderbufferStorage glRenderbufferStorage_ = (P_glRenderbufferStorage)S("glRenderbufferStorage");
    P_glFramebufferRenderbuffer glFramebufferRenderbuffer_ = (P_glFramebufferRenderbuffer)S("glFramebufferRenderbuffer");

    int major = -1, minor = -1, profile = -1;
    glGetIntegerv_(GL_MAJOR_VERSION, &major);
    glGetIntegerv_(GL_MINOR_VERSION, &minor);
    glGetIntegerv_(GL_CONTEXT_PROFILE_MASK, &profile);
    printf("  GL_VENDOR   %s\n", (const char *)glGetString_(GL_VENDOR));
    printf("  GL_RENDERER %s\n", (const char *)glGetString_(GL_RENDERER));
    printf("  GL_VERSION  %s\n", (const char *)glGetString_(GL_VERSION));
    printf("  GLSL        %s\n", (const char *)glGetString_(GL_SHADING_LANGUAGE_VERSION));
    printf("  version %d.%d profile_mask 0x%x %s\n", major, minor, profile,
           (profile & 1) ? "(core)" : "(NOT CORE)");

    unsigned char px[4];

    /* Default framebuffer. */
    glClearColor_(0.25f, 0.5f, 0.75f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    if (glFinish_) glFinish_();
    memset(px, 0, sizeof px);
    glReadPixels_(DIM / 2, DIM / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int defOk = near8(px[0], 64, 10) && near8(px[1], 128, 10) && near8(px[2], 191, 10);
    printf("  default-FB readback (%u,%u,%u,%u) %s\n", px[0], px[1], px[2], px[3],
           defOk ? "ok" : "BROKEN");

    /* User FBO - this is what dEQP uses with --deqp-surface-type=fbo. */
    unsigned int tex = 0, fbo = 0;
    glGenTextures_(1, &tex);
    glBindTexture_(GL_TEXTURE_2D, tex);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, DIM, DIM, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers_(1, &fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    unsigned int fbst = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    int fboOk = 0;
    if (fbst == GL_FRAMEBUFFER_COMPLETE) {
        glViewport_(0, 0, DIM, DIM);
        glClearColor_(0.9f, 0.2f, 0.4f, 1.0f);
        glClear_(GL_COLOR_BUFFER_BIT);
        if (glFinish_) glFinish_();
        memset(px, 0, sizeof px);
        glReadPixels_(DIM / 2, DIM / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        fboOk = near8(px[0], 230, 10) && near8(px[1], 51, 10) && near8(px[2], 102, 10);
        printf("  user-FBO readback   (%u,%u,%u,%u) %s\n", px[0], px[1], px[2], px[3],
               fboOk ? "ok" : "BROKEN");
    } else {
        printf("  user-FBO incomplete status=0x%x\n", fbst);
    }

    /* FBO with a RENDERBUFFER colour attachment. This is what dEQP's
     * FboRenderContext allocates for --deqp-surface-type=fbo, so it is the path
     * that actually decides a conformance run - a texture-attached FBO working
     * says nothing about it. */
    unsigned int rbo = 0, rfbo = 0;
    int rboOk = 0;
    if (glGenRenderbuffers_ && glBindRenderbuffer_ && glRenderbufferStorage_ && glFramebufferRenderbuffer_) {
        glGenRenderbuffers_(1, &rbo);
        glBindRenderbuffer_(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, DIM, DIM);
        glBindRenderbuffer_(GL_RENDERBUFFER, 0);
        glGenFramebuffers_(1, &rfbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, rfbo);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
        unsigned int rst = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
        if (rst == GL_FRAMEBUFFER_COMPLETE) {
            glViewport_(0, 0, DIM, DIM);
            glClearColor_(0.1f, 0.7f, 0.3f, 1.0f);
            glClear_(GL_COLOR_BUFFER_BIT);
            if (glFinish_) glFinish_();
            memset(px, 0, sizeof px);
            glReadPixels_(DIM / 2, DIM / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            rboOk = near8(px[0], 26, 10) && near8(px[1], 179, 10) && near8(px[2], 77, 10);
            printf("  rbo-FBO readback    (%u,%u,%u,%u) %s\n", px[0], px[1], px[2], px[3],
                   rboOk ? "ok" : "BROKEN");
        } else {
            printf("  rbo-FBO incomplete status=0x%x\n", rst);
        }
    } else {
        printf("  rbo-FBO skipped (renderbuffer entry points unavailable)\n");
    }

    unsigned glerr = glGetError_ ? glGetError_() : 0;
    int ok = fboOk && rboOk && (major > 3 || (major == 3 && minor >= 3)) && (profile & 1) && glerr == 0;
    printf("%s backend=%s surface=%s default_fb=%s user_fbo=%s rbo_fbo=%s glerr=0x%x\n",
           ok ? "PASS" : "FAIL", backend, surface, defOk ? "ok" : "broken",
           fboOk ? "ok" : "broken", rboOk ? "ok" : "broken", glerr);

    fflush(stdout);
    /* MobileGL aborts in static teardown; leave before that runs. */
    _exit(ok ? 0 : 1);
}
