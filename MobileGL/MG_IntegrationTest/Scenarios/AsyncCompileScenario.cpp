// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/AsyncCompileScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario E - asynchronous shader compilation and GL_KHR_parallel_shader_compile
// on a REAL driver.
//
// WHY THIS EXISTS ALONGSIDE THE UNIT SUITES. MG_Test/Program's async suites already
// drive the same GL entry points, but they stop at the frontend: nothing there ever
// reaches a driver, so nothing there can catch the failure this scenario is built for
// - artifacts produced on a worker thread that the BACKEND then rejects, mis-binds or
// renders differently from the ones the GL thread produced. The frontend cannot tell
// the two apart; a pixel can.
//
// The five things it pins, in order:
//
//   (a) 64 heavy compiles are enqueued and polled through GL_COMPLETION_STATUS_KHR.
//       At least one must be observed GL_FALSE - i.e. the query really answers while
//       work is outstanding rather than silently joining. Skipped, never failed, when
//       the machine drained the whole batch before the first poll: a fast box must not
//       be able to turn this into a red.
//   (b) Forcing the join afterwards produces the right answer for every one of them:
//       GL_COMPILE_STATUS true, an empty info log, and a program that links.
//   (c) The extension string matches the configuration. This is the half a recorded
//       trace can never cover - Iris and Sodium change their submission schedule the
//       moment they see the string - so it is asserted against a real backend's real
//       GL_EXTENSIONS, through both glGetString and glGetStringi.
//   (d) glMaxShaderCompilerThreadsKHR(0) leaves nothing in flight: every subsequent
//       GL_COMPLETION_STATUS_KHR reads GL_TRUE immediately, and compilation after it
//       is synchronous. That is what the extension requires of a zero count.
//   (e) THE ONE THAT NEEDS A GPU: the same frame, drawn with programs compiled and
//       linked asynchronously and then with programs compiled and linked inline, must
//       come out byte-identical under glReadPixels. Anything the worker thread got
//       wrong about the compile environment, the reflection or the SPIR-V shows up
//       here as a pixel difference and nowhere else.
//
// Backend selection is the module's usual one process, one backend (MOBILEGL_BACKEND_TYPE),
// so this file runs twice per ctest invocation.

#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#include "Config.h"
#include "MG_Util/Async/ShaderCompilePool.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

// GL_KHR_parallel_shader_compile. Spelled out rather than relying on the host's
// glext.h: this module is built against whatever GL headers the machine has, and an
// older one has neither token. Both are also GL_*_ARB with identical values.
#ifndef GL_MAX_SHADER_COMPILER_THREADS_KHR
#define GL_MAX_SHADER_COMPILER_THREADS_KHR 0x91B0
#endif
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif

// The entry point under test, resolved by the linker straight into MobileGL_s like
// every other gl* call in this module. Declared here for the same reason as the
// tokens above.
extern "C" void glMaxShaderCompilerThreadsKHR(GLuint count);

namespace MGITest {
    namespace {

        using MobileGL::MG_Config::QuirkOverride;

        // Same shape as the other scenarios: a two-attribute pass-through, so the only
        // thing that can differ between the two compilation modes is the compilation.
        constexpr const char* kVertexSource = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() {
    oColor = vec4(vColor, 1.0);
}
)";

        // Asymmetric in both axes, so a mode difference that also happens to be a
        // symmetry of the image cannot hide (the same reason OrientationScenario draws
        // quadrants rather than stripes).
        struct Vertex {
            float x, y;
            float r, g, b;
        };

        void AppendQuad(std::vector<Vertex>& out, float x0, float x1, float y0, float y1, float r, float g, float b) {
            const Vertex bl{x0, y0, r, g, b};
            const Vertex br{x1, y0, r, g, b};
            const Vertex tr{x1, y1, r, g, b};
            const Vertex tl{x0, y1, r, g, b};
            out.insert(out.end(), {bl, br, tr, bl, tr, tl});
        }

        std::vector<Vertex> QuadrantGeometry() {
            std::vector<Vertex> vertices;
            vertices.reserve(24);
            AppendQuad(vertices, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f); // bottom-left: blue
            AppendQuad(vertices, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f);  // bottom-right: green
            AppendQuad(vertices, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);  // top-left: red
            AppendQuad(vertices, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);   // top-right: white
            return vertices;
        }

        // Expensive enough that a compile is not instantaneous, and distinct per index so
        // the source-hash memo never turns one into a no-op: without both properties the
        // pool has no backlog and (a) has nothing to observe.
        std::string BulkyFragmentSource(int index) {
            std::string source = "#version 330 core\n";
            source += "in vec3 vColor;\nout vec4 oColor;\n";
            source += "uniform float uSeed" + std::to_string(index) + ";\n";
            source += "void main() {\n    float acc = uSeed" + std::to_string(index) + ";\n";
            for (int i = 0; i < 320; ++i) {
                source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
            }
            source += "    oColor = vec4(vColor * acc, 1.0);\n}\n";
            return source;
        }

        // MOBILEGL_ASYNC_SHADER_COMPILE decides the ambient mode; a scenario that wants
        // the other one says so here and gets the ambient one back on scope exit. Forcing
        // it in-process is what lets ONE ctest run compare the two modes against each
        // other - the whole point of (e).
        class AsyncModeScope {
        public:
            explicit AsyncModeScope(bool async) : m_saved(MobileGL::MG_Config::Features.AsyncShaderCompile) {
                MobileGL::MG_Config::Features.AsyncShaderCompile =
                    async ? QuirkOverride::ForceOn : QuirkOverride::ForceOff;
            }
            ~AsyncModeScope() { MobileGL::MG_Config::Features.AsyncShaderCompile = m_saved; }
            AsyncModeScope(const AsyncModeScope&) = delete;
            AsyncModeScope& operator=(const AsyncModeScope&) = delete;

        private:
            const QuirkOverride m_saved;
        };

        // glMaxShaderCompilerThreadsKHR writes process-wide state; a scenario that calls
        // it has to put the pool back or it changes how every scenario after it compiles.
        class CompilerThreadScope {
        public:
            CompilerThreadScope() = default;
            ~CompilerThreadScope() {
                MobileGL::MG_Util::Async::SetAsyncShaderCompileSuspended(false);
                auto& pool = MobileGL::MG_Util::Async::ShaderCompilePool::Get();
                pool.SetMaxConcurrency(pool.GetThreadCount());
            }
            CompilerThreadScope(const CompilerThreadScope&) = delete;
            CompilerThreadScope& operator=(const CompilerThreadScope&) = delete;
        };

        GLint ShaderCompletion(GLuint shader) {
            GLint status = -1;
            glGetShaderiv(shader, GL_COMPLETION_STATUS_KHR, &status);
            return status;
        }

        GLint ShaderCompileStatus(GLuint shader) {
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            return status;
        }

        std::string ShaderInfoLog(GLuint shader) {
            GLint length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            if (length <= 0) return std::string();
            std::vector<char> buffer(static_cast<std::size_t>(length));
            GLsizei written = 0;
            glGetShaderInfoLog(shader, length, &written, buffer.data());
            return std::string(buffer.data(), static_cast<std::size_t>(written));
        }

        class AsyncCompileScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                const std::vector<Vertex> vertices = QuadrantGeometry();
                m_vertexCount = static_cast<int>(vertices.size());
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(),
                             GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8));
                glBindVertexArray(0);

                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
            }

            // A fresh program every time, compiled and linked in whatever mode is in
            // force. Reusing one would defeat the comparison: the second mode would just
            // read the first mode's artifacts back out of the memo.
            GLuint BuildProgram() {
                std::string error;
                const GLuint program = CompileProgram(kVertexSource, kFragmentSource, &error);
                EXPECT_NE(program, 0u) << error;
                return program;
            }

            Image DrawFrameWith(GLuint program) {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
                glBindVertexArray(0);
                Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            }

            // Enqueues `count` distinct heavy compiles and returns their names WITHOUT
            // reading anything back, so the pool is left with a real backlog.
            std::vector<GLuint> EnqueueBacklog(int count, int seedBase) {
                std::vector<GLuint> shaders;
                shaders.reserve(static_cast<std::size_t>(count));
                m_sources.reserve(m_sources.size() + static_cast<std::size_t>(count));
                for (int i = 0; i < count; ++i) {
                    m_sources.push_back(BulkyFragmentSource(seedBase + i));
                    const char* text = m_sources.back().c_str();
                    const GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
                    glShaderSource(shader, 1, &text, nullptr);
                    glCompileShader(shader);
                    shaders.push_back(shader);
                }
                return shaders;
            }

            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            int m_vertexCount = 0;
            // Kept alive for the whole case: glShaderSource copies, but keeping the
            // strings makes a failure message able to name the source it came from.
            std::vector<std::string> m_sources;
        };

        // ---- (a) + (b) ------------------------------------------------------------
        // A backlog is enqueued, polled without joining, then forced to settle and
        // checked for correctness. Both halves in one case on purpose: (b) is only
        // interesting for shaders that (a) proved were genuinely still outstanding.
        TEST_F(AsyncCompileScenario, CompletionStatusPollingThenForcedJoin) {
            if (!Ready()) return;
            const AsyncModeScope async(true);
            const CompilerThreadScope threads;
            // One worker, so the queue behind it is what the poll observes.
            glMaxShaderCompilerThreadsKHR(1);

            const std::vector<GLuint> shaders = EnqueueBacklog(64, 6000);

            int outstanding = 0;
            for (const GLuint shader : shaders) {
                const GLint completion = ShaderCompletion(shader);
                ASSERT_TRUE(completion == GL_TRUE || completion == GL_FALSE)
                    << "GL_COMPLETION_STATUS_KHR returned " << completion;
                if (completion == GL_FALSE) ++outstanding;
            }
            if (outstanding == 0) {
                GTEST_SKIP() << "this machine drained 64 heavy compiles before the first poll; "
                                "nothing was outstanding to observe";
            }

            // (b) Forced join: every one of them is correct, and usable.
            for (const GLuint shader : shaders) {
                EXPECT_EQ(ShaderCompileStatus(shader), GL_TRUE) << ShaderInfoLog(shader);
                EXPECT_TRUE(ShaderInfoLog(shader).empty());
                EXPECT_EQ(ShaderCompletion(shader), GL_TRUE) << "GL_COMPILE_STATUS must have joined";
            }

            // And a link over one of them really produces a usable program on this driver.
            const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &kVertexSource, nullptr);
            glCompileShader(vs);
            const GLuint program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, shaders.front());
            glBindAttribLocation(program, 0, "aPos");
            glBindAttribLocation(program, 1, "aColor");
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            EXPECT_EQ(linked, GL_TRUE);
            EXPECT_GE(glGetUniformLocation(program, "uSeed6000"), 0);

            glDeleteProgram(program);
            glDeleteShader(vs);
            for (const GLuint shader : shaders) glDeleteShader(shader);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // ---- (c) ------------------------------------------------------------------
        // The extension string, read from a real backend that really brought a driver
        // up. No mode forcing here: a backend builds its advertised list once, from the
        // configuration in force at its first use, so the meaningful assertion is
        // against the AMBIENT configuration - which is exactly what makes this case
        // worth running in both of the suite's flag states.
        TEST_F(AsyncCompileScenario, ExtensionStringMatchesTheConfiguration) {
            if (!Ready()) return;
            const bool expected = MobileGL::MG_Util::Async::AsyncShaderCompileEnabled();

            const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
            ASSERT_NE(extensions, nullptr);
            const std::string extensionString(extensions);
            const bool inString = extensionString.find("GL_KHR_parallel_shader_compile") != std::string::npos;
            EXPECT_EQ(inString, expected)
                << "backend " << Gl().BackendName() << " GL_EXTENSIONS = " << extensionString;

            // LWJGL builds GLCapabilities from the INDEXED form on a core profile, so the
            // two spellings disagreeing would be invisible to the check above and fatal
            // to a real application.
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            ASSERT_GT(count, 0);
            bool inIndexed = false;
            for (GLint i = 0; i < count; ++i) {
                const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
                if (name != nullptr && std::string(name) == "GL_KHR_parallel_shader_compile") inIndexed = true;
            }
            EXPECT_EQ(inIndexed, expected);

            // The companion query, which an application reads right after the string.
            GLint maxThreads = -1;
            glGetIntegerv(GL_MAX_SHADER_COMPILER_THREADS_KHR, &maxThreads);
            if (expected) {
                EXPECT_GE(maxThreads, 1);
            } else {
                EXPECT_EQ(maxThreads, 0);
            }
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // ---- (d) ------------------------------------------------------------------
        // A zero count must leave nothing in flight and keep it that way.
        TEST_F(AsyncCompileScenario, ZeroCompilerThreadsSettlesEverythingImmediately) {
            if (!Ready()) return;
            const AsyncModeScope async(true);
            const CompilerThreadScope threads;
            glMaxShaderCompilerThreadsKHR(1);

            const std::vector<GLuint> backlog = EnqueueBacklog(48, 6200);
            glMaxShaderCompilerThreadsKHR(0);

            for (const GLuint shader : backlog) {
                EXPECT_EQ(ShaderCompletion(shader), GL_TRUE)
                    << "glMaxShaderCompilerThreadsKHR(0) must join everything still in flight";
                EXPECT_EQ(ShaderCompileStatus(shader), GL_TRUE) << ShaderInfoLog(shader);
            }

            // Compilation after the zero count is synchronous too.
            const std::vector<GLuint> serial = EnqueueBacklog(6, 6300);
            for (const GLuint shader : serial) {
                EXPECT_EQ(ShaderCompletion(shader), GL_TRUE) << "a compile after a zero count must be synchronous";
            }

            for (const GLuint shader : backlog) glDeleteShader(shader);
            for (const GLuint shader : serial) glDeleteShader(shader);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // ---- (e) ------------------------------------------------------------------
        // The one that needs the GPU. Two programs, identical source, one built with
        // compilation and linking on worker threads and one built inline; the frames
        // they draw must be byte-identical.
        //
        // Compared through the DEFAULT framebuffer deliberately: that is where the
        // backend's orientation and present path live, so the comparison covers the
        // whole pipeline rather than the reflection tables alone.
        TEST_F(AsyncCompileScenario, AsyncAndSyncProgramsRenderIdenticalFrames) {
            if (!Ready()) return;

            Image asyncImage;
            {
                const AsyncModeScope async(true);
                const GLuint program = BuildProgram();
                ASSERT_NE(program, 0u);
                asyncImage = DrawFrameWith(program);
                glDeleteProgram(program);
            }

            Image syncImage;
            {
                const AsyncModeScope async(false);
                const GLuint program = BuildProgram();
                ASSERT_NE(program, 0u);
                syncImage = DrawFrameWith(program);
                glDeleteProgram(program);
            }

            ASSERT_FALSE(asyncImage.Empty());
            ASSERT_FALSE(syncImage.Empty());
            // The frame is the expected one in the first place - two identically WRONG
            // frames would otherwise pass.
            EXPECT_EQ(asyncImage.QuadrantSignature(), "blue,green,red,white")
                << "the asynchronously compiled program did not draw the expected frame";
            EXPECT_EQ(asyncImage, syncImage)
                << "asynchronous and synchronous compilation rendered different frames ("
                << asyncImage.ByteDiffCount(syncImage) << " bytes differ); backend " << Gl().BackendName();
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // The same comparison over a batch, which is the shape a shaderpack load has:
        // many programs enqueued before any of them is read back, then each one drawn.
        // A per-worker state leak (glslang's thread-local pools are the obvious
        // candidate) shows up here and not in the single-program case above.
        TEST_F(AsyncCompileScenario, ABatchOfAsyncProgramsAllRenderCorrectly) {
            if (!Ready()) return;
            constexpr int kPrograms = 12;

            std::vector<GLuint> programs;
            {
                const AsyncModeScope async(true);
                const CompilerThreadScope threads;
                glMaxShaderCompilerThreadsKHR(1);
                // Everything enqueued before anything is read: the only shape in which
                // more than one job is in flight at a time.
                for (int i = 0; i < kPrograms; ++i) {
                    programs.push_back(BuildProgram());
                }
            }

            for (int i = 0; i < kPrograms; ++i) {
                ASSERT_NE(programs[static_cast<std::size_t>(i)], 0u) << "program " << i;
                const Image image = DrawFrameWith(programs[static_cast<std::size_t>(i)]);
                EXPECT_EQ(image.QuadrantSignature(), "blue,green,red,white") << "program " << i;
            }
            for (const GLuint program : programs) glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

    } // namespace
} // namespace MGITest
