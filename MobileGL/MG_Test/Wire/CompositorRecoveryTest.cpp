// MobileGL - MobileGL/MG_Test/Wire/CompositorRecoveryTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A COMPOSITOR OUTLIVES ITS LOST SESSION. The compositor of the anland desktop renders into the
// SERVER's window (MOBILEGL_IPC_SURFACE=server: a ServerOwned window surface, which its session holds
// the display's window lease for) and samples its clients' frames as shared images it imports from
// their dma-bufs. ClientRecoveryTest is an application's recovery; this is the compositor's, against
// the in-process display server with a stand-in window (a host has no window a backend can draw into,
// so the server gives the ServerOwned surface a pbuffer of the window's extent - the seam is
// ServerLoop's). The hand-over of the window lease from a lost session that is still tearing down is
// ServerDisplayTest's and ServerLoopTest's: the client hears of a loss it is blocked in only when the
// lost session hangs up, which is after that session ended its lease.
//
// What it asserts, from each side:
//   * the compositor's session is lost (the pid-targeted knob) and the compositor, recovering the
//     way KWin does - its contexts recreated, naming the lost share context, and the surface it kept
//     made current again - gets the server's window again on a FRESH session, asked for at the size
//     the compositor asked for (none: the window's own), not fixed at the extent it had;
//   * the surface renders on the fresh session (a clear, read back) and swaps;
//   * the other client's shared image survives the loss and is imported again; the lost session's
//     EGLImage of it can no longer be bound, and destroying it - after the same image was imported
//     again under the same id - puts nothing on the fresh session's wire (it must not drop the new
//     reference);
//   * the image's owner is untouched: its session is not lost, it renders, and the server still
//     identifies its image.

#include "P12ServerRig.h"

#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Remote/Server/ServerDisplay.h>

#include <fcntl.h>
#include <sys/uio.h>

extern "C" {
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
GLenum glGetError(void);
GLenum glGetGraphicsResetStatus(void);
void glGenTextures(GLsizei n, GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
void glFlush(void);
}

using namespace MobileGL;
using namespace P12;

namespace {

    namespace EGL = MobileGL::MG_Impl::EGLImpl;
    namespace Server = MobileGL::MG_Remote::Server;

    constexpr Uint32 kGreen = 0xFF00FF00u;
    constexpr Uint32 kBlue = 0xFFFF0000u;
    constexpr Uint32 kRed = 0xFF0000FFu;
    constexpr Uint32 kUntouched = 0x5A5A5A5Au;
    constexpr Uint32 kXrgb8888 = 0x34325258u;
    constexpr Uint32 kWindowWidth = 64;
    constexpr Uint32 kWindowHeight = 48;
    constexpr Uint32 kImageWidth = 32;
    constexpr Uint32 kImageHeight = 16;

    // KWin's context shape: desktop GL 3.1 that asks to be told of a reset.
    const EGLint kRobustAttribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                     3,
                                     EGL_CONTEXT_MINOR_VERSION,
                                     1,
                                     EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY,
                                     EGL_LOSE_CONTEXT_ON_RESET,
                                     EGL_NONE};

    Uint32 ReadPixel() {
        Uint32 pixel = kUntouched;
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
        return pixel;
    }

    Uint32 ClearAndRead(GLfloat red, GLfloat green, GLfloat blue) {
        glClearColor(red, green, blue, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return ReadPixel();
    }

    Uint64 PublishedSeq() {
        auto* session = Remote::Client::ClientSession::Active();
        return session != nullptr ? session->LastPublishedSeq() : 0;
    }

    // ---- the server process -----------------------------------------------------------------

    int g_window = 0;
    std::string g_geometryPath;

    void NoWindowReference(void*, void*) {}

    // Every geometry the display is asked for, one "W H" line each, for the parent to read.
    void RecordGeometry(void*, Uint32 width, Uint32 height) {
        const int fd = ::open(g_geometryPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return;
        const std::string line = std::to_string(width) + " " + std::to_string(height) + "\n";
        const ssize_t wrote = ::write(fd, line.data(), line.size());
        (void)wrote;
        ::close(fd);
    }

    Bool PbufferForTheServerWindow(MG_Backend::BackendObject* backend, EGLSurface surface,
                                   const MG_Backend::WindowHandle& window) {
        return backend->CreateEGLPbufferSurface(surface, static_cast<EGLint>(window.Width),
                                                static_cast<EGLint>(window.Height));
    }

    // RunUnixInProcessServerProcess (P12ServerRig.h), with a display and its stand-in window.
    [[noreturn]] void RunDisplayServerProcess(const std::string& endpoint, const std::string& logBase) {
        ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
        ::setenv("MOBILEGL_IPC_DIAL", "no", 1);
        for (const char* name : {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB",
                                 "MOBILEGL_IPC_STAGE_MB", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_SURFACE",
                                 "MOBILEGL_IPC_INPROC_MAX_SESSIONS", "MOBILEGL_BACKEND_TYPE"})
            ::unsetenv(name);
        g_geometryPath = logBase + ".geometry";
        Server::ServerDisplayHooks hooks;
        hooks.acquire = &NoWindowReference;
        hooks.release = &NoWindowReference;
        hooks.requestGeometry = &RecordGeometry;
        Server::ServerDisplayInstance().Install(hooks);
        Server::ServerDisplayInstance().Attach(&g_window, kWindowWidth, kWindowHeight);
        Server::ServerLoop::SetServerOwnedSurfaceHookForTesting(&PbufferForTheServerWindow);
        sigset_t stop;
        sigemptyset(&stop);
        sigaddset(&stop, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stop, nullptr);
        std::atomic<bool> returned{false};
        std::atomic<int> served{-1};
        std::thread server([&] {
            served.store(mobilegl_server_serve_inprocess(endpoint.c_str()));
            returned.store(true);
        });
        bool stopping = false;
        while (!returned.load()) {
            timespec slice{0, 100 * 1000 * 1000};
            if (sigtimedwait(&stop, nullptr, &slice) == SIGTERM && !stopping) {
                stopping = true;
                mobilegl_server_stop_inprocess();
            }
        }
        server.join();
        std::fflush(nullptr);
        ::_exit(stopping && served.load() == 0 ? 0 : 100 + (served.load() & 0x7f));
    }

    bool LaunchDisplayServer(ServerProcess* server, const std::string& label) {
        server->endpoint = "@mgl-comprec-" + label + "-" + std::to_string(::getpid());
        server->logBase = "/tmp/mgl-comprec-" + label + "-" + std::to_string(::getpid()) + ".log";
        ::setenv("MOBILEGL_LOG_FILE_PATH", server->logBase.c_str(), 1);
        PinHeadlessEgl();
        Debug::TruncateRoleLogs(server->logBase.c_str());
        std::error_code ec;
        std::filesystem::remove(server->logBase + ".geometry", ec);
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) RunDisplayServerProcess(server->endpoint, server->logBase);
        server->pid = pid;
        bool listening = false;
        (void)WaitFor(
            [&] {
                if (server->Log().find("listening on") != std::string::npos) return listening = true;
                return server->WaitExit(0) && server->pid <= 0;
            },
            10000);
        if (listening) server->endpoint = "unix:" + server->endpoint;
        return listening;
    }

    // ---- the descriptor between the two clients ---------------------------------------------

    struct SharedImageMessage {
        Uint32 width = 0;
        Uint32 height = 0;
        Uint32 fourcc = 0;
        Uint32 stride = 0;
        Uint32 offset = 0;
        Uint64 id = 0;
    };

    bool SendFd(int sock, int fd, const SharedImageMessage& message) {
        iovec iov{const_cast<SharedImageMessage*>(&message), sizeof(message)};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
        return ::sendmsg(sock, &msg, 0) == static_cast<ssize_t>(sizeof(message));
    }

    bool ReceiveFd(int sock, int* fd, SharedImageMessage* message, int timeoutMs) {
        pollfd pfd{sock, POLLIN, 0};
        if (::poll(&pfd, 1, timeoutMs) <= 0) return false;
        iovec iov{message, sizeof(*message)};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (::recvmsg(sock, &msg, 0) != static_cast<ssize_t>(sizeof(*message))) return false;
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == nullptr || cmsg->cmsg_type != SCM_RIGHTS) return false;
        std::memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
        return true;
    }

    bool WaitForByte(int sock, int timeoutMs) {
        pollfd pfd{sock, POLLIN, 0};
        char byte = 0;
        return ::poll(&pfd, 1, timeoutMs) > 0 && ::read(sock, &byte, 1) == 1;
    }

    // ---- the clients --------------------------------------------------------------------------

    void SetClientEnvironment(const std::string& endpoint, const std::string& logBase, bool serverWindow) {
        ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
        ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
        ::setenv("MOBILEGL_IPC_CONTROL", endpoint.c_str(), 1);
        if (serverWindow) ::setenv("MOBILEGL_IPC_SURFACE", "server", 1);
        else ::unsetenv("MOBILEGL_IPC_SURFACE");
        ::unsetenv("MOBILEGL_BACKEND_TYPE");
        ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT");
        ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_PID");
    }

    EGLImage ImportImage(EGLDisplay dpy, int fd, const SharedImageMessage& image) {
        const EGLAttrib attribs[] = {EGL_WIDTH,
                                     static_cast<EGLAttrib>(image.width),
                                     EGL_HEIGHT,
                                     static_cast<EGLAttrib>(image.height),
                                     EGL_LINUX_DRM_FOURCC_EXT,
                                     static_cast<EGLAttrib>(image.fourcc),
                                     EGL_DMA_BUF_PLANE0_FD_EXT,
                                     fd,
                                     EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                     static_cast<EGLAttrib>(image.offset),
                                     EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                     static_cast<EGLAttrib>(image.stride),
                                     EGL_NONE};
        return EGL::CreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    }

    // A new texture with `image` as its level 0 (glEGLImageTargetTexture2DOES); the GL error it raised.
    Uint32 BindImageToNewTexture(EGLImage image) {
        using TargetFn = void (*)(GLenum, void*);
        const auto target = reinterpret_cast<TargetFn>(EGL::GetProcAddress("glEGLImageTargetTexture2DOES"));
        if (target == nullptr) return 0xFFFFFFFFu;
        while (glGetError() != GL_NO_ERROR) {
        }
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        target(GL_TEXTURE_2D, image);
        return glGetError();
    }

    // THE IMAGE'S OWNER (a Wayland client of the compositor): an offscreen session that allocates a
    // shared image and hands its descriptor over, then - once the compositor has recovered - checks
    // that nothing of the compositor's loss reached it.
    struct OwnerReport {
        Int32 initialized = 0;
        Int32 allocated = 0;
        Int32 sent = 0;
        Int32 sawDone = 0;
        Uint32 statusAfter = 0xFFFFFFFFu;
        Uint32 pixelAfter = 0;
        Int32 stillIdentified = -1;
        Uint64 imageId = 0;
        char note[256] = {};
    };

    void Note(OwnerReport& r, const char* text) {
        if (r.note[0] == '\0') std::snprintf(r.note, sizeof(r.note), "%s", text);
    }

    void RunOwner(int sock, OwnerReport& r) {
        const EGLDisplay dpy = EGL::GetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0;
        EGLint minor = 0;
        if (dpy == EGL_NO_DISPLAY || EGL::Initialize(dpy, &major, &minor) != EGL_TRUE) return Note(r, "no display");
        (void)EGL::BindAPI(EGL_OPENGL_API);
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (EGL::ChooseConfig(dpy, configAttribs, &config, 1, &count) != EGL_TRUE || count < 1)
            return Note(r, "no config");
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        const EGLSurface surface = EGL::CreatePbufferSurface(dpy, config, pbufferAttribs);
        const EGLContext context = EGL::CreateContext(dpy, config, EGL_NO_CONTEXT, kRobustAttribs);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            EGL::MakeCurrent(dpy, surface, surface, context) != EGL_TRUE)
            return Note(r, "the owner's context did not go current");
        r.initialized = 1;

        auto* backend = MG_Backend::pActiveBackendObject.get();
        MG_Backend::SharedImageExport image;
        if (backend == nullptr || !backend->AllocateSharedImage(kImageWidth, kImageHeight, kXrgb8888, &image) ||
            image.Fd < 0)
            return Note(r, "no shared image was allocated");
        r.allocated = 1;
        r.imageId = image.Id;
        // A frame of the client's own first (its session is as busy as a client's is).
        (void)ClearAndRead(0.0f, 1.0f, 0.0f);
        glFlush();
        SharedImageMessage message;
        message.width = image.Width;
        message.height = image.Height;
        message.fourcc = image.Fourcc;
        message.stride = image.Stride;
        message.offset = image.Offset;
        message.id = image.Id;
        r.sent = SendFd(sock, image.Fd, message) ? 1 : 0;
        if (!r.sent) return Note(r, "the descriptor could not be sent");

        r.sawDone = WaitForByte(sock, 60000) ? 1 : 0;
        r.statusAfter = glGetGraphicsResetStatus();
        r.pixelAfter = ClearAndRead(1.0f, 0.0f, 0.0f);
        Uint64 again = 0;
        r.stillIdentified =
            backend->ImportSharedImage(image.Fd, image.Width, image.Height, image.Fourcc, &again) && again == image.Id
                ? 1
                : 0;
        if (again != 0) (void)backend->ReleaseSharedImage(again);
        ::close(image.Fd);
        (void)backend->ReleaseSharedImage(image.Id);
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, context);
        (void)EGL::DestroySurface(dpy, surface);
        (void)EGL::Terminate(dpy);
    }

    // THE COMPOSITOR: KWin's EGL shape on the server's window, its loss and its restart.
    struct CompositorReport {
        Int32 received = 0;
        Int32 surfaceOk = 0;
        Int32 width = 0;
        Int32 height = 0;
        Int32 firstCurrent = 0;
        Int32 imported = 0;
        Uint64 importedId = 0;
        Uint32 firstBindError = 0xFFFFFFFFu;
        Uint32 firstPixel = 0;
        Int32 firstSwap = -1;
        Int32 lossSeen = 0;
        Uint32 resetStatus = 0;
        Int32 lostSwap = -1;
        Int32 lostSwapError = 0;
        Int32 newContextOk = 0;
        Int32 newCurrentOk = 0;
        Int32 newCurrentError = 0;
        Int64 rehomeMs = -1;
        Uint32 resetStatusNew = 0xFFFFFFFFu;
        Int32 widthAfter = 0;
        Int32 heightAfter = 0;
        Uint32 staleBindError = 0;
        Int32 reimported = 0;
        Uint64 reimportedId = 0;
        Int64 recordsByStaleDestroy = -1;
        Uint32 freshBindError = 0xFFFFFFFFu;
        Uint32 freshPixel = 0;
        Int32 freshSwap = -1;
        Uint32 resetStatusEnd = 0xFFFFFFFFu;
        char note[256] = {};
    };

    void Note(CompositorReport& r, const char* text) {
        if (r.note[0] == '\0') std::snprintf(r.note, sizeof(r.note), "%s", text);
    }

    void RunCompositor(int sock, CompositorReport& r) {
        int fd = -1;
        SharedImageMessage image;
        r.received = ReceiveFd(sock, &fd, &image, 30000) ? 1 : 0;
        if (!r.received) return Note(r, "no descriptor from the owner");

        // AnlandBackend::initialize: the display, an RGBA window config, the server's window.
        const EGLDisplay dpy = EGL::GetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0;
        EGLint minor = 0;
        if (dpy == EGL_NO_DISPLAY || EGL::Initialize(dpy, &major, &minor) != EGL_TRUE) return Note(r, "no display");
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                        EGL_RED_SIZE,     8,              EGL_GREEN_SIZE,      8,
                                        EGL_BLUE_SIZE,    8,              EGL_ALPHA_SIZE,      8,
                                        EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (EGL::ChooseConfig(dpy, configAttribs, &config, 1, &count) != EGL_TRUE || count < 1)
            return Note(r, "no window config");
        const EGLSurface surface = EGL::CreateWindowSurface(dpy, config, NativeWindowType{}, nullptr);
        r.surfaceOk = surface != EGL_NO_SURFACE ? 1 : 0;
        if (!r.surfaceOk) return Note(r, "the server's window surface was not created");
        (void)EGL::QuerySurface(dpy, surface, EGL_WIDTH, &r.width);
        (void)EGL::QuerySurface(dpy, surface, EGL_HEIGHT, &r.height);

        // EglBackend::createContext: a global share context, and the compositor's sharing with it.
        (void)EGL::BindAPI(EGL_OPENGL_API);
        const EGLContext share = EGL::CreateContext(dpy, config, EGL_NO_CONTEXT, kRobustAttribs);
        const EGLContext lost = EGL::CreateContext(dpy, config, share, kRobustAttribs);
        r.firstCurrent = share != EGL_NO_CONTEXT && lost != EGL_NO_CONTEXT &&
                                 EGL::MakeCurrent(dpy, surface, surface, lost) == EGL_TRUE
                             ? 1
                             : 0;
        if (!r.firstCurrent) return Note(r, "the compositor's context did not go current");

        // A client's buffer, imported and bound (the scene's surface texture), and a frame.
        const EGLImage stale = ImportImage(dpy, fd, image);
        r.imported = stale != EGL_NO_IMAGE ? 1 : 0;
        if (!r.imported) return Note(r, "the client's image was not imported");
        EGLint ignored = 0;
        (void)EGL::LookupSharedImage(stale, &r.importedId, &ignored, &ignored);
        r.firstBindError = BindImageToNewTexture(stale);
        r.firstPixel = ClearAndRead(0.0f, 1.0f, 0.0f);
        r.firstSwap = EGL::SwapBuffers(dpy, surface);

        // Compositing until the session is lost (the knob targets this pid).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until) {
            (void)ClearAndRead(0.0f, 1.0f, 0.0f);
            r.lostSwap = EGL::SwapBuffers(dpy, surface);
            r.lostSwapError = EGL::GetError();
            if (glGetGraphicsResetStatus() != GL_NO_ERROR) {
                r.lossSeen = 1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!r.lossSeen) return Note(r, "the compositor's session was never lost");
        r.resetStatus = glGetGraphicsResetStatus();
        // The swap that ran into the loss, if this frame's did not: one more, which must say so.
        r.lostSwap = EGL::SwapBuffers(dpy, surface);
        r.lostSwapError = EGL::GetError();

        // Compositor::reinitialize: the scene and the context go, a new context is created (still
        // naming the lost share context) and made current on the surface the compositor kept.
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, lost);
        const auto restart = std::chrono::steady_clock::now();
        const EGLContext fresh = EGL::CreateContext(dpy, config, share, kRobustAttribs);
        r.newContextOk = fresh != EGL_NO_CONTEXT ? 1 : 0;
        if (!r.newContextOk) return Note(r, "no context after the loss");
        r.newCurrentOk = EGL::MakeCurrent(dpy, surface, surface, fresh) == EGL_TRUE ? 1 : 0;
        r.newCurrentError = EGL::GetError();
        r.rehomeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restart).count();
        if (!r.newCurrentOk) return Note(r, "the kept surface did not go current on the fresh session");
        r.resetStatusNew = glGetGraphicsResetStatus();
        (void)EGL::QuerySurface(dpy, surface, EGL_WIDTH, &r.widthAfter);
        (void)EGL::QuerySurface(dpy, surface, EGL_HEIGHT, &r.heightAfter);

        // The lost session's image names nothing the fresh one holds; the buffer is imported again
        // (the same server image, so the same id) - and only then is the stale one destroyed.
        r.staleBindError = BindImageToNewTexture(stale);
        const EGLImage again = ImportImage(dpy, fd, image);
        r.reimported = again != EGL_NO_IMAGE ? 1 : 0;
        if (!r.reimported) return Note(r, "the client's image was not imported again");
        (void)EGL::LookupSharedImage(again, &r.reimportedId, &ignored, &ignored);
        const Uint64 before = PublishedSeq();
        (void)EGL::DestroyImage(dpy, stale);
        r.recordsByStaleDestroy = static_cast<Int64>(PublishedSeq()) - static_cast<Int64>(before);
        r.freshBindError = BindImageToNewTexture(again);
        r.freshPixel = ClearAndRead(0.0f, 0.0f, 1.0f);
        r.freshSwap = EGL::SwapBuffers(dpy, surface);
        r.resetStatusEnd = glGetGraphicsResetStatus();

        // The owner looks at its own session now.
        const char done = 1;
        const ssize_t wrote = ::write(sock, &done, 1);
        (void)wrote;
        (void)EGL::DestroyImage(dpy, again);
        ::close(fd);
        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(dpy, fresh);
        (void)EGL::DestroyContext(dpy, share);
        (void)EGL::DestroySurface(dpy, surface);
        (void)EGL::Terminate(dpy);
    }

    // A forked client that waits for the server's endpoint before it does anything (the knob names
    // the compositor's pid, so the clients exist before the server starts).
    template <class Report>
    struct HeldChild {
        pid_t pid = -1;
        int go = -1;
        int report = -1;

        template <class Body>
        bool Fork(Body body) {
            int goFds[2] = {-1, -1};
            int reportFds[2] = {-1, -1};
            if (::pipe(goFds) != 0 || ::pipe(reportFds) != 0) return false;
            std::fflush(nullptr);
            pid = ::fork();
            if (pid == 0) {
                ::close(goFds[1]);
                ::close(reportFds[0]);
                char endpoint[512] = {};
                const ssize_t got = ::read(goFds[0], endpoint, sizeof(endpoint) - 1);
                if (got <= 0) ::_exit(0);
                Report r{};
                body(std::string(endpoint), r);
                const ssize_t wrote = ::write(reportFds[1], &r, sizeof(r));
                (void)wrote;
                std::fflush(nullptr);
                ::_exit(0);
            }
            ::close(goFds[0]);
            ::close(reportFds[1]);
            go = goFds[1];
            report = reportFds[0];
            return pid > 0;
        }

        void Go(const std::string& endpoint) {
            const ssize_t wrote = ::write(go, endpoint.c_str(), endpoint.size());
            (void)wrote;
            ::close(go);
            go = -1;
        }

        Report Wait() {
            Report r{};
            pollfd pfd{report, POLLIN, 0};
            if (::poll(&pfd, 1, 90000) > 0) {
                if (::read(report, &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) {
                    r = Report{};
                    Note(r, "the client died before reporting");
                }
            } else {
                Note(r, "the client timed out");
                ::kill(pid, SIGKILL);
            }
            ::close(report);
            int status = 0;
            ::waitpid(pid, &status, 0);
            return r;
        }

        ~HeldChild() {
            if (go >= 0) ::close(go); // a child never released ends itself
        }
    };

    struct ClientLog {
        std::string base;
        explicit ClientLog(const std::string& label)
            : base("/tmp/mgl-comprec-client-" + label + "-" + std::to_string(::getpid()) + ".log") {
            Debug::TruncateRoleLogs(base.c_str());
        }
        std::string Text() const { return ReadFile(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Client)); }
        ~ClientLog() {
            if (::testing::Test::HasFailure() || std::getenv("MOBILEGL_TEST_KEEP_LOGS") != nullptr) return;
            std::error_code ec;
            std::filesystem::remove(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Client), ec);
            std::filesystem::remove(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Server), ec);
            std::filesystem::remove(base, ec);
        }
    };

} // namespace

TEST(CompositorRecovery, ACompositorOnTheServerWindowRecoversOnAFreshSessionAndReimportsItsClientsImages) {
    ::signal(SIGPIPE, SIG_IGN);
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    ClientLog ownerLog("owner");
    ClientLog compositorLog("compositor");

    HeldChild<OwnerReport> owner;
    ASSERT_TRUE(owner.Fork([&](const std::string& endpoint, OwnerReport& r) {
        ::close(pair[1]);
        SetClientEnvironment(endpoint, ownerLog.base, /*serverWindow=*/false);
        RunOwner(pair[0], r);
    }));
    HeldChild<CompositorReport> compositor;
    ASSERT_TRUE(compositor.Fork([&](const std::string& endpoint, CompositorReport& r) {
        ::close(pair[0]);
        SetClientEnvironment(endpoint, compositorLog.base, /*serverWindow=*/true);
        RunCompositor(pair[1], r);
    }));
    ::close(pair[0]);
    ::close(pair[1]);

    ServerProcess server;
    ::setenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_PID", std::to_string(compositor.pid).c_str(), 1);
    const bool launched = LaunchDisplayServer(&server, "kwin");
    ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_PID");
    ASSERT_TRUE(launched) << server.Log();

    owner.Go(server.endpoint);
    compositor.Go(server.endpoint);
    const CompositorReport c = compositor.Wait();
    const OwnerReport o = owner.Wait();
    const std::string diagnostics = "server:\n" + server.Log() + "\ncompositor:\n" + compositorLog.Text() +
                                    "\nowner:\n" + ownerLog.Text();

    // The compositor, before the loss: the server's window, an imported client image, a frame.
    ASSERT_EQ(c.received, 1) << c.note << "\n" << o.note << "\n" << diagnostics;
    ASSERT_EQ(c.surfaceOk, 1) << c.note << "\n" << diagnostics;
    EXPECT_EQ(c.width, static_cast<Int32>(kWindowWidth));
    EXPECT_EQ(c.height, static_cast<Int32>(kWindowHeight));
    ASSERT_EQ(c.firstCurrent, 1) << c.note << "\n" << diagnostics;
    ASSERT_EQ(c.imported, 1) << c.note << "\n" << diagnostics;
    EXPECT_EQ(c.importedId, o.imageId) << "the compositor imported some other image";
    EXPECT_EQ(c.firstBindError, static_cast<Uint32>(GL_NO_ERROR)) << std::hex << c.firstBindError;
    EXPECT_EQ(c.firstPixel, kGreen) << std::hex << c.firstPixel << "\n" << diagnostics;
    EXPECT_EQ(c.firstSwap, EGL_TRUE);

    // The loss, reported.
    ASSERT_EQ(c.lossSeen, 1) << c.note << "\n" << diagnostics;
    EXPECT_EQ(c.resetStatus, static_cast<Uint32>(GL_UNKNOWN_CONTEXT_RESET)) << std::hex << c.resetStatus;
    EXPECT_EQ(c.lostSwap, EGL_FALSE);
    EXPECT_EQ(c.lostSwapError, EGL_CONTEXT_LOST) << std::hex << c.lostSwapError;

    // The restart: a fresh session, the server's window again - after the lost session let go of it.
    ASSERT_EQ(c.newContextOk, 1) << c.note << "\n" << diagnostics;
    ASSERT_EQ(c.newCurrentOk, 1) << "the kept server-window surface did not go current on the fresh session (EGL "
                                    "error 0x"
                                 << std::hex << c.newCurrentError << ")\n"
                                 << c.note << "\n"
                                 << diagnostics;
    EXPECT_EQ(c.resetStatusNew, static_cast<Uint32>(GL_NO_ERROR));
    EXPECT_EQ(c.widthAfter, static_cast<Int32>(kWindowWidth));
    EXPECT_EQ(c.heightAfter, static_cast<Int32>(kWindowHeight));

    // The client's image: the lost session's EGLImage binds nothing any more, the buffer imports
    // again as the same image, and destroying the stale EGLImage after that is silent.
    EXPECT_EQ(c.staleBindError, static_cast<Uint32>(GL_INVALID_OPERATION))
        << "an EGLImage of the lost session was bound on the fresh one: 0x" << std::hex << c.staleBindError;
    ASSERT_EQ(c.reimported, 1) << c.note << "\n" << diagnostics;
    EXPECT_EQ(c.reimportedId, o.imageId);
    EXPECT_EQ(c.recordsByStaleDestroy, 0) << "destroying the lost session's EGLImage released the fresh import's "
                                             "reference on the new session";
    EXPECT_EQ(c.freshBindError, static_cast<Uint32>(GL_NO_ERROR))
        << "the image imported again could not be bound: 0x" << std::hex << c.freshBindError;
    EXPECT_EQ(c.freshPixel, kBlue) << std::hex << c.freshPixel << "\n" << diagnostics;
    EXPECT_EQ(c.freshSwap, EGL_TRUE);
    EXPECT_EQ(c.resetStatusEnd, static_cast<Uint32>(GL_NO_ERROR)) << "the fresh session was lost too";

    // The owner: untouched.
    ASSERT_EQ(o.initialized, 1) << o.note << "\n" << diagnostics;
    ASSERT_EQ(o.allocated, 1) << o.note;
    EXPECT_EQ(o.sent, 1);
    EXPECT_EQ(o.sawDone, 1) << "the compositor never finished";
    EXPECT_EQ(o.statusAfter, static_cast<Uint32>(GL_NO_ERROR)) << "the owner's session was lost with the compositor's";
    EXPECT_EQ(o.pixelAfter, kRed) << std::hex << o.pixelAfter;
    EXPECT_EQ(o.stillIdentified, 1) << "the owner's image is gone from the server";

    // The server's side: the window went to the compositor twice, and only ever at the size the
    // compositor asked for (the window's own).
    ASSERT_TRUE(WaitFor([&] { return Count(server.Log(), "reaped exit=") == 3; }, 10000)) << server.Log();
    const std::string log = server.Log();
    EXPECT_EQ(Count(log, "owner=server (ServerOwned"), 2u) << log;
    EXPECT_EQ(Count(log, "SessionLatch{"), 1u) << log;
    EXPECT_NE(log.find("injected:pid=" + std::to_string(compositor.pid)), std::string::npos) << log;
    EXPECT_EQ(log.find("Fatal{ProtocolCorruption"), std::string::npos) << log;
    EXPECT_EQ(Count(log, "reaped exit=75"), 1u) << log;
    EXPECT_EQ(Count(log, "reaped exit=0"), 2u) << log;
    const std::string geometry = ReadFile(server.logBase + ".geometry");
    EXPECT_EQ(Count(geometry, "\n"), 2u) << "geometry requests:\n" << geometry;
    EXPECT_EQ(Count(geometry, "0 0\n"), Count(geometry, "\n"))
        << "the window was asked for a fixed size - re-creating the surface pinned the window's extent:\n"
        << geometry;
    server.Stop();
    std::error_code ec;
    if (!::testing::Test::HasFailure()) std::filesystem::remove(server.logBase + ".geometry", ec);
}
