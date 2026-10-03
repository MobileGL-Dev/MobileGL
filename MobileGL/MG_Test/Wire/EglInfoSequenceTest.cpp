// MobileGL - MobileGL/MG_Test/Wire/EglInfoSequenceTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// `eglinfo -B` AGAINST MOBILEGL, END TO END ON THE HOST: a real MobileGL client (the EGL entry
// points, the spawn/tcp session) against a real `--serve` supervisor, driven through the exact call
// sequence the distribution's eglinfo runs per display:
//
//   eglInitialize; EGL_CLIENT_APIS must name "OpenGL" / "OpenGL_ES"; EGL_KHR_create_context decides
//   the attribute form. Then, per API:
//     eglBindAPI(api)
//     eglChooseConfig({EGL_CONFORMANT, bit, EGL_RED/GREEN/BLUE/ALPHA_SIZE 1, EGL_RENDERABLE_TYPE,
//                      bit}) and take configs[0], or NULL when nothing matched
//     desktop: eglCreateContext({MAJOR, MINOR, PROFILE_MASK}) from 4.6 down (core stops at 3.1)
//     ES:      eglCreateContext({EGL_CONTEXT_MAJOR_VERSION, n}) for n = 3, 2, 1
//     eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)   - surfaceless
//     glGetString(GL_VENDOR / GL_RENDERER / GL_VERSION) through eglGetProcAddress
//
// On the device eglinfo printed MobileGL's EGL lines for every platform and no GL lines at all. Two
// refusals, both red here before the fix:
//   * the surfaceless eglMakeCurrent was EGL_BAD_MATCH, so no desktop context ever became current;
//   * the ES probe's bit is EGL_OPENGL_ES_BIT (ES 1.x), which no MobileGL config claims, so the
//     config is NULL and eglCreateContext answered EGL_BAD_CONFIG.

#include "P12ServerRig.h"

#include <MG_Impl/EGLImpl/EGLImpl.h>

using namespace MobileGL;
using namespace P12;

namespace {

    namespace EGL = MobileGL::MG_Impl::EGLImpl;

    constexpr unsigned kGlVendor = 0x1F00;
    constexpr unsigned kGlRenderer = 0x1F01;
    constexpr unsigned kGlVersion = 0x1F02;

    // One "OpenGL ... profile" block of eglinfo's output, or why it is missing.
    struct ProfileReport {
        Int32 configFound = 0;   // eglChooseConfig matched something
        Int32 created = 0;       // some eglCreateContext in the version walk returned a context
        Int32 madeCurrent = 0;   // ...and the surfaceless eglMakeCurrent took it
        Int32 lastError = 0;     // eglGetError after the last failed create / make-current
        Int32 drawIsNone = 0;    // eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE while current
        Int32 major = 0;
        Int32 minor = 0;
        char vendor[96] = {};
        char renderer[96] = {};
        char version[96] = {};
    };

    struct EglInfoReport {
        Int32 initialized = 0;
        Int32 eglMajor = 0;
        Int32 eglMinor = 0;
        Int32 hasOpenGL = 0;
        Int32 hasOpenGLES = 0;
        Int32 khrCreateContext = 0;
        Int32 surfacelessExtensions = 0; // EGL_KHR_surfaceless_context and EGL_KHR_no_config_context advertised
        ProfileReport core{};
        ProfileReport compat{};
        ProfileReport es{};
        char note[128] = {};
    };

    // eglinfo's extension_supported(): a whole space-separated token.
    bool HasToken(const char* list, const char* token) {
        if (list == nullptr) return false;
        const std::string all = std::string(" ") + list + " ";
        return all.find(std::string(" ") + token + " ") != std::string::npos;
    }

    EGLConfig ChooseLikeEglInfo(EGLDisplay dpy, EGLint apiBit, ProfileReport& out) {
        const EGLint attribs[] = {EGL_CONFORMANT, apiBit, EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1,
                                  EGL_ALPHA_SIZE, 1, EGL_RENDERABLE_TYPE, apiBit, EGL_NONE};
        EGLConfig configs[64] = {};
        EGLint count = 0;
        (void)EGL::ChooseConfig(dpy, attribs, configs, 64, &count);
        out.configFound = count > 0 ? 1 : 0;
        return count > 0 ? configs[0] : nullptr;
    }

    // eglinfo's doOneContext, minus the printing.
    void ReadStrings(ProfileReport& out) {
        using GetStringFn = const unsigned char* (*)(unsigned);
        const auto getString = reinterpret_cast<GetStringFn>(EGL::GetProcAddress("glGetString"));
        if (getString == nullptr) return;
        const auto copy = [&](unsigned name, char* into, size_t size) {
            const unsigned char* text = getString(name);
            std::snprintf(into, size, "%s", text != nullptr ? reinterpret_cast<const char*>(text) : "");
        };
        copy(kGlVendor, out.vendor, sizeof(out.vendor));
        copy(kGlRenderer, out.renderer, sizeof(out.renderer));
        copy(kGlVersion, out.version, sizeof(out.version));
    }

    // Returns the context made current, or EGL_NO_CONTEXT - eglinfo's createEGLContext.
    EGLContext TryCurrent(EGLDisplay dpy, EGLConfig config, const EGLint* attribs, ProfileReport& out) {
        const EGLContext ctx = EGL::CreateContext(dpy, config, EGL_NO_CONTEXT, attribs);
        if (ctx == EGL_NO_CONTEXT) {
            out.lastError = EGL::GetError();
            return EGL_NO_CONTEXT;
        }
        out.created = 1;
        if (EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
            out.lastError = EGL::GetError();
            (void)EGL::DestroyContext(dpy, ctx);
            return EGL_NO_CONTEXT;
        }
        out.madeCurrent = 1;
        out.drawIsNone = EGL::GetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE ? 1 : 0;
        return ctx;
    }

    void Release(EGLDisplay dpy, EGLContext ctx, ProfileReport& out) {
        ReadStrings(out);
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, ctx);
    }

    void DesktopProfile(EGLDisplay dpy, EGLConfig config, bool core, bool khrCreateContext, ProfileReport& out) {
        static const int kVersions[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3},
                                           {3, 2}, {3, 1}, {3, 0}, {2, 1}, {2, 0}, {1, 5}, {1, 4}, {1, 3},
                                           {1, 2}, {1, 1}, {1, 0}};
        for (const auto& v : kVersions) {
            if (core && v[0] == 3 && v[1] == 0) return;
            const EGLint withProfile[] = {EGL_CONTEXT_MAJOR_VERSION, v[0], EGL_CONTEXT_MINOR_VERSION, v[1],
                                          EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                          core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
                                               : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
                                          EGL_NONE};
            const EGLint old[] = {EGL_CONTEXT_CLIENT_VERSION, v[0], EGL_NONE};
            const EGLContext ctx = TryCurrent(dpy, config, khrCreateContext ? withProfile : old, out);
            if (ctx != EGL_NO_CONTEXT) {
                out.major = v[0];
                out.minor = v[1];
                Release(dpy, ctx, out);
                return;
            }
        }
    }

    void EsProfile(EGLDisplay dpy, EGLConfig config, ProfileReport& out) {
        for (int major = 3; major > 0; --major) {
            const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_NONE};
            const EGLContext ctx = TryCurrent(dpy, config, attribs, out);
            if (ctx != EGL_NO_CONTEXT) {
                out.major = major;
                Release(dpy, ctx, out);
                return;
            }
        }
    }

    EglInfoReport RunEglInfoPeer(const ServerProcess& server, const std::string& clientLogBase) {
        EglInfoReport report{};
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
            ::unsetenv("MOBILEGL_IPC_SURFACE");
            EglInfoReport r{};
            // eglinfo walks every platform; surfaceless is the one a display-less container has.
            const EGLDisplay dpy = EGL::GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            EGLint major = 0;
            EGLint minor = 0;
            if (dpy == EGL_NO_DISPLAY || EGL::Initialize(dpy, &major, &minor) != EGL_TRUE) {
                std::snprintf(r.note, sizeof(r.note), "eglInitialize failed (no spawn session?)");
            } else {
                r.initialized = 1;
                r.eglMajor = major;
                r.eglMinor = minor;
                const char* apis = EGL::QueryString(dpy, EGL_CLIENT_APIS);
                const char* exts = EGL::QueryString(dpy, EGL_EXTENSIONS);
                r.hasOpenGL = HasToken(apis, "OpenGL") ? 1 : 0;
                r.hasOpenGLES = HasToken(apis, "OpenGL_ES") ? 1 : 0;
                const bool khr = major == 1 && minor >= 4 && HasToken(exts, "EGL_KHR_create_context");
                r.khrCreateContext = khr ? 1 : 0;
                r.surfacelessExtensions =
                    HasToken(exts, "EGL_KHR_surfaceless_context") && HasToken(exts, "EGL_KHR_no_config_context") ? 1 : 0;
                if (r.hasOpenGL && EGL::BindAPI(EGL_OPENGL_API) == EGL_TRUE) {
                    const EGLConfig config = ChooseLikeEglInfo(dpy, EGL_OPENGL_BIT, r.core);
                    r.compat.configFound = r.core.configFound;
                    if (khr) DesktopProfile(dpy, config, /*core=*/true, khr, r.core);
                    DesktopProfile(dpy, config, /*core=*/false, khr, r.compat);
                }
                if (r.hasOpenGLES && EGL::BindAPI(EGL_OPENGL_ES_API) == EGL_TRUE) {
                    const EGLConfig config = ChooseLikeEglInfo(dpy, EGL_OPENGL_ES_BIT, r.es);
                    EsProfile(dpy, config, r.es);
                }
                (void)EGL::Terminate(dpy);
            }
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
                report = EglInfoReport{};
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

    std::string Describe(const char* label, const ProfileReport& p) {
        char text[512];
        std::snprintf(text, sizeof(text),
                      "%s: config=%d created=%d current=%d eglError=0x%04x version=%d.%d vendor='%s' renderer='%s' "
                      "GL_VERSION='%s'",
                      label, p.configFound, p.created, p.madeCurrent, p.lastError, p.major, p.minor, p.vendor,
                      p.renderer, p.version);
        return text;
    }

    void ExpectProfilePrinted(const char* label, const ProfileReport& p) {
        EXPECT_EQ(p.created, 1) << Describe(label, p);
        EXPECT_EQ(p.madeCurrent, 1) << "the surfaceless eglMakeCurrent was refused - " << Describe(label, p);
        EXPECT_EQ(p.drawIsNone, 1) << "a surfaceless binding reported a current draw surface - " << Describe(label, p);
        EXPECT_NE(p.renderer[0], '\0') << "no renderer line - " << Describe(label, p);
        EXPECT_NE(p.version[0], '\0') << "no version line - " << Describe(label, p);
    }

} // namespace

TEST(EglInfoSequence, EveryProfilePrintsItsRendererAndVersion) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("eglinfo", &server)) << server.Log();
    const std::string clientLog = "/tmp/mgl-eglinfo-client-" + std::to_string(::getpid()) + ".log";
    Debug::TruncateRoleLogs(clientLog.c_str());
    const EglInfoReport r = RunEglInfoPeer(server, clientLog);
    ASSERT_EQ(r.initialized, 1) << r.note;
    // eglinfo only takes the KHR_create_context attribute form on EGL >= 1.4 with the extension.
    EXPECT_EQ(r.khrCreateContext, 1) << "EGL " << r.eglMajor << "." << r.eglMinor;
    ASSERT_EQ(r.hasOpenGL, 1);
    ASSERT_EQ(r.hasOpenGLES, 1);
    EXPECT_EQ(r.surfacelessExtensions, 1) << "the display does not advertise what it now accepts";
    std::printf("[ eglinfo ] %s\n[ eglinfo ] %s\n[ eglinfo ] %s\n", Describe("core", r.core).c_str(),
                Describe("compat", r.compat).c_str(), Describe("es", r.es).c_str());
    if (r.core.madeCurrent == 1 && r.core.renderer[0] == '\0')
        GTEST_SKIP() << "the surfaceless binding was accepted; the strings need headless EGL on the server";
    ExpectProfilePrinted("OpenGL core profile", r.core);
    ExpectProfilePrinted("OpenGL compatibility profile", r.compat);
    ExpectProfilePrinted("OpenGL ES profile", r.es);
    std::error_code ec;
    if (!::testing::Test::HasFailure()) {
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Client), ec);
        std::filesystem::remove(Debug::RoleLogPath(clientLog.c_str(), Debug::LogRole::Server), ec);
        std::filesystem::remove(clientLog, ec);
    }
}
