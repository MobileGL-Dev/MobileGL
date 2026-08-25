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
//   (c) The extension string matches the configuration - where "the configuration" is
//       MOBILEGL_ASYNC_SHADER_COMPILE as this process inherited it, and NOT anything the
//       implementation says about itself. This is the half a recorded trace can never
//       cover - Iris and Sodium change their submission schedule the moment they see the
//       string - so it is asserted against a real backend's real GL_EXTENSIONS, through
//       both glGetString and glGetStringi.
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
//
// COMPILATION MODE IS PER PROCESS TOO. Every case here needs a particular configuration of
// MobileGL's shader compiler, and takes it from the ENVIRONMENT
// (MOBILEGL_ASYNC_SHADER_COMPILE, MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS) rather than by
// writing MG_Config::Features on the way past. Half of what those variables decide is
// latched before the first GL call - the compile pool and its threads, and the advertised
// extension list a backend builds once from the configuration in force at its first use -
// so an in-process poke could only ever have moved the other half; and on Android it could
// move nothing at all, because this module links against the shipping libMobileGL.so, which
// exports no such symbol. A case whose process is not in the configuration it needs SKIPS
// with that as its reason. CMakeLists.txt registers the extra ctest entries that put a
// process into each configuration (AsyncOn., AsyncOff., OptimisticShaderStatus.), so one
// ctest run still covers both sides of every switch. Run straight from a shell with nothing
// set - the on-device shape - the ambient configuration runs and the rest skip cleanly.
//
// WITHIN one process, "compiled on a worker" versus "compiled on this thread" is switched
// through glMaxShaderCompilerThreadsKHR, the extension's own entry point: a zero count joins
// everything outstanding and compiles inline from then on, any nonzero count lifts that
// again, and 0xFFFFFFFF asks for the implementation maximum (GL_Program.cpp,
// MaxShaderCompilerThreadsKHR_State). Doing it through the public call rather than the
// feature table means the switching is itself part of what these cases exercise.

#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

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

        // Whether this context advertises GL_KHR_parallel_shader_compile, which is exactly
        // "MobileGL is configured to compile asynchronously" as an application can see it:
        // the backends gate the string on AsyncShaderCompileEnabled() and on nothing else
        // (BackendObject_DirectGLES.cpp / BackendObject_DirectVulkan.cpp), and the string
        // is the only way MobileGL ever tells anyone. A case that needs asynchronous
        // compilation checks for it the way an application would, and skips without it.
        //
        // The INDEXED form, because that is the one a core-profile application reads.
        bool HasParallelShaderCompile() {
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i) {
                const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
                if (name != nullptr && std::string(name) == "GL_KHR_parallel_shader_compile") return true;
            }
            return false;
        }

        // glMaxShaderCompilerThreadsKHR writes process-wide state; a scenario that calls it
        // has to put the pool back or it changes how every scenario after it compiles.
        //
        // The restore is the extension's own "implementation maximum" spelling rather than a
        // hand-rolled poke at the pool. glMaxShaderCompilerThreadsKHR(0xFFFFFFFF) is defined
        // (GL_Program.cpp, MaxShaderCompilerThreadsKHR_State) as precisely the two steps this
        // used to perform through internal entry points - concurrency := the pool's full
        // thread count, then lift any suspension a zero count had armed - in the safer order,
        // since it raises the budget before re-admitting work rather than after. Going through
        // the public call also puts the restore path itself under test, and it is the only
        // spelling available on Android, where this module links the shipping shared library
        // and can reach nothing but the GL entry points.
        class CompilerThreadScope {
        public:
            CompilerThreadScope() = default;
            ~CompilerThreadScope() { glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu); }
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
            if (!HasParallelShaderCompile()) {
                GTEST_SKIP() << "this process is configured to compile inline "
                                "(GL_KHR_parallel_shader_compile is not advertised), so no compile can be "
                                "outstanding; the AsyncOn. ctest entries run this case with "
                                "MOBILEGL_ASYNC_SHADER_COMPILE=1";
            }
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
        // The extension string, read from a real backend that really brought a driver up.
        //
        // The expectation comes from the ENVIRONMENT, never from the implementation. This
        // case used to derive it by calling AsyncShaderCompileEnabled() - which is the same
        // function the backends gate the string on, so the two halves could only ever agree
        // and the case would have passed however wrong both of them were. Asserting an
        // implementation against itself pins nothing.
        //
        // MOBILEGL_ASYNC_SHADER_COMPILE is the whole input: the process inherited it before
        // any GL call, a backend builds its advertised list once from the configuration in
        // force at first use, and nothing in this process can move it afterwards. So reading
        // the variable IS reading the configuration, independently. With the variable unset
        // the configuration in force is MobileGL's built-in default, which only the
        // implementation knows - there is nothing independent left to compare against, and
        // this case says so rather than inventing an expectation. The AsyncOn. and AsyncOff.
        // ctest entries pin the variable to each of its two values, so one ctest run still
        // asserts both the advertised and the withdrawn side.
        TEST_F(AsyncCompileScenario, ExtensionStringMatchesTheConfiguration) {
            if (!Ready()) return;
            const AmbientQuirk configured = AmbientQuirkFromEnvironment("MOBILEGL_ASYNC_SHADER_COMPILE");
            if (configured == AmbientQuirk::Auto) {
                GTEST_SKIP() << "MOBILEGL_ASYNC_SHADER_COMPILE is unset, so the configuration in force is "
                                "MobileGL's built-in default and the only way to learn it would be to ask "
                                "the implementation this case exists to check; the AsyncOn. and AsyncOff. "
                                "ctest entries run it with the variable pinned to each of its two values";
            }
            const bool expected = configured == AmbientQuirk::On;

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
            if (!HasParallelShaderCompile()) {
                GTEST_SKIP() << "this process is configured to compile inline "
                                "(GL_KHR_parallel_shader_compile is not advertised), so a zero count has "
                                "nothing to settle; the AsyncOn. ctest entries run this case with "
                                "MOBILEGL_ASYNC_SHADER_COMPILE=1";
            }
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
        //
        // The two modes are selected through glMaxShaderCompilerThreadsKHR, the extension's
        // own entry point, rather than through the feature table: a zero count joins
        // everything outstanding and makes every later glCompileShader/glLinkProgram run its
        // body on the calling thread, and 0xFFFFFFFF lifts that again with the pool at its
        // full thread count (GL_Program.cpp, MaxShaderCompilerThreadsKHR_State; the compile
        // and link paths both gate on AsyncShaderCompileActive(), which is what the zero
        // count switches). So this is still one process comparing worker-built artifacts
        // against inline-built ones - just asked for the way an application asks.
        TEST_F(AsyncCompileScenario, AsyncAndSyncProgramsRenderIdenticalFrames) {
            if (!Ready()) return;
            if (!HasParallelShaderCompile()) {
                GTEST_SKIP() << "this process is configured to compile inline "
                                "(GL_KHR_parallel_shader_compile is not advertised), so both halves would "
                                "be the same inline build and the comparison would be vacuous; the "
                                "AsyncOn. ctest entries run this case with MOBILEGL_ASYNC_SHADER_COMPILE=1";
            }
            const CompilerThreadScope threads;

            Image asyncImage;
            {
                glMaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
                const GLuint program = BuildProgram();
                ASSERT_NE(program, 0u);
                asyncImage = DrawFrameWith(program);
                glDeleteProgram(program);
            }

            Image syncImage;
            {
                glMaxShaderCompilerThreadsKHR(0);
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
            if (!HasParallelShaderCompile()) {
                GTEST_SKIP() << "this process is configured to compile inline "
                                "(GL_KHR_parallel_shader_compile is not advertised), so nothing would be "
                                "built on a worker and there is no per-worker state to leak; the AsyncOn. "
                                "ctest entries run this case with MOBILEGL_ASYNC_SHADER_COMPILE=1";
            }
            constexpr int kPrograms = 12;

            std::vector<GLuint> programs;
            {
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

        // The Iris two-phase shape end to end on a real driver, with the optimistic-status
        // quirk on: phase 1 compiles each stage and reads its log then its status (both
        // answered optimistically), links, detaches and deletes the shaders for every
        // program with no program-level read anywhere; phase 2 then checks every link and
        // draws every program. Deliberately NOT built on the harness CompileProgram(),
        // whose status read would join and collapse the phase-1 overlap this exists to
        // exercise. What the unit suite cannot see - worker-produced artifacts the backend
        // then mis-renders - shows up here as a wrong quadrant signature.
        TEST_F(AsyncCompileScenario, IrisShapedTwoPhaseBatchRendersCorrectly) {
            if (!Ready()) return;
            // The quirk is off by default and never advertised, so unlike the cases above
            // there is no GL observable that says whether it is in force - only the variable
            // that put it there. It also has to be set BEFORE this process started for the
            // shape to be the real one: the optimistic answer is latched per compile, and a
            // quirk switched on mid-process would only cover the compiles after it.
            if (AmbientQuirkFromEnvironment("MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS") != AmbientQuirk::On) {
                GTEST_SKIP() << "this case is the optimistic-status quirk's end-to-end shape and needs it on "
                                "for the whole process; the OptimisticShaderStatus. ctest entries run it with "
                                "MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS=1";
            }
            if (!HasParallelShaderCompile()) {
                GTEST_SKIP() << "the optimistic status only ever applies to a compile that is still in flight "
                                "(OptimisticShaderStatusActive() requires AsyncShaderCompileActive()), and "
                                "this process is configured to compile inline";
            }
            constexpr int kPrograms = 12;

            // Distinct per program (so neither the source memo nor the adoption map turns
            // a compile into a no-op) but a pure pass-through at runtime: the bulk sits in
            // a branch a zero-initialised uniform never takes.
            const auto fragmentSource = [](const int index) {
                std::string source = "#version 330 core\nin vec3 vColor;\nout vec4 oColor;\n";
                source += "uniform float uGate" + std::to_string(index) + ";\n";
                source += "void main() {\n    oColor = vec4(vColor, 1.0);\n";
                source += "    if (uGate" + std::to_string(index) + " > 1e30) {\n        float acc = 1.0;\n";
                for (int i = 0; i < 60; ++i) {
                    source += "        acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0);\n";
                }
                source += "        oColor = vec4(acc);\n    }\n}\n";
                return source;
            };

            std::vector<GLuint> programs;
            {
                const CompilerThreadScope threads;
                glMaxShaderCompilerThreadsKHR(1);

                for (int i = 0; i < kPrograms; ++i) {
                    m_sources.push_back(fragmentSource(i));
                    const char* fsText = m_sources.back().c_str();

                    const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                    glShaderSource(vs, 1, &kVertexSource, nullptr);
                    glCompileShader(vs);
                    (void)ShaderInfoLog(vs);       // Iris's exact order: the log first...
                    (void)ShaderCompileStatus(vs); // ...then the status; both optimistic.

                    const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                    glShaderSource(fs, 1, &fsText, nullptr);
                    glCompileShader(fs);
                    (void)ShaderInfoLog(fs);
                    (void)ShaderCompileStatus(fs);

                    const GLuint program = glCreateProgram();
                    glAttachShader(program, vs);
                    glAttachShader(program, fs);
                    glBindAttribLocation(program, 0, "aPos");
                    glBindAttribLocation(program, 1, "aColor");
                    glLinkProgram(program);
                    glDetachShader(program, vs);
                    glDetachShader(program, fs);
                    glDeleteShader(vs);
                    glDeleteShader(fs);
                    programs.push_back(program);
                }
            }

            for (int i = 0; i < kPrograms; ++i) {
                const GLuint program = programs[static_cast<std::size_t>(i)];
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                ASSERT_EQ(linked, GL_TRUE) << "program " << i;
                const Image image = DrawFrameWith(program);
                EXPECT_EQ(image.QuadrantSignature(), "blue,green,red,white") << "program " << i;
            }
            for (const GLuint program : programs) glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

    } // namespace
} // namespace MGITest
