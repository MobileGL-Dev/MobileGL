#include "glws.hpp"
#include "retrace.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <thread>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

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
using PfnGlGetString = const unsigned char *(*)(unsigned int);

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
int gRequestedWidth = 0;
int gRequestedHeight = 0;
bool gPrintedGlIdentity = false;

#if defined(__APPLE__)
constexpr unsigned long kNSWindowStyleMaskTitled = 1ul << 0;
constexpr unsigned long kNSWindowStyleMaskClosable = 1ul << 1;
constexpr unsigned long kNSWindowStyleMaskMiniaturizable = 1ul << 2;
constexpr unsigned long kNSWindowStyleMaskResizable = 1ul << 3;
constexpr unsigned long kNSBackingStoreBuffered = 2;
constexpr long kNSApplicationActivationPolicyRegular = 0;
constexpr unsigned long long kNSEventMaskAny = ~0ull;

template <typename Fn>
Fn ObjcMsgSend() {
    return reinterpret_cast<Fn>(objc_msgSend);
}

id SendId(id receiver, const char *selector) {
    return ObjcMsgSend<id (*)(id, SEL)>()(receiver, sel_registerName(selector));
}

id NSStringFromUtf8(const char *value) {
    id stringClass = reinterpret_cast<id>(objc_getClass("NSString"));
    return ObjcMsgSend<id (*)(id, SEL, const char *)>()(
        stringClass, sel_registerName("stringWithUTF8String:"), value);
}

void SendVoid(id receiver, const char *selector) {
    ObjcMsgSend<void (*)(id, SEL)>()(receiver, sel_registerName(selector));
}

void SendVoidId(id receiver, const char *selector, id value) {
    ObjcMsgSend<void (*)(id, SEL, id)>()(receiver, sel_registerName(selector), value);
}

void SendVoidBool(id receiver, const char *selector, bool value) {
    ObjcMsgSend<void (*)(id, SEL, bool)>()(receiver, sel_registerName(selector), value);
}

void SendVoidLong(id receiver, const char *selector, long value) {
    ObjcMsgSend<void (*)(id, SEL, long)>()(receiver, sel_registerName(selector), value);
}

void SendVoidCGRect(id receiver, const char *selector, CGRect value) {
    ObjcMsgSend<void (*)(id, SEL, CGRect)>()(receiver, sel_registerName(selector), value);
}

void SendVoidCGSize(id receiver, const char *selector, CGSize value) {
    ObjcMsgSend<void (*)(id, SEL, CGSize)>()(receiver, sel_registerName(selector), value);
}

id Retain(id object) {
    return object ? SendId(object, "retain") : nil;
}

void Release(id object) {
    if (object) {
        SendVoid(object, "release");
    }
}

id SharedApplication() {
    id appClass = reinterpret_cast<id>(objc_getClass("NSApplication"));
    return appClass ? SendId(appClass, "sharedApplication") : nil;
}

void PumpMacOSEvents() {
    id app = SharedApplication();
    if (!app) {
        return;
    }
    id distantPast = SendId(reinterpret_cast<id>(objc_getClass("NSDate")), "distantPast");
    id stringClass = reinterpret_cast<id>(objc_getClass("NSString"));
    id defaultRunLoopMode = ObjcMsgSend<id (*)(id, SEL, const char *)>()(
        stringClass, sel_registerName("stringWithUTF8String:"), "kCFRunLoopDefaultMode");
    while (true) {
        id event = ObjcMsgSend<id (*)(id, SEL, unsigned long long, id, id, bool)>()(
            app, sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:"),
            kNSEventMaskAny, distantPast, defaultRunLoopMode, true);
        if (!event) {
            break;
        }
        SendVoidId(app, "sendEvent:", event);
    }
    SendVoid(app, "updateWindows");
}

extern "C" void mobilegl_trace_pump_events() {
    PumpMacOSEvents();
}

void EnsureApplicationActive() {
    id app = SharedApplication();
    if (!app) {
        return;
    }
    SendVoid(app, "finishLaunching");
    SendVoidLong(app, "setActivationPolicy:", kNSApplicationActivationPolicyRegular);
    SendVoidId(app, "unhide:", nil);
    SendVoidBool(app, "activateIgnoringOtherApps:", true);
}

id CreateMetalWindow(int width, int height, id *outLayer) {
    EnsureApplicationActive();
    id windowClass = reinterpret_cast<id>(objc_getClass("NSWindow"));
    id metalLayerClass = reinterpret_cast<id>(objc_getClass("CAMetalLayer"));
    if (!windowClass || !metalLayerClass) {
        std::cerr << "error: failed to resolve NSWindow/CAMetalLayer\n";
        return nil;
    }

    const auto surfaceWidth = static_cast<CGFloat>(std::max(width, 1));
    const auto surfaceHeight = static_cast<CGFloat>(std::max(height, 1));
    const CGRect frame = {{80.0, 80.0}, {surfaceWidth, surfaceHeight}};
    constexpr unsigned long style = kNSWindowStyleMaskTitled | kNSWindowStyleMaskClosable |
                                    kNSWindowStyleMaskMiniaturizable | kNSWindowStyleMaskResizable;
    id window = SendId(windowClass, "alloc");
    window = ObjcMsgSend<id (*)(id, SEL, CGRect, unsigned long, unsigned long, bool)>()(
        window, sel_registerName("initWithContentRect:styleMask:backing:defer:"),
        frame, style, kNSBackingStoreBuffered, false);
    if (!window) {
        std::cerr << "error: failed to create NSWindow\n";
        return nil;
    }
    SendVoidId(window, "setTitle:", NSStringFromUtf8("MobileGL Trace Replay"));
    SendVoidBool(window, "setReleasedWhenClosed:", false);

    id contentView = SendId(window, "contentView");
    id layer = SendId(metalLayerClass, "layer");
    if (!contentView || !layer) {
        Release(window);
        std::cerr << "error: failed to create CAMetalLayer\n";
        return nil;
    }
    Retain(layer);
    SendVoidBool(contentView, "setWantsLayer:", true);
    SendVoidCGRect(layer, "setFrame:", {{0.0, 0.0}, {surfaceWidth, surfaceHeight}});
    SendVoidCGSize(layer, "setDrawableSize:", {surfaceWidth, surfaceHeight});
    SendVoidId(contentView, "setLayer:", layer);

    *outLayer = layer;
    return window;
}

void ShowMetalWindow(id window) {
    if (!window) {
        return;
    }
    EnsureApplicationActive();
    ObjcMsgSend<void (*)(id, SEL, id)>()(window, sel_registerName("makeKeyAndOrderFront:"), nil);
    SendVoid(window, "orderFrontRegardless");
    PumpMacOSEvents();
}
#endif

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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    while (std::chrono::steady_clock::now() < deadline) {
#if defined(__APPLE__)
        PumpMacOSEvents();
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
#if defined(__APPLE__)
    PumpMacOSEvents();
#endif
    setenv("MOBILEGL_TRACE_HOLD_DONE", "1", 1);
}

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

bool TraceReplayWantsWindowSurface() {
    const char *mode = std::getenv("MOBILEGL_TRACE_SURFACE");
    return mode != nullptr && std::strcmp(mode, "window") == 0;
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
#if defined(__APPLE__)
    id window = nil;
    id metalLayer = nil;
    bool windowShown = false;
#endif

    EglDrawable(const EglVisual *visual, int width, int height, bool pbuffer)
        : Drawable(visual, width, height, pbuffer) {
        createSurface();
    }

    ~EglDrawable() override {
        destroySurface();
#if defined(__APPLE__)
        Release(metalLayer);
        metalLayer = nil;
        Release(window);
        window = nil;
#endif
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
#if defined(__APPLE__)
        if (windowShown) {
            ShowMetalWindow(window);
        }
#endif
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
#if defined(__APPLE__)
        PumpMacOSEvents();
        if (window && !windowShown) {
            ShowMetalWindow(window);
            windowShown = true;
        }
#endif
        HoldAfterTargetPresent(callNo);
    }

private:
    bool shouldCreateWindowSurface() const {
        if (pbuffer) {
            return false;
        }
        const char *mode = std::getenv("MOBILEGL_TRACE_SURFACE");
        return mode != nullptr && std::strcmp(mode, "window") == 0;
    }

    void createSurface() {
        const int surfaceWidth = ResolveWidth(width);
        const int surfaceHeight = ResolveHeight(height);
#if defined(__APPLE__)
        if (shouldCreateWindowSurface()) {
            if (!window) {
                window = CreateMetalWindow(surfaceWidth, surfaceHeight, &metalLayer);
            } else if (metalLayer) {
                const auto w = static_cast<CGFloat>(std::max(surfaceWidth, 1));
                const auto h = static_cast<CGFloat>(std::max(surfaceHeight, 1));
                SendVoidCGSize(metalLayer, "setDrawableSize:", {w, h});
                SendVoidCGRect(metalLayer, "setFrame:", {{0.0, 0.0}, {w, h}});
            }
            if (metalLayer) {
                surface = gEgl.createWindowSurface(
                        gDisplay,
                        static_cast<const EglVisual *>(visual)->config,
                        reinterpret_cast<EGLNativeWindowType>(metalLayer),
                        nullptr);
            }
            if (surface == EGL_NO_SURFACE) {
                std::cerr << "error: EGL window surface creation failed: 0x" << std::hex
                          << gEgl.getError() << std::dec << "\n";
            }
            return;
        }
#endif
        const EGLint attribs[] = {
                EGL_WIDTH, surfaceWidth,
                EGL_HEIGHT, surfaceHeight,
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
#if defined(__APPLE__)
        if (!shouldCreateWindowSurface()) {
            Release(metalLayer);
            metalLayer = nil;
            Release(window);
            window = nil;
        }
#endif
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

    const EGLint surfaceType = TraceReplayWantsWindowSurface() ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT;
    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, surfaceType,
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
    (void)pbInfo;
    const bool usePbuffer = !TraceReplayWantsWindowSurface();
    return new EglDrawable(static_cast<const EglVisual *>(visual), ResolveWidth(width),
                           ResolveHeight(height), usePbuffer);
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
#if defined(__APPLE__)
    PumpMacOSEvents();
#endif
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
