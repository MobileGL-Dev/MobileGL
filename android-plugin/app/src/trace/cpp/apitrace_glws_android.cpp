#include "glws.hpp"
#include "retrace.hpp"

#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

using PfnEglBindApi = EGLBoolean (*)(EGLenum);
using PfnEglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using PfnEglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
using PfnEglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *);
using PfnEglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
using PfnEglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PfnEglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PfnEglGetConfigAttrib = EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint *);
using PfnEglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
using PfnEglGetError = EGLint (*)();
using PfnEglInitialize = EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *);
using PfnEglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PfnEglQueryString = const char *(*)(EGLDisplay, EGLint);
using PfnEglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PfnEglTerminate = EGLBoolean (*)(EGLDisplay);

struct EglFns {
    PfnEglBindApi bindApi = nullptr;
    PfnEglChooseConfig chooseConfig = nullptr;
    PfnEglCreateContext createContext = nullptr;
    PfnEglCreatePbufferSurface createPbufferSurface = nullptr;
    PfnEglCreateWindowSurface createWindowSurface = nullptr;
    PfnEglDestroyContext destroyContext = nullptr;
    PfnEglDestroySurface destroySurface = nullptr;
    PfnEglGetConfigAttrib getConfigAttrib = nullptr;
    PfnEglGetDisplay getDisplay = nullptr;
    PfnEglGetError getError = nullptr;
    PfnEglInitialize initialize = nullptr;
    PfnEglMakeCurrent makeCurrent = nullptr;
    PfnEglQueryString queryString = nullptr;
    PfnEglSwapBuffers swapBuffers = nullptr;
    PfnEglTerminate terminate = nullptr;
};

EglFns gEgl;
EGLDisplay gDisplay = EGL_NO_DISPLAY;
void *gMobileGl = nullptr;
const glws::Drawable *gCurrentDrawable = nullptr;
EGLContext gCurrentContext = EGL_NO_CONTEXT;
ANativeWindow *gNativeWindow = nullptr;
int gRequestedWidth = 0;
int gRequestedHeight = 0;

int ResolveWidth(int width) {
    if (gRequestedWidth > 0) {
        return gRequestedWidth;
    }
    return width > 0 ? width : 1;
}

int ResolveHeight(int height) {
    if (gRequestedHeight > 0) {
        return gRequestedHeight;
    }
    return height > 0 ? height : 1;
}

void *Lookup(const char *name) {
    if (gMobileGl == nullptr) {
        gMobileGl = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (gMobileGl == nullptr) {
            gMobileGl = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (gMobileGl != nullptr) {
        void *symbol = dlsym(gMobileGl, name);
        if (symbol != nullptr) {
            return symbol;
        }
    }
    return dlsym(RTLD_DEFAULT, name);
}

void HoldAfterTargetPresent(const char *callNo) {
    const char *holdMsText = std::getenv("MOBILEGL_TRACE_HOLD_MS");
    if (holdMsText == nullptr || holdMsText[0] == '\0') {
        return;
    }
    const int holdMs = std::atoi(holdMsText);
    if (holdMs <= 0) {
        return;
    }
    const char *holdDone = std::getenv("MOBILEGL_TRACE_HOLD_DONE");
    if (holdDone != nullptr && std::strcmp(holdDone, "1") == 0) {
        return;
    }
    const char *holdCall = std::getenv("MOBILEGL_TRACE_HOLD_CALL");
    if (holdCall != nullptr && holdCall[0] != '\0' && std::strcmp(holdCall, callNo) != 0) {
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    setenv("MOBILEGL_TRACE_HOLD_DONE", "1", 1);
}

template <typename T>
bool Load(T &slot, const char *name) {
    slot = reinterpret_cast<T>(Lookup(name));
    if (slot == nullptr) {
        std::cerr << "error: failed to resolve " << name << " from MobileGL\n";
        return false;
    }
    return true;
}

bool LoadEgl() {
    return Load(gEgl.bindApi, "eglBindAPI") &&
           Load(gEgl.chooseConfig, "eglChooseConfig") &&
           Load(gEgl.createContext, "eglCreateContext") &&
           Load(gEgl.createPbufferSurface, "eglCreatePbufferSurface") &&
           Load(gEgl.createWindowSurface, "eglCreateWindowSurface") &&
           Load(gEgl.destroyContext, "eglDestroyContext") &&
           Load(gEgl.destroySurface, "eglDestroySurface") &&
           Load(gEgl.getConfigAttrib, "eglGetConfigAttrib") &&
           Load(gEgl.getDisplay, "eglGetDisplay") &&
           Load(gEgl.getError, "eglGetError") &&
           Load(gEgl.initialize, "eglInitialize") &&
           Load(gEgl.makeCurrent, "eglMakeCurrent") &&
           Load(gEgl.queryString, "eglQueryString") &&
           Load(gEgl.swapBuffers, "eglSwapBuffers") &&
           Load(gEgl.terminate, "eglTerminate");
}

class AndroidVisual final : public glws::Visual {
public:
    EGLConfig config = nullptr;
    EGLenum api = EGL_OPENGL_API;
    EGLint nativeVisualId = 0;

    explicit AndroidVisual(glws::Profile prof) : Visual(prof) {}
};

class AndroidDrawable final : public glws::Drawable {
public:
    EGLSurface surface = EGL_NO_SURFACE;

    AndroidDrawable(const AndroidVisual *visual, int width, int height, bool pbuffer)
        : Drawable(visual, width, height, pbuffer) {
        createSurface();
    }

    ~AndroidDrawable() override {
        destroySurface();
    }

    void resize(int w, int h) override {
        w = ResolveWidth(w);
        h = ResolveHeight(h);
        if (w == width && h == height) {
            return;
        }
        destroySurface();
        width = w;
        height = h;
        createSurface();
        if (gCurrentDrawable == this && gCurrentContext != EGL_NO_CONTEXT) {
            gEgl.makeCurrent(gDisplay, surface, surface, gCurrentContext);
        }
    }

    void show() override {
        visible = true;
    }

    void swapBuffers() override {
        if (surface != EGL_NO_SURFACE) {
            char callNo[32];
            const char *overrideCallNo = getenv("MOBILEGL_TRACE_CURRENT_CALL_OVERRIDE");
            if (overrideCallNo != nullptr && overrideCallNo[0] != '\0') {
                snprintf(callNo, sizeof(callNo), "%s", overrideCallNo);
            } else {
                snprintf(callNo, sizeof(callNo), "%u", retrace::callNo);
            }
            const char *targetCall = getenv("MOBILEGL_PRESENT_DUMP_CALL");
            const char *presentStats = getenv("MOBILEGL_PRESENT_STATS");
            if (targetCall != nullptr && targetCall[0] != '\0' &&
                presentStats != nullptr && presentStats[0] != '\0' && strcmp(presentStats, "0") != 0) {
                std::cerr << "MOBILEGL_TRACE_SWAP call=" << callNo
                          << " target=" << targetCall << "\n";
            }
            setenv("MOBILEGL_PRESENT_CURRENT_CALL", callNo, 1);
            gEgl.swapBuffers(gDisplay, surface);
            unsetenv("MOBILEGL_PRESENT_CURRENT_CALL");
            HoldAfterTargetPresent(callNo);
        }
    }

private:
    void createSurface() {
        const int surfaceWidth = ResolveWidth(width);
        const int surfaceHeight = ResolveHeight(height);
        if (!pbuffer && gNativeWindow != nullptr) {
            EGLint nativeVisualId = static_cast<const AndroidVisual *>(visual)->nativeVisualId;
            ANativeWindow_setBuffersGeometry(gNativeWindow, surfaceWidth, surfaceHeight, nativeVisualId);
            surface = gEgl.createWindowSurface(
                    gDisplay,
                    static_cast<const AndroidVisual *>(visual)->config,
                    gNativeWindow,
                    nullptr);
        } else {
            const EGLint attribs[] = {
                    EGL_WIDTH, surfaceWidth,
                    EGL_HEIGHT, surfaceHeight,
                    EGL_NONE,
            };
            surface = gEgl.createPbufferSurface(
                    gDisplay,
                    static_cast<const AndroidVisual *>(visual)->config,
                    attribs);
        }
        if (surface == EGL_NO_SURFACE) {
            std::cerr << "error: EGL surface creation failed: 0x" << std::hex
                      << gEgl.getError() << std::dec << "\n";
        }
    }

    void destroySurface() {
        if (surface != EGL_NO_SURFACE) {
            gEgl.destroySurface(gDisplay, surface);
            surface = EGL_NO_SURFACE;
        }
    }
};

class AndroidContext final : public glws::Context {
public:
    EGLContext context = EGL_NO_CONTEXT;

    AndroidContext(const AndroidVisual *visual, glws::Context *shareContext)
        : Context(visual) {
        const EGLContext share = shareContext == nullptr
                                         ? EGL_NO_CONTEXT
                                         : static_cast<AndroidContext *>(shareContext)->context;
        if (!gEgl.bindApi(visual->api)) {
            std::cerr << "error: eglBindAPI failed: 0x" << std::hex << gEgl.getError()
                      << std::dec << "\n";
        }

        EGLint attribs[9];
        int index = 0;
        if (visual->api == EGL_OPENGL_ES_API) {
            attribs[index++] = EGL_CONTEXT_CLIENT_VERSION;
            attribs[index++] = static_cast<EGLint>(std::max<unsigned>(profile.major, 2));
        } else {
            attribs[index++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
            attribs[index++] = profile.major >= 3 ? profile.major : 3;
            attribs[index++] = EGL_CONTEXT_MINOR_VERSION_KHR;
            attribs[index++] = profile.major >= 3 ? profile.minor : 3;
            attribs[index++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
            attribs[index++] = profile.core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR
                                            : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR;
        }
        attribs[index++] = EGL_NONE;

        context = gEgl.createContext(gDisplay, visual->config, share, attribs);
        if (context == EGL_NO_CONTEXT && visual->api == EGL_OPENGL_API) {
            const EGLint fallbackAttribs[] = {EGL_NONE};
            context = gEgl.createContext(gDisplay, visual->config, share, fallbackAttribs);
        }
        if (context == EGL_NO_CONTEXT) {
            std::cerr << "error: eglCreateContext failed: 0x" << std::hex << gEgl.getError()
                      << std::dec << "\n";
        }
    }

    ~AndroidContext() override {
        if (context != EGL_NO_CONTEXT) {
            gEgl.destroyContext(gDisplay, context);
        }
    }
};

const AndroidDrawable *AsAndroidDrawable(const glws::Drawable *drawable) {
    return static_cast<const AndroidDrawable *>(drawable);
}

const AndroidContext *AsAndroidContext(const glws::Context *context) {
    return static_cast<const AndroidContext *>(context);
}

} // namespace

namespace glws {

void init() {
    if (gDisplay != EGL_NO_DISPLAY) {
        return;
    }
    if (!LoadEgl()) {
        return;
    }
    gDisplay = gEgl.getDisplay(EGL_DEFAULT_DISPLAY);
    if (gDisplay == EGL_NO_DISPLAY) {
        std::cerr << "error: eglGetDisplay failed: 0x" << std::hex << gEgl.getError()
                  << std::dec << "\n";
        return;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!gEgl.initialize(gDisplay, &major, &minor)) {
        std::cerr << "error: eglInitialize failed: 0x" << std::hex << gEgl.getError()
                  << std::dec << "\n";
    }
}

void cleanup() {
    if (gDisplay != EGL_NO_DISPLAY) {
        gEgl.makeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        gEgl.terminate(gDisplay);
        gDisplay = EGL_NO_DISPLAY;
    }
}

Visual *createVisual(bool doubleBuffer, unsigned samples, Profile profile) {
    auto *visual = new AndroidVisual(profile);
    visual->doubleBuffer = doubleBuffer;
    // This glws implementation adapts apitrace's platform calls to EGL only.
    // A GLX trace may reach this path through glretrace_glx.cpp, but MobileGL is
    // still accessed through egl* and gl* symbols, never through glX* symbols.
    visual->api = profile.api == glfeatures::API_GLES ? EGL_OPENGL_ES_API : EGL_OPENGL_API;

    if (!gEgl.bindApi(visual->api)) {
        delete visual;
        return nullptr;
    }

    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, gNativeWindow == nullptr ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE,
            visual->api == EGL_OPENGL_ES_API ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_SAMPLE_BUFFERS, samples > 1 ? 1 : 0,
            EGL_SAMPLES, samples > 1 ? static_cast<EGLint>(samples) : 0,
            EGL_NONE,
    };
    EGLint count = 0;
    if (!gEgl.chooseConfig(gDisplay, attribs, &visual->config, 1, &count) || count < 1) {
        delete visual;
        return nullptr;
    }
    gEgl.getConfigAttrib(gDisplay, visual->config, EGL_NATIVE_VISUAL_ID, &visual->nativeVisualId);
    return visual;
}

Drawable *createDrawable(const Visual *visual, int width, int height, const pbuffer_info *pbInfo) {
    return new AndroidDrawable(static_cast<const AndroidVisual *>(visual), ResolveWidth(width),
                               ResolveHeight(height), pbInfo != nullptr);
}

Context *createContext(const Visual *visual, Context *shareContext, bool) {
    return new AndroidContext(static_cast<const AndroidVisual *>(visual), shareContext);
}

bool makeCurrentInternal(Drawable *drawable, Drawable *readable, Context *context) {
    EGLSurface drawSurface = drawable == nullptr ? EGL_NO_SURFACE : AsAndroidDrawable(drawable)->surface;
    EGLSurface readSurface = readable == nullptr ? drawSurface : AsAndroidDrawable(readable)->surface;
    EGLContext eglContext = context == nullptr ? EGL_NO_CONTEXT : AsAndroidContext(context)->context;
    if (drawSurface == EGL_NO_SURFACE && readSurface == EGL_NO_SURFACE && eglContext == EGL_NO_CONTEXT) {
        gEgl.makeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        gCurrentDrawable = nullptr;
        gCurrentContext = EGL_NO_CONTEXT;
        return true;
    }
    if (gEgl.makeCurrent(gDisplay, drawSurface, readSurface, eglContext) != EGL_TRUE) {
        return false;
    }
    gCurrentDrawable = drawable;
    gCurrentContext = eglContext;
    return true;
}

bool processEvents() {
    return false;
}

bool bindTexImage(Drawable *, int) {
    return false;
}

bool releaseTexImage(Drawable *, int) {
    return false;
}

bool setPbufferAttrib(Drawable *, const int *) {
    return false;
}

} // namespace glws

extern "C" bool mobilegl_trace_get_drawable_bounds(int *width, int *height) {
    if (gCurrentDrawable == nullptr || width == nullptr || height == nullptr) {
        return false;
    }
    *width = gCurrentDrawable->width;
    *height = gCurrentDrawable->height;
    return *width > 0 && *height > 0;
}

extern "C" void mobilegl_trace_set_native_window(ANativeWindow *window) {
    if (gNativeWindow == window) {
        return;
    }
    if (gNativeWindow != nullptr) {
        ANativeWindow_release(gNativeWindow);
    }
    gNativeWindow = window;
    if (gNativeWindow != nullptr) {
        ANativeWindow_acquire(gNativeWindow);
    }
}

extern "C" void mobilegl_trace_set_requested_size(int width, int height) {
    gRequestedWidth = width > 0 ? width : 0;
    gRequestedHeight = height > 0 ? height : 0;
}
