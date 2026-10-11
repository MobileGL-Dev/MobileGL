// MobileGL - MobileGL/MG_Test/Wire/GlamorEglSequenceTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// XWAYLAND'S GLAMOR AGAINST MOBILEGL'S EGL, END TO END ON THE HOST: a real MobileGL client against
// a real `--serve` supervisor, through what xwl_glamor_gbm_init_egl and glamor_init (Xwayland
// 24.1) require before they take the GPU path:
//
//   client extension EGL_MESA_platform_gbm (or _KHR_), eglGetPlatformDisplay(GBM, gbm_device),
//   eglInitialize, eglBindAPI(EGL_OPENGL_API), eglCreateContext(EGL_NO_CONFIG_KHR, core 3.1),
//   surfaceless eglMakeCurrent, GL_RENDERER not llvmpipe/softpipe, desktop GL >= 2.1, no
//   GL_ARB_compatibility (core), GL_ARB_vertex_array_object, GL_OES_EGL_image,
//   EGL_EXT_image_dma_buf_import + _modifiers (dmabuf_capable), XRGB8888/ARGB8888 importable,
//   eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) of a shared image's descriptor.
//
// And the implicit-sync flush (docs/Disaggregated/notes/anland/plan-x11-gpu.md): the shared_image
// Flush verb the client sends from glFlush is answered by the server.
//
// Chrome's GPU process on the same kind of display (a GBM one) is the other dma-buf producer, and it
// never flushes: when the compositor's kernel takes dma-buf fences it ends each frame by creating an
// EGL_ANDROID_native_fence_sync fence (through ANGLE, which offers fences only when the native EGL
// has EGL_KHR_fence_sync and the extension, and loads the KHR entry points by name) and hands its
// descriptor to the browser with the buffer. That fence is the only point at which the frame's
// writes into the buffer can reach the compositor's session, so it must be a real one.

#include "P12ServerRig.h"

#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>

#include <fstream>
#include <iterator>

using namespace MobileGL;
using namespace P12;

namespace {
    namespace EGL = MobileGL::MG_Impl::EGLImpl;

    constexpr EGLenum kPlatformGbm = 0x31D7;
    constexpr unsigned kGlRenderer = 0x1F01;
    constexpr unsigned kGlVersion = 0x1F02;
    constexpr unsigned kGlExtensions = 0x1F03;
    constexpr unsigned kGlNumExtensions = 0x821D;
    constexpr Uint32 kXrgb8888 = 0x34325258u;
    constexpr Uint32 kArgb8888 = 0x34325241u;

    struct GlamorReport {
        Int32 platformGbmExtension = 0;
        Int32 display = 0;
        Int32 initialized = 0;
        Int32 noConfigCoreContext = 0;
        Int32 surfacelessCurrent = 0;
        Int32 dmabufCapable = 0;
        Int32 desktopGl = 0;
        Int32 glVersion = 0; // epoxy_gl_version(): major * 10 + minor
        Int32 rendererNotSoftware = 0;
        Int32 coreProfile = 0; // no GL_ARB_compatibility
        Int32 vertexArrayObject = 0;
        Int32 oesEglImage = 0;
        Int32 xrgbImportable = 0;
        Int32 argbImportable = 0;
        Int32 imageCreated = 0;
        Int32 flushAnswered = 0;
        Int32 flushAgainAnswered = 0;
        Int32 eglError = 0;
        char renderer[128] = {};
        char note[128] = {};
    };

    bool HasToken(const char* list, const char* token) {
        if (list == nullptr) return false;
        const std::string all = std::string(" ") + list + " ";
        return all.find(std::string(" ") + token + " ") != std::string::npos;
    }

    // epoxy_has_gl_extension on a core context: glGetStringi.
    bool HasGlExtension(const char* name) {
        using GetIntegervFn = void (*)(unsigned, int*);
        using GetStringiFn = const unsigned char* (*)(unsigned, unsigned);
        const auto getIntegerv = reinterpret_cast<GetIntegervFn>(EGL::GetProcAddress("glGetIntegerv"));
        const auto getStringi = reinterpret_cast<GetStringiFn>(EGL::GetProcAddress("glGetStringi"));
        if (getIntegerv == nullptr || getStringi == nullptr) return false;
        int count = 0;
        getIntegerv(kGlNumExtensions, &count);
        for (int i = 0; i < count; ++i) {
            const unsigned char* ext = getStringi(kGlExtensions, static_cast<unsigned>(i));
            if (ext != nullptr && std::strcmp(reinterpret_cast<const char*>(ext), name) == 0) return true;
        }
        return false;
    }

    void RunGlamor(GlamorReport& r) {
        r.platformGbmExtension = HasToken(EGL::QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_MESA_platform_gbm") ||
                                         HasToken(EGL::QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_KHR_platform_gbm")
                                     ? 1
                                     : 0;
        // Any gbm_device* will do: this process has no GBM loader to ask its backend's name, and the
        // display's frames come from the server, never from the device.
        static int fakeGbmDevice[16] = {};
        const EGLDisplay dpy = EGL::GetPlatformDisplay(kPlatformGbm, fakeGbmDevice, nullptr);
        r.display = dpy != EGL_NO_DISPLAY ? 1 : 0;
        if (!r.display) return;
        EGLint major = 0, minor = 0;
        r.initialized = EGL::Initialize(dpy, &major, &minor) == EGL_TRUE ? 1 : 0;
        if (!r.initialized) return;
        const char* exts = EGL::QueryString(dpy, EGL_EXTENSIONS);
        r.dmabufCapable =
            HasToken(exts, "EGL_EXT_image_dma_buf_import") && HasToken(exts, "EGL_EXT_image_dma_buf_import_modifiers");

        (void)EGL::BindAPI(EGL_OPENGL_API);
        const EGLint core31[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
        const EGLContext ctx = EGL::CreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, core31);
        r.noConfigCoreContext = ctx != EGL_NO_CONTEXT ? 1 : 0;
        if (!r.noConfigCoreContext) {
            r.eglError = EGL::GetError();
            return;
        }
        r.surfacelessCurrent = EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE ? 1 : 0;
        if (!r.surfacelessCurrent) {
            r.eglError = EGL::GetError();
            return;
        }

        using GetStringFn = const unsigned char* (*)(unsigned);
        const auto getString = reinterpret_cast<GetStringFn>(EGL::GetProcAddress("glGetString"));
        const char* renderer = getString ? reinterpret_cast<const char*>(getString(kGlRenderer)) : nullptr;
        const char* version = getString ? reinterpret_cast<const char*>(getString(kGlVersion)) : nullptr;
        std::snprintf(r.renderer, sizeof(r.renderer), "%s", renderer ? renderer : "");
        // xwl_glamor_gbm_init_egl's refusals.
        r.rendererNotSoftware = renderer != nullptr && std::strncmp(renderer, "llvmpipe", 8) != 0 &&
                                std::strstr(renderer, "softpipe") == nullptr;
        // epoxy_is_desktop_gl / epoxy_gl_version.
        if (version != nullptr) {
            r.desktopGl = std::strncmp(version, "OpenGL ES", 9) != 0;
            int vmaj = 0, vmin = 0;
            if (std::sscanf(version, "%d.%d", &vmaj, &vmin) == 2) r.glVersion = vmaj * 10 + vmin;
        }
        r.coreProfile = !HasGlExtension("GL_ARB_compatibility");
        r.vertexArrayObject = HasGlExtension("GL_ARB_vertex_array_object") || HasGlExtension("GL_OES_vertex_array_object");
        r.oesEglImage = HasGlExtension("GL_OES_EGL_image");

        EGLint formats[16] = {};
        EGLint count = 0;
        if (EGL::QueryDmaBufFormats(dpy, 16, formats, &count)) {
            for (EGLint i = 0; i < count; ++i) {
                r.xrgbImportable |= static_cast<Uint32>(formats[i]) == kXrgb8888;
                r.argbImportable |= static_cast<Uint32>(formats[i]) == kArgb8888;
            }
        }

        // A window pixmap: a shared image (what gbm_bo_create gives glamor), bound through
        // EGL_LINUX_DMA_BUF_EXT the way xwl_glamor_gbm_create_pixmap_for_bo does.
        auto* backend = MG_Backend::pActiveBackendObject.get();
        MG_Backend::SharedImageExport image;
        if (backend != nullptr && backend->AllocateSharedImage(64, 32, kXrgb8888, &image) && image.Fd >= 0) {
            const EGLAttrib attribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_LINUX_DRM_FOURCC_EXT, kXrgb8888,
                                         EGL_DMA_BUF_PLANE0_FD_EXT, image.Fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                         static_cast<EGLAttrib>(image.Offset), EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                         static_cast<EGLAttrib>(image.Stride), EGL_NONE};
            const EGLImage eglImage = EGL::CreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
            r.imageCreated = eglImage != EGL_NO_IMAGE ? 1 : 0;
            if (eglImage != EGL_NO_IMAGE) (void)EGL::DestroyImage(dpy, eglImage);
            ::close(image.Fd);
            (void)backend->ReleaseSharedImage(image.Id);
        }

        // glamor's block handler: glFlush, which in an X server is where its renders become visible
        // to the compositor. The verb must be answered (an unknown verb is refused).
        using ClearFn = void (*)(unsigned);
        if (const auto clear = reinterpret_cast<ClearFn>(EGL::GetProcAddress("glClear"))) clear(0x4000);
        r.flushAnswered = backend != nullptr && backend->FlushSharedImageAccesses() ? 1 : 0;
        // Nothing reached the stream since: no round trip, still a success.
        r.flushAgainAnswered = backend != nullptr && backend->FlushSharedImageAccesses() ? 1 : 0;

        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, ctx);
        (void)EGL::Terminate(dpy);
    }

    struct ChromeFenceReport {
        Int32 initialized = 0;
        Int32 fenceSyncExtension = 0;
        Int32 waitSyncExtension = 0;
        Int32 nativeFenceExtension = 0;
        Int32 entryPoints = 0;
        Int32 current = 0;
        Int32 imageBound = 0;
        Int32 framebufferComplete = 0;
        Int32 noContextRefused = 0;
        Int32 fenceCreated = 0;
        Int32 fenceType = 0;
        Int32 fenceWaited = 0;
        Int32 fenceSignaled = 0;
        Int32 dupFd = -2;
        Int32 dupError = 0;
        Int32 dupSignaled = 0;
        Int32 importCreated = 0;
        Int32 importWaited = 0;
        Int32 eglError = 0;
        char note[128] = {};
    };

    // ANGLE's FunctionsEGL and Chrome's GLFenceAndroidNativeFenceSync / GbmSurfacelessWayland,
    // reduced to the calls they make on the native EGL.
    void RunChromeGpuFence(ChromeFenceReport& r) {
        using CreateSyncFn = EGLSyncKHR (*)(EGLDisplay, EGLenum, const EGLint*);
        using DestroySyncFn = EGLBoolean (*)(EGLDisplay, EGLSyncKHR);
        using ClientWaitFn = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
        using GetSyncAttribFn = EGLBoolean (*)(EGLDisplay, EGLSyncKHR, EGLint, EGLint*);
        using WaitSyncFn = EGLint (*)(EGLDisplay, EGLSyncKHR, EGLint);
        using DupFn = EGLint (*)(EGLDisplay, EGLSyncKHR);
        static int fakeGbmDevice[16] = {};
        const EGLDisplay dpy = EGL::GetPlatformDisplay(kPlatformGbm, fakeGbmDevice, nullptr);
        if (dpy == EGL_NO_DISPLAY) return;
        EGLint major = 0, minor = 0;
        r.initialized = EGL::Initialize(dpy, &major, &minor) == EGL_TRUE ? 1 : 0;
        if (!r.initialized) return;
        const char* exts = EGL::QueryString(dpy, EGL_EXTENSIONS);
        r.fenceSyncExtension = HasToken(exts, "EGL_KHR_fence_sync") ? 1 : 0;
        r.waitSyncExtension = HasToken(exts, "EGL_KHR_wait_sync") ? 1 : 0;
        r.nativeFenceExtension = HasToken(exts, "EGL_ANDROID_native_fence_sync") ? 1 : 0;
        const auto createSync = reinterpret_cast<CreateSyncFn>(EGL::GetProcAddress("eglCreateSyncKHR"));
        const auto destroySync = reinterpret_cast<DestroySyncFn>(EGL::GetProcAddress("eglDestroySyncKHR"));
        const auto clientWait = reinterpret_cast<ClientWaitFn>(EGL::GetProcAddress("eglClientWaitSyncKHR"));
        const auto getSyncAttrib = reinterpret_cast<GetSyncAttribFn>(EGL::GetProcAddress("eglGetSyncAttribKHR"));
        const auto waitSync = reinterpret_cast<WaitSyncFn>(EGL::GetProcAddress("eglWaitSyncKHR"));
        const auto dupFence = reinterpret_cast<DupFn>(EGL::GetProcAddress("eglDupNativeFenceFDANDROID"));
        r.entryPoints = createSync && destroySync && clientWait && getSyncAttrib && waitSync && dupFence ? 1 : 0;
        if (!r.entryPoints) return;

        // A fence command needs a current context.
        r.noContextRefused = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr) == EGL_NO_SYNC_KHR &&
                                     EGL::GetError() == EGL_BAD_MATCH
                                 ? 1
                                 : 0;

        // Chrome's GPU process: ES through ANGLE's identity, surfaceless, no config.
        (void)EGL::BindAPI(EGL_OPENGL_ES_API);
        const EGLint es3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        const EGLContext ctx = EGL::CreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, es3);
        if (ctx == EGL_NO_CONTEXT) {
            r.eglError = EGL::GetError();
            return;
        }
        r.current = EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE ? 1 : 0;
        if (!r.current) {
            r.eglError = EGL::GetError();
            return;
        }

        // A GBM scanout buffer (a shared image), bound as a texture and rendered into through a
        // framebuffer - and then no glFlush, as Chrome does when it has native fences.
        auto* backend = MG_Backend::pActiveBackendObject.get();
        MG_Backend::SharedImageExport image;
        EGLImage eglImage = EGL_NO_IMAGE;
        using GenFn = void (*)(int, unsigned*);
        using BindFn = void (*)(unsigned, unsigned);
        using ImageTargetFn = void (*)(unsigned, void*);
        using FbTexFn = void (*)(unsigned, unsigned, unsigned, unsigned, int);
        using StatusFn = unsigned (*)(unsigned);
        using ClearColorFn = void (*)(float, float, float, float);
        using ClearFn = void (*)(unsigned);
        const auto genTextures = reinterpret_cast<GenFn>(EGL::GetProcAddress("glGenTextures"));
        const auto bindTexture = reinterpret_cast<BindFn>(EGL::GetProcAddress("glBindTexture"));
        const auto imageTarget = reinterpret_cast<ImageTargetFn>(EGL::GetProcAddress("glEGLImageTargetTexture2DOES"));
        const auto genFramebuffers = reinterpret_cast<GenFn>(EGL::GetProcAddress("glGenFramebuffers"));
        const auto bindFramebuffer = reinterpret_cast<BindFn>(EGL::GetProcAddress("glBindFramebuffer"));
        const auto framebufferTexture = reinterpret_cast<FbTexFn>(EGL::GetProcAddress("glFramebufferTexture2D"));
        const auto checkStatus = reinterpret_cast<StatusFn>(EGL::GetProcAddress("glCheckFramebufferStatus"));
        const auto clearColor = reinterpret_cast<ClearColorFn>(EGL::GetProcAddress("glClearColor"));
        const auto clear = reinterpret_cast<ClearFn>(EGL::GetProcAddress("glClear"));
        if (backend != nullptr && backend->AllocateSharedImage(64, 32, kArgb8888, &image) && image.Fd >= 0) {
            const EGLAttrib attribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 32, EGL_LINUX_DRM_FOURCC_EXT, kArgb8888,
                                         EGL_DMA_BUF_PLANE0_FD_EXT, image.Fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                         static_cast<EGLAttrib>(image.Offset), EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                         static_cast<EGLAttrib>(image.Stride), EGL_NONE};
            eglImage = EGL::CreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
        }
        if (eglImage != EGL_NO_IMAGE && genTextures && bindTexture && imageTarget && genFramebuffers &&
            bindFramebuffer && framebufferTexture && checkStatus && clearColor && clear) {
            unsigned texture = 0, framebuffer = 0;
            genTextures(1, &texture);
            bindTexture(0x0DE1 /* GL_TEXTURE_2D */, texture);
            imageTarget(0x0DE1, eglImage);
            r.imageBound = 1;
            genFramebuffers(1, &framebuffer);
            bindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, framebuffer);
            framebufferTexture(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, 0x0DE1, texture, 0);
            r.framebufferComplete = checkStatus(0x8D40) == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */ ? 1 : 0;
            clearColor(0.0f, 1.0f, 1.0f, 1.0f);
            clear(0x4000);
        }

        // GLFenceAndroidNativeFenceSync::CreateForGpuFence, then GetGpuFenceHandle.
        const EGLSyncKHR fence = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        r.fenceCreated = fence != EGL_NO_SYNC_KHR ? 1 : 0;
        if (!r.fenceCreated) {
            r.eglError = EGL::GetError();
        } else {
            EGLint value = 0;
            if (getSyncAttrib(dpy, fence, EGL_SYNC_TYPE_KHR, &value)) r.fenceType = value;
            r.fenceWaited = clientWait(dpy, fence, 0, EGL_FOREVER_KHR) == EGL_CONDITION_SATISFIED_KHR ? 1 : 0;
            if (getSyncAttrib(dpy, fence, EGL_SYNC_STATUS_KHR, &value)) r.fenceSignaled = value == EGL_SIGNALED_KHR;
            r.dupFd = dupFence(dpy, fence);
            r.dupError = r.dupFd < 0 ? EGL::GetError() : EGL_SUCCESS;
            if (r.dupFd >= 0) {
                pollfd entry{r.dupFd, POLLIN, 0};
                r.dupSignaled = ::poll(&entry, 1, 5000) == 1 ? 1 : 0;
                // GLFenceAndroidNativeFenceSync::CreateFromGpuFenceHandle + ServerWait: the
                // descriptor imported back (the sync takes it) and waited on.
                const EGLint importAttribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, r.dupFd, EGL_NONE};
                const EGLSyncKHR imported = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, importAttribs);
                r.importCreated = imported != EGL_NO_SYNC_KHR ? 1 : 0;
                if (imported != EGL_NO_SYNC_KHR) {
                    r.importWaited = waitSync(dpy, imported, 0) == EGL_TRUE ? 1 : 0;
                    (void)destroySync(dpy, imported);
                } else {
                    ::close(r.dupFd);
                }
            }
            (void)destroySync(dpy, fence);
        }

        if (eglImage != EGL_NO_IMAGE) (void)EGL::DestroyImage(dpy, eglImage);
        if (image.Fd >= 0) ::close(image.Fd);
        if (backend != nullptr && image.Id != 0) (void)backend->ReleaseSharedImage(image.Id);
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, ctx);
        (void)EGL::Terminate(dpy);
    }

    struct InprocReport {
        Int32 initialized = 0;
        Int32 current = 0;
        Int32 dmabufImport = 0;
        Int32 nativeFence = 0;
        Int32 externalImage = 0;
        Int32 allocated = 0;
        Int32 eglError = 0;
        char note[128] = {};
    };

    // An inproc client: its server is this process, so nothing that carries memory between
    // processes is offered, asked for or allocated.
    void RunInproc(InprocReport& r) {
        static int fakeGbmDevice[16] = {};
        const EGLDisplay dpy = EGL::GetPlatformDisplay(kPlatformGbm, fakeGbmDevice, nullptr);
        EGLint major = 0, minor = 0;
        r.initialized = dpy != EGL_NO_DISPLAY && EGL::Initialize(dpy, &major, &minor) == EGL_TRUE ? 1 : 0;
        if (!r.initialized) return;
        const char* exts = EGL::QueryString(dpy, EGL_EXTENSIONS);
        r.dmabufImport = HasToken(exts, "EGL_EXT_image_dma_buf_import") ? 1 : 0;
        r.nativeFence = HasToken(exts, "EGL_ANDROID_native_fence_sync") ? 1 : 0;
        (void)EGL::BindAPI(EGL_OPENGL_API);
        const EGLint core33[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
        const EGLContext ctx = EGL::CreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, core33);
        r.current = ctx != EGL_NO_CONTEXT && EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
        if (!r.current) {
            r.eglError = EGL::GetError();
            return;
        }
        // The GL extension list is where a split client asks the server whether it holds YUV images.
        r.externalImage = HasGlExtension("GL_OES_EGL_image_external") ? 1 : 0;
        // Asked anyway (a C-ABI or GLX caller does not look at the extensions first): refused.
        auto* backend = MG_Backend::pActiveBackendObject.get();
        MG_Backend::SharedImageExport image;
        if (backend != nullptr && backend->AllocateSharedImage(64, 32, kXrgb8888, &image)) {
            r.allocated = 1;
            if (image.Fd >= 0) ::close(image.Fd);
            (void)backend->ReleaseSharedImage(image.Id);
        }
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, ctx);
        (void)EGL::Terminate(dpy);
    }

    // The supervisor on an abstract unix socket, as the device's is: shared images travel as
    // descriptors on its aux socket, which a TCP endpoint does not have.
    bool LaunchUnixSupervisor(ServerProcess* server) {
        server->logBase = "/tmp/mgl-p12-glamor-" + std::to_string(::getpid()) + ".log";
        ::setenv("MOBILEGL_LOG_FILE_PATH", server->logBase.c_str(), 1);
        PinHeadlessEgl();
        // The listener takes the native address; the client's selector carries the transport prefix.
        const std::string address = "@mgl-glamor-test-" + std::to_string(::getpid());
        server->endpoint = "unix:" + address;
        Debug::TruncateRoleLogs(server->logBase.c_str());
        Remote::Server::LaunchedServer launched;
        if (Remote::Server::LaunchServerWithArgs(ServerImage(), address, {"--serve"}, &launched) != MOBILEGL_OK)
            return false;
        server->pid = launched.pid;
        return WaitFor([&] { return server->Log().find("listening on") != std::string::npos; }, 10000);
    }

    // One forked client running `run` against the server, its report back through a pipe.
    // `server == nullptr` runs the client inproc, its server in its own process.
    template <class Report>
    Report RunPeer(const ServerProcess* server, const std::string& clientLogBase, void (*run)(Report&)) {
        Report report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", clientLogBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", server != nullptr ? "spawn" : "inproc", 1);
            if (server != nullptr) ::setenv("MOBILEGL_IPC_CONTROL", server->endpoint.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
            ::setenv("MOBILEGL_IPC_SURFACE", "offscreen", 1);
            Report r{};
            run(r);
            const ssize_t wrote = ::write(fds[1], &r, sizeof(r));
            (void)wrote;
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(fds[1]);
        if (pid < 0) {
            ::close(fds[0]);
            return report;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        if (::poll(&pfd, 1, 90000) > 0) {
            if (::read(fds[0], &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report))) {
                report = Report{};
                std::snprintf(report.note, sizeof(report.note), "peer died before reporting");
            }
        } else {
            std::snprintf(report.note, sizeof(report.note), "peer timed out");
            ::kill(pid, SIGKILL);
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }
} // namespace

TEST(GlamorEglSequence, XwaylandFindsEverythingGlamorNeeds) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixSupervisor(&server)) << server.Log();
    const std::string clientLog = "/tmp/mgl-glamor-client-" + std::to_string(::getpid()) + ".log";
    Debug::TruncateRoleLogs(clientLog.c_str());
    const GlamorReport r = RunPeer<GlamorReport>(&server, clientLog, RunGlamor);
    ASSERT_EQ(r.display, 1) << r.note;
    EXPECT_EQ(r.platformGbmExtension, 1);
    ASSERT_EQ(r.initialized, 1);
    EXPECT_EQ(r.dmabufCapable, 1) << "glamor would not be dmabuf_capable: no linux-dmabuf window buffers";
    ASSERT_EQ(r.noConfigCoreContext, 1) << "eglCreateContext(EGL_NO_CONFIG_KHR) error 0x" << std::hex << r.eglError;
    ASSERT_EQ(r.surfacelessCurrent, 1) << "surfaceless eglMakeCurrent error 0x" << std::hex << r.eglError;
    std::printf("[ glamor ] renderer='%s' GL %d.%d\n", r.renderer, r.glVersion / 10, r.glVersion % 10);
    EXPECT_EQ(r.rendererNotSoftware, 1) << r.renderer;
    EXPECT_EQ(r.desktopGl, 1);
    EXPECT_GE(r.glVersion, 21);
    EXPECT_EQ(r.coreProfile, 1);
    EXPECT_EQ(r.vertexArrayObject, 1);
    EXPECT_EQ(r.oesEglImage, 1);
    EXPECT_EQ(r.xrgbImportable, 1);
    EXPECT_EQ(r.argbImportable, 1);
    EXPECT_EQ(r.imageCreated, 1) << "a shared image's descriptor did not import as an EGLImage";
    EXPECT_EQ(r.flushAnswered, 1) << "the shared_image Flush verb was not answered";
    EXPECT_EQ(r.flushAgainAnswered, 1);
    std::error_code ec;
    if (!::testing::Test::HasFailure()) {
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Client), ec);
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Server), ec);
        std::filesystem::remove(clientLog, ec);
    }
}

TEST(GlamorEglSequence, ChromesGpuProcessGetsANativeFenceOfItsDmaBufWrites) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixSupervisor(&server)) << server.Log();
    const std::string clientLog = "/tmp/mgl-chrome-fence-client-" + std::to_string(::getpid()) + ".log";
    Debug::TruncateRoleLogs(clientLog.c_str());
    const ChromeFenceReport r = RunPeer<ChromeFenceReport>(&server, clientLog, RunChromeGpuFence);
    ASSERT_EQ(r.initialized, 1) << r.note;
    // ANGLE offers Chrome fences only with all three, and loads the KHR names.
    EXPECT_EQ(r.fenceSyncExtension, 1);
    EXPECT_EQ(r.waitSyncExtension, 1);
    EXPECT_EQ(r.nativeFenceExtension, 1);
    ASSERT_EQ(r.entryPoints, 1) << "eglCreateSyncKHR & co. or eglDupNativeFenceFDANDROID did not resolve";
    EXPECT_EQ(r.noContextRefused, 1);
    ASSERT_EQ(r.current, 1) << "error 0x" << std::hex << r.eglError;
    std::printf("[ chrome ] image bound %d, framebuffer complete %d, fence descriptor %d (error 0x%x)\n",
                r.imageBound, r.framebufferComplete, r.dupFd, r.dupError);
    // The fence command reaches the server, which publishes the context's image writes and answers.
    ASSERT_EQ(r.fenceCreated, 1) << "the native fence was refused, error 0x" << std::hex << r.eglError;
    EXPECT_EQ(r.fenceType, EGL_SYNC_NATIVE_FENCE_ANDROID);
    EXPECT_EQ(r.fenceWaited, 1);
    EXPECT_EQ(r.fenceSignaled, 1);
    if (r.dupFd >= 0) {
        // A sync_file of the work, which signals; imported back it is waited on.
        EXPECT_EQ(r.dupSignaled, 1);
        EXPECT_EQ(r.importCreated, 1);
        EXPECT_EQ(r.importWaited, 1);
    } else {
        // A server that cannot export fences (a host driver without them) waits the work out and
        // has no descriptor: the copy is refused by name, not answered with garbage.
        EXPECT_EQ(r.dupError, EGL_BAD_PARAMETER);
    }
    std::error_code ec;
    if (!::testing::Test::HasFailure()) {
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Client), ec);
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Server), ec);
        std::filesystem::remove(clientLog, ec);
    }
}

TEST(GlamorEglSequence, AnInprocClientIsOfferedNoSharedImagesAndAllocatesNone) {
    PinHeadlessEgl();
    const std::string clientLog = "/tmp/mgl-inproc-images-" + std::to_string(::getpid()) + ".log";
    Debug::TruncateRoleLogs(clientLog.c_str());
    const InprocReport r = RunPeer<InprocReport>(nullptr, clientLog, RunInproc);
    ASSERT_EQ(r.initialized, 1) << r.note;
    ASSERT_EQ(r.current, 1) << "error 0x" << std::hex << r.eglError;
    EXPECT_EQ(r.dmabufImport, 0) << "an inproc display offered dma-buf import";
    EXPECT_EQ(r.nativeFence, 0) << "an inproc display offered native fences";
    EXPECT_EQ(r.externalImage, 0) << "an inproc context offered external (YUV) images";
    EXPECT_EQ(r.allocated, 0) << "an inproc client was given a shared image";
    std::string log;
    for (const auto role : {Debug::LogRole::Client, Debug::LogRole::Server}) {
        std::ifstream in(Debug::RoleLogPath(clientLog.c_str(), role));
        log += std::string(std::istreambuf_iterator<char>(in), {});
    }
    EXPECT_EQ(log.find("Shared images: the server"), std::string::npos) << "the YUV probe allocated an image";
    EXPECT_NE(log.find("shared-image allocation was refused: the client is this process"), std::string::npos)
        << "the server did not refuse the allocation by name";
    std::error_code ec;
    if (!::testing::Test::HasFailure()) {
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Client), ec);
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Server), ec);
        std::filesystem::remove(clientLog, ec);
    }
}
