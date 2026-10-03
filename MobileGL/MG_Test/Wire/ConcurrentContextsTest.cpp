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
// DISABLED: KNOWN TO FAIL. With six threads, a thread's read-backs come back untouched once another
// thread has released and destroyed its context (both shapes, most runs), and a run sometimes hangs
// until the client is killed. On the phone an unshared thread can also read back nothing from its
// first frame. Run it with --gtest_also_run_disabled_tests while working on that.

#include "P12ServerRig.h"

#include <EGL/egl.h>

#include <thread>

extern "C" {
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void glGenTextures(GLsizei n, GLuint* textures);
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

    struct ThreadReport {
        Int32 current = 0;
        Int32 linked = 0;
        Int32 good = 0;
        Int32 bad = 0;
        Uint32 firstBad = 0;
        Int32 firstBadFrame = -1;
        Int32 vertexCompiled = -1;
        Int32 fragmentCompiled = -1;
        char linkLog[200] = {};
    };

    struct Report {
        Int32 initialized = 0;
        ThreadReport threads[kThreads];
    };

    // A<<24|B<<16|G<<8|R of the texel each thread draws.
    Uint32 ColourOf(int index) { return 0xFF000000u | (0x40u + 0x50u * static_cast<Uint32>(index)) << 8 | 0x20u; }

    void RenderOnThread(EGLDisplay dpy, EGLConfig config, EGLContext share, int index, ThreadReport* out) {
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        const EGLSurface surface = eglCreatePbufferSurface(dpy, config, pbufferAttribs);
        const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
        const EGLContext context = eglCreateContext(dpy, config, share, attribs);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            eglMakeCurrent(dpy, surface, surface, context) != EGL_TRUE)
            return;
        out->current = 1;
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
        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        out->linked = linked;
        glGetShaderiv(vertex, GL_COMPILE_STATUS, &out->vertexCompiled);
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &out->fragmentCompiled);
        if (!linked) glGetProgramInfoLog(program, sizeof(out->linkLog) - 1, nullptr, out->linkLog);
        GLuint vertexArray = 0;
        glGenVertexArrays(1, &vertexArray);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        const Uint32 colour = ColourOf(index);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &colour);
        for (int frame = 0; frame < kFrames; ++frame) {
            glViewport(0, 0, 8, 8);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "image"), 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glBindVertexArray(vertexArray);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            Uint32 pixel = 0x5A5A5A5Au;
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
            if (pixel == colour) {
                ++out->good;
            } else {
                if (out->bad++ == 0) {
                    out->firstBad = pixel;
                    out->firstBadFrame = frame;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        (void)eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        (void)eglDestroyContext(dpy, context);
        (void)eglDestroySurface(dpy, surface);
    }

    Report RunClient(const ServerProcess& server, const std::string& logBase, bool shareRoot) {
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
            Report r{};
            const EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            EGLint major = 0;
            EGLint minor = 0;
            if (dpy != EGL_NO_DISPLAY && eglInitialize(dpy, &major, &minor) == EGL_TRUE) {
                (void)eglBindAPI(EGL_OPENGL_API);
                const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
                EGLConfig config = nullptr;
                EGLint count = 0;
                if (eglChooseConfig(dpy, configAttribs, &config, 1, &count) == EGL_TRUE && count > 0) {
                    r.initialized = 1;
                    const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
                    const EGLContext share =
                        shareRoot ? eglCreateContext(dpy, config, EGL_NO_CONTEXT, attribs) : EGL_NO_CONTEXT;
                    std::thread threads[kThreads];
                    for (int i = 0; i < kThreads; ++i)
                        threads[i] = std::thread(RenderOnThread, dpy, config, share, i, &r.threads[i]);
                    for (auto& thread : threads) thread.join();
                    if (share != EGL_NO_CONTEXT) (void)eglDestroyContext(dpy, share);
                }
                (void)eglTerminate(dpy);
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
            if (::read(fds[0], &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report))) report = Report{};
        } else {
            ::kill(pid, SIGKILL);
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }

    void Check(bool shareRoot) {
        ::signal(SIGPIPE, SIG_IGN);
        ServerProcess server;
        const std::string label = shareRoot ? "concurrent-shared" : "concurrent-unshared";
        ASSERT_TRUE(LaunchUnixInProcessServer(&server, label)) << server.Log();
        const std::string logBase = "/tmp/mgl-" + label + "-client-" + std::to_string(::getpid()) + ".log";
        Debug::TruncateRoleLogs(logBase.c_str());
        const Report r = RunClient(server, logBase, shareRoot);
        ASSERT_EQ(r.initialized, 1) << server.Log();
        for (int i = 0; i < kThreads; ++i) {
            const ThreadReport& t = r.threads[i];
            if (t.current == 0) GTEST_SKIP() << "a context did not go current (no headless EGL here?)";
            EXPECT_EQ(t.linked, 1) << "thread " << i << " (compiled " << t.vertexCompiled << "/" << t.fragmentCompiled
                                   << "): " << t.linkLog;
            EXPECT_EQ(t.bad, 0) << "thread " << i << " read back 0x" << std::hex << t.firstBad << " for 0x"
                                << ColourOf(i) << std::dec << " first at frame " << t.firstBadFrame << " ("
                                << t.good << " frames right)";
        }
    }

} // namespace

TEST(ConcurrentContexts, DISABLED_ThreadsSharingOneRootContextEachDrawTheirOwnTexture) { Check(true); }

TEST(ConcurrentContexts, DISABLED_ThreadsInSeparateShareGroupsEachDrawTheirOwnTexture) { Check(false); }
