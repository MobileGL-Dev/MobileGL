// MobileGL - MobileGL/MG_Test/Wire/ClientRecoveryTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A CLIENT OUTLIVES ITS LOST SESSION. A GPU device loss ends exactly one session on the server
// (MultiSessionTest.ADeviceLossEndsOnlyItsOwnSession); this suite is the client's half. A real
// MobileGL client - the EGL and GL entry points, MobileGL's own initialisation, a unix spawn
// session - against the in-process display server, whose debug knob makes the second device check
// in the server process (this client's second read-back) report the device lost.
//
// What an application that handles context loss (Chrome through ANGLE) needs, each case asserting
// it from the application's side:
//   * the loss is REPORTED: glGetGraphicsResetStatus answers a reset, glGetError GL_CONTEXT_LOST
//     (once), eglSwapBuffers and eglMakeCurrent on the old context fail with EGL_CONTEXT_LOST;
//   * the old context is SAFE TO USE AND TO DESTROY: a draw and a texture upload on it after the
//     loss do nothing, and destroying it puts nothing on the new session's wire;
//   * a context created after the loss gets a FRESH session (the server starts session #2, nothing
//     latches again) and renders: a textured draw through a new program, a clear, and a texture
//     framebuffer, each read back;
//   * nothing of the lost session reaches the fresh one: no record names a handle it never created
//     (the server's per-session refusal line stays silent), and the old texture's name is gone;
//   * the surface the application kept works with the new context (EGL keeps surfaces across a
//     power-management loss; only their contents go);
//   * the other route back - eglTerminate, then eglInitialize - brings a fresh session up too.

#include "P12ServerRig.h"

#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/EGLState/Core.h>

#include <condition_variable>
#include <mutex>

extern "C" {
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
GLenum glGetError(void);
GLenum glGetGraphicsResetStatus(void);
void glGenTextures(GLsizei n, GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
void glActiveTexture(GLenum texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const void* pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const void* pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
GLboolean glIsTexture(GLuint texture);
void glGenFramebuffers(GLsizei n, GLuint* framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLenum glCheckFramebufferStatus(GLenum target);
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glUseProgram(GLuint program);
GLint glGetUniformLocation(GLuint program, const GLchar* name);
void glUniform1i(GLint location, GLint v0);
void glGenVertexArrays(GLsizei n, GLuint* arrays);
void glBindVertexArray(GLuint array);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
}

using namespace MobileGL;
using namespace P12;

namespace {

    namespace EGL = MobileGL::MG_Impl::EGLImpl;

    // A<<24|B<<16|G<<8|R, the little-endian bytes of a GL_RGBA/GL_UNSIGNED_BYTE texel.
    constexpr Uint32 kGreen = 0xFF00FF00u;
    constexpr Uint32 kBlue = 0xFFFF0000u;
    constexpr Uint32 kRed = 0xFF0000FFu;
    constexpr Uint32 kYellow = 0xFF00FFFFu;
    constexpr Uint32 kWhite = 0xFFFFFFFFu;
    constexpr Uint32 kUntouched = 0x5A5A5A5Au;

    enum class Route {
        NewContext,          // destroy the lost context, create another, keep the surface
        TerminateInitialize, // destroy everything, eglTerminate, eglInitialize, start over
    };

    struct RecoveryReport {
        Int32 initialized = 0;
        Uint32 firstPixel = 0;
        Uint32 lostPixel = 0;
        Uint32 resetStatusLost = 0;
        Uint32 errorLost = 0;
        Uint32 errorAfterLost = 0;
        Int32 swapLost = -1;
        Int32 swapLostError = 0;
        Int32 releaseOk = 0;
        Int32 oldCurrent = -1;
        Int32 oldCurrentError = 0;
        Int32 destroyOldOk = 0;
        Int64 recordsByOldDestroy = -1;
        Int32 newContextOk = 0;
        Int32 newContextError = 0;
        Int32 newCurrentOk = 0;
        Int32 newCurrentError = 0;
        Uint32 resetStatusNew = 0xFFFFFFFFu;
        Int32 oldNameIsTexture = -1;
        // What the client believes about the lost context's texture: whether the server has its
        // record (the publication latch) and whether an upload is still owed to it (the drain).
        Int32 oldTexturePublishedBefore = -1;
        Int32 oldTextureOwedBefore = -1;
        Int32 oldTexturePublishedAfter = -1;
        Int32 oldTextureOwedAfter = -1;
        Uint32 drawnPixel = 0;
        Uint32 secondPixel = 0;
        Uint32 fboStatus = 0;
        Uint32 fboPixel = 0;
        Int32 swapNew = -1;
        Uint32 resetStatusEnd = 0xFFFFFFFFu;
        Uint32 serverPidBefore = 0;
        Uint32 serverPidAfter = 0;
        char note[256] = {};
    };

    void Note(RecoveryReport& r, const char* text) {
        if (r.note[0] == '\0') std::snprintf(r.note, sizeof(r.note), "%s", text);
    }

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

    // A full-surface triangle that samples one texel: a program, a vertex array, a texture with its
    // sampler parameters and a draw - the shader, sampler, texture-upload and render-state families.
    struct TexturedDraw {
        GLuint program = 0;
        GLuint vertexArray = 0;
        GLuint texture = 0;

        Bool Build(RecoveryReport& r, Uint32 rgba) {
            static const char* kVertex = "#version 330 core\n"
                                         "const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
                                         "void main() { gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0); }\n";
            static const char* kFragment = "#version 330 core\n"
                                           "uniform sampler2D image;\n"
                                           "out vec4 color;\n"
                                           "void main() { color = texture(image, vec2(0.5)); }\n";
            const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
            const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(vertex, 1, &kVertex, nullptr);
            glShaderSource(fragment, 1, &kFragment, nullptr);
            glCompileShader(vertex);
            glCompileShader(fragment);
            GLint compiled = 0;
            glGetShaderiv(vertex, GL_COMPILE_STATUS, &compiled);
            if (!compiled) return Note(r, "the vertex shader did not compile"), false;
            glGetShaderiv(fragment, GL_COMPILE_STATUS, &compiled);
            if (!compiled) return Note(r, "the fragment shader did not compile"), false;
            program = glCreateProgram();
            glAttachShader(program, vertex);
            glAttachShader(program, fragment);
            glLinkProgram(program);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (!linked) return Note(r, "the program did not link"), false;
            glGenVertexArrays(1, &vertexArray);
            glGenTextures(1, &texture);
            Fill(rgba);
            return true;
        }

        void Fill(Uint32 rgba) const {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba);
        }

        void Draw() const {
            glViewport(0, 0, 8, 8);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "image"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glBindVertexArray(vertexArray);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    };

    Uint32 ServerPid() {
        auto* session = Remote::Client::ClientSession::Active();
        return session != nullptr ? session->PeerServerPid() : 0;
    }

    // The wire handle of texture `name` of `context`, through the client's own books.
    MobileGL::MG_Pipe::MGPipeHandle TextureHandle(EGLContext context, GLuint name) {
        const auto glState = MobileGL::MG_State::pEGLContext->GetContextGLState(context);
        if (!glState) return MobileGL::MG_Pipe::kMGPipeNullHandle;
        const auto& texture = glState->GetTextureObject(name);
        if (!texture) return MobileGL::MG_Pipe::kMGPipeNullHandle;
        return MobileGL::MG_Pipe::MGPipeSlots().FindByLifetimeId(MobileGL::MG_Pipe::MGPipeKind::Texture,
                                                                 texture->GetLifetimeId());
    }

    void NoteTextureBooks(MobileGL::MG_Pipe::MGPipeHandle handle, Int32* published, Int32* owed) {
        using namespace MobileGL::MG_Pipe;
        *published = MGPipeHandleIsPublished(MGPipeKind::Texture, handle) ? 1 : 0;
        *owed = MGPipeTextureEmitterInstance().OwesUpload(handle) ? 1 : 0;
    }

    Uint64 PublishedSeq() {
        auto* session = Remote::Client::ClientSession::Active();
        return session != nullptr ? session->LastPublishedSeq() : 0;
    }

    struct Egl {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLConfig config = nullptr;
        EGLSurface surface = EGL_NO_SURFACE;

        Bool Bring(RecoveryReport& r) {
            dpy = EGL::GetDisplay(EGL_DEFAULT_DISPLAY);
            EGLint major = 0;
            EGLint minor = 0;
            if (dpy == EGL_NO_DISPLAY || EGL::Initialize(dpy, &major, &minor) != EGL_TRUE)
                return Note(r, "eglInitialize failed (no spawn session?)"), false;
            (void)EGL::BindAPI(EGL_OPENGL_API);
            const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
            EGLint count = 0;
            if (EGL::ChooseConfig(dpy, configAttribs, &config, 1, &count) != EGL_TRUE || count < 1)
                return Note(r, "no config"), false;
            const EGLint pbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
            surface = EGL::CreatePbufferSurface(dpy, config, pbufferAttribs);
            if (surface == EGL_NO_SURFACE) return Note(r, "no pbuffer"), false;
            return true;
        }

        EGLContext Context() const {
            const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
            return EGL::CreateContext(dpy, config, EGL_NO_CONTEXT, attribs);
        }
    };

    // The client, in a fork: everything an application does around a loss, through the entry points.
    void RunRecoveringClient(Route route, RecoveryReport& r) {
        Egl egl;
        if (!egl.Bring(r)) return;
        const EGLContext lost = egl.Context();
        if (lost == EGL_NO_CONTEXT || EGL::MakeCurrent(egl.dpy, egl.surface, egl.surface, lost) != EGL_TRUE)
            return Note(r, "the first context did not go current");
        r.initialized = 1;
        r.serverPidBefore = ServerPid();

        // The objects of the context that is about to be lost, and device check #1 in the server
        // process: a textured draw, read back.
        TexturedDraw old;
        if (!old.Build(r, kGreen)) return;
        old.Draw();
        r.firstPixel = ReadPixel();
        // Device check #2: the device is "lost" there. The session latches, the server ends it.
        r.lostPixel = ClearAndRead(0.0f, 0.0f, 1.0f);
        (void)WaitFor([] { return glGetGraphicsResetStatus() != GL_NO_ERROR; }, 5000);

        // THE LOSS, REPORTED.
        r.resetStatusLost = glGetGraphicsResetStatus();
        r.errorLost = glGetError();
        r.errorAfterLost = glGetError();
        // What an application does before it has looked: more work on the lost context - a draw,
        // and then an upload that no later verb of the lost context will ever carry (it is owed to
        // a server that is gone). None of it may surface on the fresh session.
        old.Draw();
        const Uint32 white = kWhite;
        glBindTexture(GL_TEXTURE_2D, old.texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        const auto oldHandle = TextureHandle(lost, old.texture);
        NoteTextureBooks(oldHandle, &r.oldTexturePublishedBefore, &r.oldTextureOwedBefore);
        r.swapLost = EGL::SwapBuffers(egl.dpy, egl.surface);
        r.swapLostError = EGL::GetError();
        r.releaseOk = EGL::MakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
        r.oldCurrent = EGL::MakeCurrent(egl.dpy, egl.surface, egl.surface, lost);
        r.oldCurrentError = EGL::GetError();
        if (r.oldCurrent == EGL_TRUE) (void)EGL::MakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        EGLContext fresh = EGL_NO_CONTEXT;
        if (route == Route::NewContext) {
            // A NEW CONTEXT FIRST (Chrome's order: the replacement exists before the old one goes),
            // then the old one is destroyed - which must put nothing on the new session's wire.
            fresh = egl.Context();
            r.newContextOk = fresh != EGL_NO_CONTEXT;
            r.newContextError = EGL::GetError();
            // The fresh session is up: the lost texture is not on its server, and nothing is owed.
            NoteTextureBooks(oldHandle, &r.oldTexturePublishedAfter, &r.oldTextureOwedAfter);
            const Uint64 before = PublishedSeq();
            r.destroyOldOk = EGL::DestroyContext(egl.dpy, lost) == EGL_TRUE;
            r.recordsByOldDestroy = static_cast<Int64>(PublishedSeq()) - static_cast<Int64>(before);
        } else {
            r.destroyOldOk = EGL::DestroyContext(egl.dpy, lost) == EGL_TRUE;
            (void)EGL::DestroySurface(egl.dpy, egl.surface);
            (void)EGL::Terminate(egl.dpy);
            Egl again;
            if (!again.Bring(r)) return;
            egl = again;
            NoteTextureBooks(oldHandle, &r.oldTexturePublishedAfter, &r.oldTextureOwedAfter);
            fresh = egl.Context();
            r.newContextOk = fresh != EGL_NO_CONTEXT;
            r.newContextError = EGL::GetError();
        }
        if (fresh == EGL_NO_CONTEXT) return Note(r, "no context after the loss");
        r.newCurrentOk = EGL::MakeCurrent(egl.dpy, egl.surface, egl.surface, fresh) == EGL_TRUE;
        r.newCurrentError = EGL::GetError();
        if (!r.newCurrentOk) return Note(r, "the new context did not go current");
        r.serverPidAfter = ServerPid();
        r.resetStatusNew = glGetGraphicsResetStatus();
        r.oldNameIsTexture = glIsTexture(old.texture) == GL_TRUE ? 1 : 0;

        // THE NEW CONTEXT RENDERS: the same textured draw with a new program and texture, a clear of
        // the surface's own framebuffer, then a texture-backed framebuffer.
        TexturedDraw fresher;
        if (!fresher.Build(r, kRed)) return;
        fresher.Draw();
        r.drawnPixel = ReadPixel();
        r.secondPixel = ClearAndRead(0.0f, 0.0f, 1.0f);
        GLuint target = 0;
        GLuint framebuffer = 0;
        glGenTextures(1, &target);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        r.fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        r.fboPixel = ClearAndRead(1.0f, 1.0f, 0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        r.swapNew = EGL::SwapBuffers(egl.dpy, egl.surface);
        r.resetStatusEnd = glGetGraphicsResetStatus();

        (void)EGL::MakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(egl.dpy, fresh);
        (void)EGL::DestroySurface(egl.dpy, egl.surface);
        (void)EGL::Terminate(egl.dpy);
    }

    // A thread of the client that runs what it is handed, one job at a time, and keeps whatever
    // EGL context it made current between jobs - an application's other GL thread.
    class Worker {
    public:
        Worker() : m_thread([this] { Loop(); }) {}
        ~Worker() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_quit = true;
            }
            m_wake.notify_all();
            m_thread.join();
        }
        void Run(const std::function<void()>& job) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_job = job;
            m_wake.notify_all();
            m_wake.wait(lock, [this] { return !m_job; });
        }

    private:
        void Loop() {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;) {
                m_wake.wait(lock, [this] { return m_quit || static_cast<bool>(m_job); });
                if (m_quit) return;
                lock.unlock();
                m_job();
                lock.lock();
                m_job = nullptr;
                m_wake.notify_all();
            }
        }
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::function<void()> m_job;
        Bool m_quit = false;
        std::thread m_thread;
    };

    // THE OTHER THREAD. The context that is lost stays current on a second thread of the
    // application while its main thread recovers and renders on a fresh session; then the second
    // thread - which has not looked yet - draws, uploads and reads on the lost context. All of it
    // must be refused as lost (reset status, nothing read back) and none of it may reach the fresh
    // session: no record published while it runs, no latch on the server (a binding that named the
    // lost context's token there would be Fatal{ProtocolCorruption}), and the main thread's
    // rendering is unaffected afterwards.
    struct StaleThreadReport {
        Int32 initialized = 0;
        Uint32 firstPixel = 0;
        Int32 newCurrentOk = 0;
        Uint32 drawnPixel = 0;
        Uint32 staleResetStatus = 0;
        Uint32 stalePixel = 0;
        Int32 staleSwap = -1;
        Int32 staleSwapError = 0;
        Int32 staleRelease = 0;
        Int64 recordsByStaleThread = -1;
        Uint32 pixelAfterStale = 0;
        Uint32 resetStatusAfterStale = 0xFFFFFFFFu;
        Int32 destroyOldOk = 0;
        char note[256] = {};
    };

    void Note(StaleThreadReport& r, const char* text) {
        if (r.note[0] == '\0') std::snprintf(r.note, sizeof(r.note), "%s", text);
    }

    void RunStaleThreadClient(StaleThreadReport& r) {
        Egl egl;
        RecoveryReport bring{};
        if (!egl.Bring(bring)) return Note(r, bring.note);
        const EGLContext lost = egl.Context();
        Worker second;
        TexturedDraw old;
        second.Run([&] {
            if (lost == EGL_NO_CONTEXT || EGL::MakeCurrent(egl.dpy, egl.surface, egl.surface, lost) != EGL_TRUE)
                return Note(r, "the first context did not go current on the second thread");
            RecoveryReport built{};
            if (!old.Build(built, kGreen)) return Note(r, built.note);
            r.initialized = 1;
            old.Draw();
            r.firstPixel = ReadPixel();                        // device check #1
            (void)ClearAndRead(0.0f, 0.0f, 1.0f);              // device check #2: lost
            (void)WaitFor([] { return glGetGraphicsResetStatus() != GL_NO_ERROR; }, 5000);
        });
        if (!r.initialized) return;

        // The main thread recovers: a surface and a context of its own, on a fresh session.
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        const EGLSurface mine = EGL::CreatePbufferSurface(egl.dpy, egl.config, pbufferAttribs);
        const EGLContext fresh = egl.Context();
        r.newCurrentOk = fresh != EGL_NO_CONTEXT && mine != EGL_NO_SURFACE &&
                         EGL::MakeCurrent(egl.dpy, mine, mine, fresh) == EGL_TRUE;
        if (!r.newCurrentOk) return Note(r, "no fresh context went current on the main thread");
        TexturedDraw fresher;
        RecoveryReport built{};
        if (!fresher.Build(built, kRed)) return Note(r, built.note);
        fresher.Draw();
        r.drawnPixel = ReadPixel();

        // The second thread, still on the lost context, gets to it now.
        const Uint64 before = PublishedSeq();
        second.Run([&] {
            r.staleResetStatus = glGetGraphicsResetStatus();
            old.Draw();
            old.Fill(kWhite);
            r.stalePixel = ClearAndRead(1.0f, 1.0f, 1.0f);
            r.staleSwap = EGL::SwapBuffers(egl.dpy, egl.surface);
            r.staleSwapError = EGL::GetError();
            r.staleRelease = EGL::MakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
        });
        r.recordsByStaleThread = static_cast<Int64>(PublishedSeq()) - static_cast<Int64>(before);
        r.destroyOldOk = EGL::DestroyContext(egl.dpy, lost) == EGL_TRUE;

        // The main thread renders on, unaffected.
        fresher.Draw();
        r.pixelAfterStale = ReadPixel();
        r.resetStatusAfterStale = glGetGraphicsResetStatus();

        (void)EGL::MakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)EGL::DestroyContext(egl.dpy, fresh);
        (void)EGL::DestroySurface(egl.dpy, mine);
        (void)EGL::DestroySurface(egl.dpy, egl.surface);
        (void)EGL::Terminate(egl.dpy);
    }

    // Runs `body` in a forked client (the environment a deployment sets, then the entry points)
    // and hands back the report it filled.
    template <class Report, class Body>
    Report RunInClient(const ServerProcess& server, const std::string& clientLogBase, Body body) {
        Report report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", clientLogBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", server.endpoint.c_str(), 1);
            ::unsetenv("MOBILEGL_IPC_SURFACE");
            ::unsetenv("MOBILEGL_BACKEND_TYPE");
            ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT");
            Report r{};
            body(r);
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
                Note(report, "the client died before reporting");
            }
        } else {
            Note(report, "the client timed out");
            ::kill(pid, SIGKILL);
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }

    RecoveryReport RunClient(const ServerProcess& server, Route route, const std::string& clientLogBase) {
        return RunInClient<RecoveryReport>(server, clientLogBase,
                                           [route](RecoveryReport& r) { RunRecoveringClient(route, r); });
    }

    struct ClientLog {
        std::string base;
        explicit ClientLog(const std::string& label)
            : base("/tmp/mgl-recover-client-" + label + "-" + std::to_string(::getpid()) + ".log") {
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

    class ClientRecovery : public ::testing::Test {
    protected:
        void SetUp() override { ::signal(SIGPIPE, SIG_IGN); }

        // The server, with the loss armed for the second device check in its process.
        bool Launch(ServerProcess* server, const std::string& label) {
            ::setenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT", "2", 1);
            const bool launched = LaunchUnixInProcessServer(server, label);
            ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT");
            return launched;
        }

        // What every route has to show from the application's side, before and after the loss.
        static void ExpectLossReportedAndRecovered(const RecoveryReport& r, const std::string& diagnostics) {
            ASSERT_EQ(r.initialized, 1) << r.note << "\n" << diagnostics;
            ASSERT_EQ(r.firstPixel, kGreen) << "the first draw (device check #1) did not read back: " << std::hex
                                            << r.firstPixel << "\n" << r.note << "\n" << diagnostics;
            EXPECT_NE(r.lostPixel, kBlue) << "a read-back on the lost device returned a frame";
            // The loss is reported.
            EXPECT_TRUE(r.resetStatusLost == GL_UNKNOWN_CONTEXT_RESET || r.resetStatusLost == GL_GUILTY_CONTEXT_RESET)
                << "glGetGraphicsResetStatus after the loss: 0x" << std::hex << r.resetStatusLost;
            EXPECT_EQ(r.errorLost, static_cast<Uint32>(GL_CONTEXT_LOST))
                << "glGetError after the loss: 0x" << std::hex << r.errorLost;
            EXPECT_EQ(r.errorAfterLost, static_cast<Uint32>(GL_NO_ERROR))
                << "GL_CONTEXT_LOST must be reported once, not forever (a drain loop must end)";
            EXPECT_EQ(r.swapLost, EGL_FALSE) << "eglSwapBuffers on the lost context succeeded";
            EXPECT_EQ(r.swapLostError, EGL_CONTEXT_LOST) << "eglSwapBuffers on the lost context: 0x" << std::hex
                                                         << r.swapLostError;
            EXPECT_EQ(r.releaseOk, 1) << "releasing the lost context failed";
            EXPECT_EQ(r.oldCurrent, EGL_FALSE) << "the lost context was made current again";
            EXPECT_EQ(r.oldCurrentError, EGL_CONTEXT_LOST) << "eglMakeCurrent of the lost context: 0x" << std::hex
                                                           << r.oldCurrentError;
            EXPECT_EQ(r.destroyOldOk, 1) << "the lost context could not be destroyed";
            // A fresh session, and the new context renders.
            ASSERT_EQ(r.newContextOk, 1) << "no context could be created after the loss (EGL error 0x" << std::hex
                                         << r.newContextError << ")\n" << r.note << "\n" << diagnostics;
            ASSERT_EQ(r.newCurrentOk, 1) << "the new context did not go current (EGL error 0x" << std::hex
                                         << r.newCurrentError << ")\n" << r.note << "\n" << diagnostics;
            EXPECT_EQ(r.serverPidAfter, r.serverPidBefore) << "the new session is not on the same server process";
            EXPECT_EQ(r.resetStatusNew, static_cast<Uint32>(GL_NO_ERROR)) << "the new context reports a reset";
            EXPECT_EQ(r.oldNameIsTexture, 0) << "the old context's texture name resolves in the new context";
            // The client's books on the lost texture: before the recovery the lost session had its
            // record (the arm proof that there was something to forget), and the upload made on the
            // lost context was queued for nobody; after it, neither - its death and its texels must
            // not reach the fresh server.
            EXPECT_EQ(r.oldTexturePublishedBefore, 1) << "the lost texture was never published (nothing to forget)";
            EXPECT_EQ(r.oldTextureOwedBefore, 0) << "an upload on the lost context was queued; the next context's "
                                                    "verb would carry it to whatever session is current";
            EXPECT_EQ(r.oldTexturePublishedAfter, 0) << "the fresh session's client still believes it holds the "
                                                        "lost texture's record";
            EXPECT_EQ(r.oldTextureOwedAfter, 0) << "an upload owed to the lost session is queued for the fresh one";
            EXPECT_EQ(r.drawnPixel, kRed) << "the new context's textured draw did not read back: " << std::hex
                                          << r.drawnPixel << "\n" << r.note << "\n" << diagnostics;
            EXPECT_EQ(r.secondPixel, kBlue) << "the new context's clear did not read back: " << std::hex
                                            << r.secondPixel;
            EXPECT_EQ(r.fboStatus, static_cast<Uint32>(GL_FRAMEBUFFER_COMPLETE)) << std::hex << r.fboStatus;
            EXPECT_EQ(r.fboPixel, kYellow) << "the new context's texture framebuffer did not read back: " << std::hex
                                           << r.fboPixel;
            EXPECT_EQ(r.swapNew, EGL_TRUE) << "eglSwapBuffers on the new context failed";
            EXPECT_EQ(r.resetStatusEnd, static_cast<Uint32>(GL_NO_ERROR)) << "the new session was lost too";
        }

        // The server's side: the first session latched on the injected loss and nothing else did; a
        // second session started and ended cleanly; nothing from the first reached it.
        static void ExpectOneLossAndACleanSecondSession(ServerProcess& server) {
            ASSERT_TRUE(WaitFor([&] { return Count(server.Log(), "in-process unix session #2 started") == 1; }, 5000))
                << "the client never opened a second session:\n" << server.Log();
            // Both sessions' ends: the lost one's latched exit and the fresh one's clean one.
            ASSERT_TRUE(WaitFor([&] { return Count(server.Log(), "reaped exit=") == 2; }, 10000))
                << "the second session did not end:\n" << server.Log();
            const std::string log = server.Log();
            EXPECT_NE(log.find("SessionLatch{BackendDeviceLost}"), std::string::npos) << log;
            EXPECT_EQ(Count(log, "reaped exit=75"), 1u) << "the lost session did not end latched:\n" << log;
            EXPECT_EQ(Count(log, "reaped exit=0"), 1u) << "the fresh session did not end cleanly:\n" << log;
            // A fresh session latches the moment the client carries anything of the lost one it
            // checks: the applier-reset serial it counts from 0, a context binding naming a token it
            // never created, a create or bind it cannot parse - each Fatal{ProtocolCorruption}.
            EXPECT_EQ(Count(log, "SessionLatch{"), 1u) << "the recovered session latched too:\n" << log;
            EXPECT_EQ(log.find("Fatal{ProtocolCorruption"), std::string::npos)
                << "the recovered session was sent something it never created:\n" << log;
            EXPECT_TRUE(server.pid > 0 && ::kill(server.pid, 0) == 0) << "the server died";
        }
    };

} // namespace

// THE HEADLINE: Chrome's route. The lost context is reported lost, a new one is created on a fresh
// session and renders into the surface the application kept, and destroying the old one is silent.
TEST_F(ClientRecovery, ANewContextAfterADeviceLossRendersOnAFreshSession) {
    ServerProcess server;
    ASSERT_TRUE(Launch(&server, "newctx")) << server.Log();
    ClientLog client("newctx");
    const RecoveryReport r = RunClient(server, Route::NewContext, client.base);
    const std::string diagnostics = "server:\n" + server.Log() + "\nclient:\n" + client.Text();
    ExpectLossReportedAndRecovered(r, diagnostics);
    EXPECT_EQ(r.recordsByOldDestroy, 0) << "destroying the lost context published records on the new session";
    ExpectOneLossAndACleanSecondSession(server);
    server.Stop();
}

// The other route back: everything destroyed, eglTerminate, eglInitialize, and a fresh start.
TEST_F(ClientRecovery, TerminateAndInitializeAfterADeviceLossBringsUpAFreshSession) {
    ServerProcess server;
    ASSERT_TRUE(Launch(&server, "reinit")) << server.Log();
    ClientLog client("reinit");
    const RecoveryReport r = RunClient(server, Route::TerminateInitialize, client.base);
    const std::string diagnostics = "server:\n" + server.Log() + "\nclient:\n" + client.Text();
    ExpectLossReportedAndRecovered(r, diagnostics);
    ExpectOneLossAndACleanSecondSession(server);
    server.Stop();
}

// THE OTHER THREAD: the lost context is still current on a second thread while the main thread
// recovers. Everything that thread does afterwards is refused as lost and reaches nothing of the
// fresh session - without the per-thread answer its first GL call would bind the lost context's
// token on the fresh session, which latches it Fatal{ProtocolCorruption, "BindContext..."}.
TEST_F(ClientRecovery, AThreadStillOnTheLostContextReachesNothingOfTheFreshSession) {
    ServerProcess server;
    ASSERT_TRUE(Launch(&server, "stale")) << server.Log();
    ClientLog client("stale");
    const StaleThreadReport r = RunInClient<StaleThreadReport>(server, client.base, &RunStaleThreadClient);
    const std::string diagnostics = "server:\n" + server.Log() + "\nclient:\n" + client.Text();
    ASSERT_EQ(r.initialized, 1) << r.note << "\n" << diagnostics;
    ASSERT_EQ(r.firstPixel, kGreen) << std::hex << r.firstPixel << "\n" << diagnostics;
    ASSERT_EQ(r.newCurrentOk, 1) << r.note << "\n" << diagnostics;
    EXPECT_EQ(r.drawnPixel, kRed) << "the fresh context did not render: " << std::hex << r.drawnPixel;
    EXPECT_TRUE(r.staleResetStatus == GL_UNKNOWN_CONTEXT_RESET || r.staleResetStatus == GL_GUILTY_CONTEXT_RESET)
        << "the thread on the lost context sees no reset: 0x" << std::hex << r.staleResetStatus;
    EXPECT_NE(r.stalePixel, kWhite) << "a read-back on the lost context returned a frame";
    EXPECT_EQ(r.staleSwap, EGL_FALSE);
    EXPECT_EQ(r.staleSwapError, EGL_CONTEXT_LOST) << std::hex << r.staleSwapError;
    EXPECT_EQ(r.staleRelease, 1) << "the second thread could not release the lost context";
    EXPECT_EQ(r.recordsByStaleThread, 0) << "the thread on the lost context published records on the fresh session";
    EXPECT_EQ(r.destroyOldOk, 1);
    EXPECT_EQ(r.pixelAfterStale, kRed) << "the fresh context stopped rendering: " << std::hex << r.pixelAfterStale
                                       << "\n" << diagnostics;
    EXPECT_EQ(r.resetStatusAfterStale, static_cast<Uint32>(GL_NO_ERROR)) << "the fresh session was lost too";
    ExpectOneLossAndACleanSecondSession(server);
    server.Stop();
}
