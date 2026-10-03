// MobileGL - MobileGL/MG_Test/Wire/ConcurrentContextsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SEVERAL CONTEXTS, SEVERAL THREADS, ONE SESSION. A Qt Quick application renders each window on a
// thread of its own, every window with its own context. Normally they all share with one global
// context; after a device loss that share context is lost, so the replacements are created without
// sharing - one share group per window, drawing at the same time. Each thread here owns a pbuffer,
// a context and a textured draw of its own colour, and reads it back over and over - through the
// exported EGL and GL entry points, which is where the client serialises its threads.
//
// Every shape below has one rule in common: a thread's records apply under ITS context, whatever the
// other threads bind, release or destroy meanwhile. The shapes differ in what the other threads do:
//   - each thread destroys its context when it is done (shared root, and one group per thread);
//   - threads release and re-bind in the middle of their run and release without destroying at
//     the end (the destroy comes after every thread has joined);
//   - siblings of one share group with no root: the group's first context is destroyed while the
//     others still draw with objects of the group;
//   - a context destroyed by ANOTHER thread while it is current (EGL defers the destroy until it
//     is released: the thread keeps drawing, and the context is gone once released);
//   - one thread reading back without pause while others create, draw in, release and destroy
//     contexts as fast as they can.

#include "P12ServerRig.h"

#include <EGL/egl.h>

#include <thread>

extern "C" {
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void glGenTextures(GLsizei n, GLuint* textures);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
void glActiveTexture(GLenum texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const void* pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glCompileShader(GLuint shader);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
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

    constexpr int kThreads = 6;
    constexpr int kFrames = 40;
    // The reader of the read-back race, and the frames it reads.
    constexpr int kRaceChurners = 3;
    constexpr int kRaceReaderFrames = 300;

    enum class Shape : Int32 {
        SharedRoot,          // every thread shares with one root context, destroys its own when done
        Unshared,            // one share group per thread, each destroys its own when done
        ReleaseOnly,         // one group per thread; release + re-bind mid-run, release (no destroy) at the end
        Siblings,            // one group, no root: the group's first context is destroyed while the rest draw
        DestroyWhileCurrent, // another thread destroys a context while it is current (deferred by EGL)
        ReadbackRace,        // one reader without pause, the rest churn create/draw/release/destroy
    };

    struct ThreadReport {
        Int32 current = 0;
        Int32 linked = 0;
        Int32 good = 0;
        Int32 bad = 0;
        Uint32 firstBad = 0;
        Uint32 firstExpected = 0;
        Int32 firstBadFrame = -1;
        Int32 vertexCompiled = -1;
        Int32 fragmentCompiled = -1;
        Int32 rebinds = 0;      // ReleaseOnly: mid-run release + make-current that succeeded
        Int32 cycles = 0;       // ReadbackRace churners: contexts created, used and destroyed
        Int32 cycleFailures = 0;
        // DestroyWhileCurrent: what eglDestroyContext answered on the OTHER thread, and what
        // binding the destroyed context again after this thread released it answered.
        Int32 destroyAnswer = -1;
        Int32 destroyError = 0;
        Int32 rebindAnswer = -1;
        Int32 rebindError = 0;
        Int32 destroyWaited = 0; // the destroy happened while this thread was current
        char linkLog[200] = {};
    };

    struct Report {
        Int32 initialized = 0;
        Int32 finished = 0;
        ThreadReport threads[kThreads];
    };

    // A<<24|B<<16|G<<8|R of the texel each thread draws.
    Uint32 ColourOf(int index) { return 0xFF000000u | (0x40u + 0x50u * static_cast<Uint32>(index % 3)) << 8 |
                                        (0x20u + 0x30u * static_cast<Uint32>(index / 3)); }
    // The clear colour of a churn cycle (ReadbackRace): a different one per thread and cycle.
    Uint32 ChurnColourOf(int index, int cycle) {
        return 0xFF000000u | (0x10u * static_cast<Uint32>(index)) << 16 | (static_cast<Uint32>(cycle * 7) & 0xFFu) << 8 |
               0x80u;
    }

    struct Rig {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLConfig config = nullptr;
        Shape shape = Shape::Unshared;
        Report* report = nullptr;
        EGLContext share = EGL_NO_CONTEXT;       // SharedRoot's root
        EGLContext given[kThreads] = {};         // contexts made on the main thread (Siblings)
        std::atomic<EGLContext> published[kThreads] = {}; // DestroyWhileCurrent: each thread's own
        std::atomic<Int32> halfway[kThreads] = {};
        std::atomic<Int32> destroyed[kThreads] = {};
        std::atomic<Int32> readerDone{0};
    };

    const EGLint kContextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    const EGLint kPbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};

    template <typename Predicate>
    bool WaitUntil(Predicate predicate, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    struct Drawing {
        GLuint program = 0;
        GLuint vertexArray = 0;
        GLuint texture = 0;
        Uint32 colour = 0;
    };

    // The thread's program, vertex array and one-texel texture of its own colour.
    Drawing BuildDrawing(int index, ThreadReport* out) {
        static const char* kVertex = "#version 330 core\n"
                                     "const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));\n"
                                     "void main() { gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0); }\n";
        static const char* kFragment = "#version 330 core\n"
                                       "uniform sampler2D image;\n"
                                       "out vec4 color;\n"
                                       "void main() { color = texture(image, vec2(0.5)); }\n";
        Drawing drawing;
        const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
        const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vertex, 1, &kVertex, nullptr);
        glShaderSource(fragment, 1, &kFragment, nullptr);
        glCompileShader(vertex);
        glCompileShader(fragment);
        drawing.program = glCreateProgram();
        glAttachShader(drawing.program, vertex);
        glAttachShader(drawing.program, fragment);
        glLinkProgram(drawing.program);
        GLint linked = 0;
        glGetProgramiv(drawing.program, GL_LINK_STATUS, &linked);
        out->linked = linked;
        glGetShaderiv(vertex, GL_COMPILE_STATUS, &out->vertexCompiled);
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &out->fragmentCompiled);
        if (!linked) glGetProgramInfoLog(drawing.program, sizeof(out->linkLog) - 1, nullptr, out->linkLog);
        glGenVertexArrays(1, &drawing.vertexArray);
        glGenTextures(1, &drawing.texture);
        drawing.colour = ColourOf(index);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, drawing.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &drawing.colour);
        return drawing;
    }

    void Tally(ThreadReport* out, int frame, Uint32 pixel, Uint32 expected) {
        if (pixel == expected) {
            ++out->good;
            return;
        }
        if (out->bad++ == 0) {
            out->firstBad = pixel;
            out->firstExpected = expected;
            out->firstBadFrame = frame;
        }
    }

    // One frame: clear to transparent black, draw the thread's texel over the whole surface, read
    // one pixel back.
    void DrawFrame(const Drawing& drawing, int frame, ThreadReport* out) {
        glViewport(0, 0, 8, 8);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(drawing.program);
        glUniform1i(glGetUniformLocation(drawing.program, "image"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, drawing.texture);
        glBindVertexArray(drawing.vertexArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        Uint32 pixel = 0x5A5A5A5Au;
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
        Tally(out, frame, pixel, drawing.colour);
    }

    // The frames a thread draws: staggered, so the threads finish (and tear down) one after another
    // while the rest are still drawing.
    int FramesOf(const Rig& rig, int index) {
        if (rig.shape == Shape::Siblings) return index == 0 ? kFrames / 4 : kFrames + 4 * index;
        return kFrames + 6 * index;
    }

    void RenderOnThread(Rig* rig, int index) {
        ThreadReport* out = &rig->report->threads[index];
        const EGLDisplay dpy = rig->dpy;
        const EGLSurface surface = eglCreatePbufferSurface(dpy, rig->config, kPbufferAttribs);
        EGLContext context = EGL_NO_CONTEXT;
        switch (rig->shape) {
        case Shape::SharedRoot: context = eglCreateContext(dpy, rig->config, rig->share, kContextAttribs); break;
        case Shape::Siblings: context = rig->given[index]; break;
        default: context = eglCreateContext(dpy, rig->config, EGL_NO_CONTEXT, kContextAttribs); break;
        }
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            eglMakeCurrent(dpy, surface, surface, context) != EGL_TRUE) {
            if (rig->shape == Shape::DestroyWhileCurrent) rig->published[index].store(EGL_NO_CONTEXT);
            rig->halfway[index].store(1);
            return;
        }
        out->current = 1;
        rig->published[index].store(context);
        const Drawing drawing = BuildDrawing(index, out);
        const int frames = FramesOf(*rig, index);
        for (int frame = 0; frame < frames; ++frame) {
            if (rig->shape == Shape::DestroyWhileCurrent && frame == frames / 2) {
                // Halfway: let the main thread destroy this context while it is current here, and
                // draw the second half in it all the same.
                rig->halfway[index].store(1);
                out->destroyWaited = WaitUntil([&] { return rig->destroyed[index].load() != 0; }, 20000) ? 1 : 0;
            }
            if (rig->shape == Shape::ReleaseOnly && frame > 0 && frame % 8 == 0) {
                // Release and bind again in the middle of the run: nothing is destroyed, and this
                // thread's state must be just as it was.
                if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE &&
                    eglMakeCurrent(dpy, surface, surface, context) == EGL_TRUE)
                    ++out->rebinds;
            }
            DrawFrame(drawing, frame, out);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        switch (rig->shape) {
        case Shape::ReleaseOnly:
            break; // destroyed by the main thread once every thread has joined
        case Shape::DestroyWhileCurrent:
            // Released, so the destroy the main thread asked for has happened: the handle is gone.
            out->rebindAnswer = eglMakeCurrent(dpy, surface, surface, context);
            out->rebindError = eglGetError();
            if (out->rebindAnswer == EGL_TRUE) (void)eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            break;
        default:
            (void)eglDestroyContext(dpy, context);
            break;
        }
        (void)eglDestroySurface(dpy, surface);
    }

    // ReadbackRace's reader: one context, no pause between frames, until it has read them all.
    void ReadWithoutPause(Rig* rig, int index) {
        ThreadReport* out = &rig->report->threads[index];
        const EGLSurface surface = eglCreatePbufferSurface(rig->dpy, rig->config, kPbufferAttribs);
        const EGLContext context = eglCreateContext(rig->dpy, rig->config, EGL_NO_CONTEXT, kContextAttribs);
        if (surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
            eglMakeCurrent(rig->dpy, surface, surface, context) == EGL_TRUE) {
            out->current = 1;
            const Drawing drawing = BuildDrawing(index, out);
            for (int frame = 0; frame < kRaceReaderFrames; ++frame) DrawFrame(drawing, frame, out);
            (void)eglMakeCurrent(rig->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            (void)eglDestroyContext(rig->dpy, context);
        }
        if (surface != EGL_NO_SURFACE) (void)eglDestroySurface(rig->dpy, surface);
        rig->readerDone.store(1);
    }

    // ReadbackRace's churners: create a context of its own, make it current, give it objects (a
    // texture, deleted every other cycle and otherwise left to die with the context, and a vertex
    // array), clear and read back, release and destroy it. Until the reader is done.
    void ChurnContexts(Rig* rig, int index) {
        ThreadReport* out = &rig->report->threads[index];
        const EGLSurface surface = eglCreatePbufferSurface(rig->dpy, rig->config, kPbufferAttribs);
        if (surface == EGL_NO_SURFACE) return;
        out->current = 1;
        out->linked = 1;
        for (int cycle = 0; rig->readerDone.load() == 0 && cycle < 400; ++cycle) {
            const EGLContext context = eglCreateContext(rig->dpy, rig->config, EGL_NO_CONTEXT, kContextAttribs);
            if (context == EGL_NO_CONTEXT || eglMakeCurrent(rig->dpy, surface, surface, context) != EGL_TRUE) {
                ++out->cycleFailures;
                if (context != EGL_NO_CONTEXT) (void)eglDestroyContext(rig->dpy, context);
                continue;
            }
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            const Uint32 texel = ChurnColourOf(index, cycle);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
            GLuint vertexArray = 0;
            glGenVertexArrays(1, &vertexArray);
            glBindVertexArray(vertexArray);
            const Uint32 colour = ChurnColourOf(index, cycle);
            glViewport(0, 0, 8, 8);
            glClearColor(static_cast<float>(colour & 0xFFu) / 255.0f, static_cast<float>((colour >> 8) & 0xFFu) / 255.0f,
                         static_cast<float>((colour >> 16) & 0xFFu) / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            Uint32 pixel = 0x5A5A5A5Au;
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
            Tally(out, cycle, pixel, colour);
            if (cycle % 2 == 0) glDeleteTextures(1, &texture); // the odd ones die with the context
            (void)eglMakeCurrent(rig->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            (void)eglDestroyContext(rig->dpy, context);
            ++out->cycles;
        }
        (void)eglDestroySurface(rig->dpy, surface);
    }

    void RunShape(Rig& rig) {
        std::thread threads[kThreads];
        if (rig.shape == Shape::ReadbackRace) {
            threads[0] = std::thread(ReadWithoutPause, &rig, 0);
            for (int i = 1; i <= kRaceChurners; ++i) threads[i] = std::thread(ChurnContexts, &rig, i);
            for (int i = 0; i <= kRaceChurners; ++i) threads[i].join();
            return;
        }
        if (rig.shape == Shape::SharedRoot) {
            rig.share = eglCreateContext(rig.dpy, rig.config, EGL_NO_CONTEXT, kContextAttribs);
        }
        if (rig.shape == Shape::Siblings) {
            // The group's first context, and the rest sharing with it; nothing else holds the group.
            rig.given[0] = eglCreateContext(rig.dpy, rig.config, EGL_NO_CONTEXT, kContextAttribs);
            for (int i = 1; i < kThreads; ++i)
                rig.given[i] = eglCreateContext(rig.dpy, rig.config, rig.given[0], kContextAttribs);
        }
        for (int i = 0; i < kThreads; ++i) threads[i] = std::thread(RenderOnThread, &rig, i);
        if (rig.shape == Shape::DestroyWhileCurrent) {
            // Each context is destroyed from HERE while its own thread still has it current. EGL
            // answers EGL_TRUE and destroys it once that thread releases it.
            for (int i = 0; i < kThreads; ++i) {
                ThreadReport& t = rig.report->threads[i];
                if (WaitUntil([&] { return rig.halfway[i].load() != 0; }, 30000)) {
                    const EGLContext context = rig.published[i].load();
                    if (context != EGL_NO_CONTEXT) {
                        t.destroyAnswer = eglDestroyContext(rig.dpy, context);
                        t.destroyError = eglGetError();
                    }
                }
                rig.destroyed[i].store(1);
            }
        }
        for (auto& thread : threads) thread.join();
        if (rig.shape == Shape::ReleaseOnly) {
            for (int i = 0; i < kThreads; ++i) {
                const EGLContext context = rig.published[i].load();
                if (context != EGL_NO_CONTEXT) (void)eglDestroyContext(rig.dpy, context);
            }
        }
        if (rig.share != EGL_NO_CONTEXT) (void)eglDestroyContext(rig.dpy, rig.share);
    }

    Report RunClient(const ServerProcess& server, const std::string& logBase, Shape shape) {
        Report report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", server.endpoint.c_str(), 1);
            ::unsetenv("MOBILEGL_IPC_SURFACE");
            ::unsetenv("MOBILEGL_BACKEND_TYPE");
            static Report r{};
            static Rig rig;
            rig.shape = shape;
            rig.report = &r;
            rig.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            EGLint major = 0;
            EGLint minor = 0;
            if (rig.dpy != EGL_NO_DISPLAY && eglInitialize(rig.dpy, &major, &minor) == EGL_TRUE) {
                (void)eglBindAPI(EGL_OPENGL_API);
                const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
                EGLint count = 0;
                if (eglChooseConfig(rig.dpy, configAttribs, &rig.config, 1, &count) == EGL_TRUE && count > 0) {
                    r.initialized = 1;
                    RunShape(rig);
                    r.finished = 1;
                }
                (void)eglTerminate(rig.dpy);
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
        if (::poll(&pfd, 1, 60000) > 0) {
            if (::read(fds[0], &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report))) report = Report{};
        } else {
            ::kill(pid, SIGKILL); // a hang: report.initialized stays 0 and the case says so
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }

    const char* NameOf(Shape shape) {
        switch (shape) {
        case Shape::SharedRoot: return "concurrent-shared";
        case Shape::Unshared: return "concurrent-unshared";
        case Shape::ReleaseOnly: return "concurrent-release";
        case Shape::Siblings: return "concurrent-siblings";
        case Shape::DestroyWhileCurrent: return "concurrent-destroy-current";
        case Shape::ReadbackRace: return "concurrent-race";
        }
        return "concurrent";
    }

    void Check(Shape shape) {
        ::signal(SIGPIPE, SIG_IGN);
        ServerProcess server;
        const std::string label = NameOf(shape);
        ASSERT_TRUE(LaunchUnixInProcessServer(&server, label)) << server.Log();
        const std::string logBase = "/tmp/mgl-" + label + "-client-" + std::to_string(::getpid()) + ".log";
        Debug::TruncateRoleLogs(logBase.c_str());
        const Report r = RunClient(server, logBase, shape);
        ASSERT_EQ(r.initialized, 1) << "the client hung or did not come up; client log " << logBase << "\n"
                                    << server.Log();
        ASSERT_EQ(r.finished, 1) << "the client did not finish";
        // Every record named something its own context's share group holds. The default
        // framebuffer's placeholder textures were the standing exception: one process-wide set,
        // recorded in the group bound when they were born and named by every context's draw.
        const std::string serverLog = server.Log();
        EXPECT_EQ(serverLog.find("has no applier resource record"), std::string::npos)
            << "a record named a texture its share group holds no record for";
        const int threads = shape == Shape::ReadbackRace ? 1 + kRaceChurners : kThreads;
        for (int i = 0; i < threads; ++i) {
            const ThreadReport& t = r.threads[i];
            if (t.current == 0) GTEST_SKIP() << "a context did not go current (no headless EGL here?)";
            EXPECT_EQ(t.linked, 1) << "thread " << i << " (compiled " << t.vertexCompiled << "/" << t.fragmentCompiled
                                   << "): " << t.linkLog;
            EXPECT_EQ(t.bad, 0) << "thread " << i << " read back 0x" << std::hex << t.firstBad << " for 0x"
                                << t.firstExpected << std::dec << " first at frame " << t.firstBadFrame << " ("
                                << t.good << " frames right)";
            EXPECT_GT(t.good, 0) << "thread " << i;
            if (shape == Shape::ReleaseOnly) EXPECT_GT(t.rebinds, 0) << "thread " << i;
            if (shape == Shape::DestroyWhileCurrent) {
                EXPECT_EQ(t.destroyWaited, 1) << "thread " << i;
                EXPECT_EQ(t.destroyAnswer, EGL_TRUE)
                    << "thread " << i << ": eglDestroyContext of a context current on another thread is deferred, "
                    << "not refused (error 0x" << std::hex << t.destroyError << ")";
                EXPECT_EQ(t.rebindAnswer, EGL_FALSE) << "thread " << i << ": the context outlived its release";
                EXPECT_EQ(t.rebindError, EGL_BAD_CONTEXT) << "thread " << i;
            }
            if (shape == Shape::ReadbackRace && i > 0) {
                EXPECT_GT(t.cycles, 3) << "churner " << i;
                EXPECT_EQ(t.cycleFailures, 0) << "churner " << i;
            }
        }
    }

} // namespace

TEST(ConcurrentContexts, ThreadsSharingOneRootContextEachDrawTheirOwnTexture) { Check(Shape::SharedRoot); }

TEST(ConcurrentContexts, ThreadsInSeparateShareGroupsEachDrawTheirOwnTexture) { Check(Shape::Unshared); }

TEST(ConcurrentContexts, ReleasingWithoutDestroyingLeavesTheOtherThreadsDrawing) { Check(Shape::ReleaseOnly); }

TEST(ConcurrentContexts, ShareGroupSiblingsOutliveTheGroupsFirstContext) { Check(Shape::Siblings); }

TEST(ConcurrentContexts, DestroyingAContextCurrentOnAnotherThreadIsDeferredUntilItsRelease) {
    Check(Shape::DestroyWhileCurrent);
}

TEST(ConcurrentContexts, AReadBackRacingOtherThreadsContextTeardownsIsNotLost) { Check(Shape::ReadbackRace); }
