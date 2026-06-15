#include "glws.hpp"
#include "retrace.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>

namespace {

using PfnEglBindApi = EGLBoolean (*)(EGLenum);
using PfnEglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
using PfnEglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
using PfnEglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *);
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
using PfnGlGetString = const unsigned char *(*)(unsigned int);

struct EglFns {
    PfnEglBindApi bindApi = nullptr;
    PfnEglChooseConfig chooseConfig = nullptr;
    PfnEglCreateContext createContext = nullptr;
    PfnEglCreatePbufferSurface createPbufferSurface = nullptr;
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
int gRequestedWidth = 0;
int gRequestedHeight = 0;
bool gPrintedGlIdentity = false;

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
        const char *library = std::getenv("MOBILEGL_TRACE_LIBRARY");
        if (library != nullptr && library[0] != '\0') {
            gMobileGl = dlopen(library, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
            if (gMobileGl == nullptr) {
                gMobileGl = dlopen(library, RTLD_NOW | RTLD_GLOBAL);
            }
        }
        if (gMobileGl == nullptr) {
            gMobileGl = dlopen("libMobileGL.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        }
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

void PrintGlIdentityOnce() {
    if (gPrintedGlIdentity) {
        return;
    }
    auto glGetString = reinterpret_cast<PfnGlGetString>(Lookup("glGetString"));
    if (glGetString == nullptr) {
        std::cerr << "MOBILEGL_TRACE_GL_INFO unavailable: glGetString was not resolved\n";
        gPrintedGlIdentity = true;
        return;
    }
    constexpr unsigned int GL_VENDOR_ENUM = 0x1F00;
    constexpr unsigned int GL_RENDERER_ENUM = 0x1F01;
    constexpr unsigned int GL_VERSION_ENUM = 0x1F02;
    constexpr unsigned int GL_SHADING_LANGUAGE_VERSION_ENUM = 0x8B8C;
    auto stringOrUnknown = [glGetString](unsigned int name) {
        const auto *value = glGetString(name);
        return value == nullptr ? reinterpret_cast<const unsigned char *>("<null>") : value;
    };
    std::cerr << "MOBILEGL_TRACE_GL_VENDOR: "
              << reinterpret_cast<const char *>(stringOrUnknown(GL_VENDOR_ENUM)) << "\n"
              << "MOBILEGL_TRACE_GL_RENDERER: "
              << reinterpret_cast<const char *>(stringOrUnknown(GL_RENDERER_ENUM)) << "\n"
              << "MOBILEGL_TRACE_GL_VERSION: "
              << reinterpret_cast<const char *>(stringOrUnknown(GL_VERSION_ENUM)) << "\n"
              << "MOBILEGL_TRACE_GL_SHADING_LANGUAGE_VERSION: "
              << reinterpret_cast<const char *>(stringOrUnknown(GL_SHADING_LANGUAGE_VERSION_ENUM)) << "\n";
    gPrintedGlIdentity = true;
}

class EglVisual final : public glws::Visual {
public:
    EGLConfig config = nullptr;
    EGLenum api = EGL_OPENGL_API;

    explicit EglVisual(glws::Profile prof) : Visual(prof) {}
};

class EglDrawable final : public glws::Drawable {
public:
    EGLSurface surface = EGL_NO_SURFACE;

    EglDrawable(const EglVisual *visual, int width, int height, bool pbuffer)
        : Drawable(visual, width, height, pbuffer) {
        createSurface();
    }

    ~EglDrawable() override {
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
        if (surface == EGL_NO_SURFACE) {
            return;
        }
        char callNo[32];
        const char *overrideCallNo = getenv("MOBILEGL_TRACE_CURRENT_CALL_OVERRIDE");
        if (overrideCallNo != nullptr && overrideCallNo[0] != '\0') {
            snprintf(callNo, sizeof(callNo), "%s", overrideCallNo);
        } else {
            snprintf(callNo, sizeof(callNo), "%u", retrace::callNo);
        }
        setenv("MOBILEGL_PRESENT_CURRENT_CALL", callNo, 1);
        gEgl.swapBuffers(gDisplay, surface);
        unsetenv("MOBILEGL_PRESENT_CURRENT_CALL");
    }

private:
    void createSurface() {
        const EGLint attribs[] = {
                EGL_WIDTH, ResolveWidth(width),
                EGL_HEIGHT, ResolveHeight(height),
                EGL_NONE,
        };
        surface = gEgl.createPbufferSurface(gDisplay, static_cast<const EglVisual *>(visual)->config, attribs);
        if (surface == EGL_NO_SURFACE) {
            std::cerr << "error: EGL pbuffer creation failed: 0x" << std::hex
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

class EglContext final : public glws::Context {
public:
    EGLContext context = EGL_NO_CONTEXT;

    EglContext(const EglVisual *visual, glws::Context *shareContext)
        : Context(visual) {
        const EGLContext share = shareContext == nullptr ? EGL_NO_CONTEXT : static_cast<EglContext *>(shareContext)->context;
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
            attribs[index++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
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

    ~EglContext() override {
        if (context != EGL_NO_CONTEXT) {
            gEgl.destroyContext(gDisplay, context);
        }
    }
};

const EglDrawable *AsEglDrawable(const glws::Drawable *drawable) {
    return static_cast<const EglDrawable *>(drawable);
}

const EglContext *AsEglContext(const glws::Context *context) {
    return static_cast<const EglContext *>(context);
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
    auto *visual = new EglVisual(profile);
    visual->doubleBuffer = doubleBuffer;
    visual->api = profile.api == glfeatures::API_GLES ? EGL_OPENGL_ES_API : EGL_OPENGL_API;

    if (!gEgl.bindApi(visual->api)) {
        delete visual;
        return nullptr;
    }

    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, visual->api == EGL_OPENGL_ES_API ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
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
    return visual;
}

Drawable *createDrawable(const Visual *visual, int width, int height, const pbuffer_info *pbInfo) {
    return new EglDrawable(static_cast<const EglVisual *>(visual), ResolveWidth(width),
                           ResolveHeight(height), true);
}

Context *createContext(const Visual *visual, Context *shareContext, bool) {
    return new EglContext(static_cast<const EglVisual *>(visual), shareContext);
}

bool makeCurrentInternal(Drawable *drawable, Drawable *readable, Context *context) {
    EGLSurface drawSurface = drawable == nullptr ? EGL_NO_SURFACE : AsEglDrawable(drawable)->surface;
    EGLSurface readSurface = readable == nullptr ? drawSurface : AsEglDrawable(readable)->surface;
    EGLContext eglContext = context == nullptr ? EGL_NO_CONTEXT : AsEglContext(context)->context;
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
    PrintGlIdentityOnce();
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

extern "C" void mobilegl_trace_set_requested_size(int width, int height) {
    gRequestedWidth = width > 0 ? width : 0;
    gRequestedHeight = height > 0 ? height : 0;
}
