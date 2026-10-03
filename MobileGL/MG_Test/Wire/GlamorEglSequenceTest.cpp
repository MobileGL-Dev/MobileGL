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

#include "P12ServerRig.h"

#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>

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

    GlamorReport RunGlamorPeer(const ServerProcess& server, const std::string& clientLogBase) {
        GlamorReport report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", clientLogBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", server.endpoint.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
            ::setenv("MOBILEGL_IPC_SURFACE", "offscreen", 1);
            GlamorReport r{};
            RunGlamor(r);
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
                report = GlamorReport{};
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
    const GlamorReport r = RunGlamorPeer(server, clientLog);
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
