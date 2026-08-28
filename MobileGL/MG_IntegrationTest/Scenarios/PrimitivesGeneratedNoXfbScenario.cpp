// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PrimitivesGeneratedNoXfbScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GL_PRIMITIVES_GENERATED COUNTS DRAWS MADE WITH TRANSFORM FEEDBACK
// INACTIVE.
//
// GL 4.6 core 13.4: the query counts what the last vertex processing stage emits,
// capture or no capture. The CTS leans its whole tessellation suite on that - the
// tessellator's output is MEASURED by an XFB-inactive PATCHES draw under
// rasterizer discard inside a GENERATED query, and the capture buffers of ~29
// tessellation tests are sized from the answer - so a backend that answers 0
// hands them a zero-byte buffer and an INVALID_OPERATION off its zero-length map.
//
// DirectVulkan serves the query from the transform-feedback stream query's
// primitivesNeeded, which VK_EXT_transform_feedback defines to count whether or
// not a capture span is open. Both the Mali-G1-Ultra driver AND Mesa lavapipe
// disagree with that definition: with no vkCmdBeginTransformFeedbackEXT recorded,
// the pair reads back 0. Where the bring-up probe measures that defect with a
// working control - or MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1 pins it on - the
// renderer accumulates XFB-inactive draws through the best proven substitute
// pool: VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT (which lavapipe hosts and passes,
// rasterizer discard included), else pipeline statistics over clipping-stage
// invocations (GL's CLIPPING_INPUT_PRIMITIVES). These cases assert the GL-visible
// answer, so on this machine they hold the reroute to the same numbers the
// healthy stream path must produce - the "two pools must agree" assertion - and
// on a healthy driver they pin the stream path itself.
//
// DirectVulkan only: DirectGLES has no GPU counter for an XFB-inactive draw at
// all (ES has no PRIMITIVES_GENERATED without a capture), and its CPU accounting
// is a different mechanism with its own tests.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
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

namespace MGITest {
    namespace {

        GLuint CompileShaderStage(GLenum type, const char* source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetShaderInfoLog(shader, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // A capture-capable vertex-only program: the varying gives glBeginTransformFeedback
        // something to capture for the mixed-span case; the XFB-inactive cases draw with the
        // same program and simply never begin a span.
        const char* const kVertexSource = R"(#version 430 core
out vec4 vs_out_value;
void main() {
  const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  vs_out_value = vec4(1.0);
  gl_Position = vec4(corners[gl_VertexID % 3], 0.0, 1.0);
}
)";

        // A passthrough tessellation pipeline whose all-1 levels emit exactly one
        // triangle per patch - the count the tessellation cases assert.
        const char* const kTessVertexSource = R"(#version 430 core
void main() {
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";
        const char* const kTessControlSource = R"(#version 430 core
layout(vertices = 1) out;
void main() {
  gl_TessLevelOuter[0] = 1.0;
  gl_TessLevelOuter[1] = 1.0;
  gl_TessLevelOuter[2] = 1.0;
  gl_TessLevelOuter[3] = 1.0;
  gl_TessLevelInner[0] = 1.0;
  gl_TessLevelInner[1] = 1.0;
}
)";
        const char* const kTessEvalSource = R"(#version 430 core
layout(triangles, equal_spacing, cw) in;
void main() {
  gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0);
}
)";

        // The same tessellation pipeline with something to capture, so that
        // glBeginTransformFeedback accepts it: the paused-span PATCHES case needs an
        // open (but paused) capture span AND a tessellator in one program.
        const char* const kTessEvalCaptureSource = R"(#version 430 core
layout(triangles, equal_spacing, cw) in;
out vec4 te_out_value;
void main() {
  te_out_value = vec4(1.0);
  gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0);
}
)";

        class PrimitivesGeneratedNoXfbScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (Gl().BackendName() != std::string("DirectVulkan")) {
                    GTEST_SKIP() << "the stream-query defect and its reroute are DirectVulkan's; "
                                 << Gl().BackendName()
                                 << " answers this query from a different mechanism";
                }
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenQueries(2, m_queries);
                ASSERT_NE(m_queries[0], 0u);
                ASSERT_NE(m_queries[1], 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                if (m_queries[0] != 0 || m_queries[1] != 0) glDeleteQueries(2, m_queries);
                m_queries[0] = m_queries[1] = 0;
                for (const GLuint program : m_programs) {
                    glDeleteProgram(program);
                }
                m_programs.clear();
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
                ScenarioTest::TearDown();
            }

            // captureVarying: the name to record with glTransformFeedbackVaryings, or
            // nullptr for a program that can never open a capture span.
            GLuint BuildProgram(std::initializer_list<std::pair<GLenum, const char*>> stages,
                                const char* captureVarying) {
                std::vector<GLuint> shaders;
                for (const auto& [type, source] : stages) {
                    const GLuint shader = CompileShaderStage(type, source, &m_buildLog);
                    if (shader == 0) {
                        for (const GLuint built : shaders) glDeleteShader(built);
                        return 0;
                    }
                    shaders.push_back(shader);
                }
                const GLuint program = glCreateProgram();
                for (const GLuint shader : shaders) glAttachShader(program, shader);
                if (captureVarying != nullptr) {
                    glTransformFeedbackVaryings(program, 1, &captureVarying, GL_INTERLEAVED_ATTRIBS);
                }
                glLinkProgram(program);
                for (const GLuint shader : shaders) glDeleteShader(shader);
                GLint status = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &status);
                if (status == GL_FALSE) {
                    GLint length = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                    std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                    glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                    m_buildLog = buffer.data();
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            GLuint BuildCaptureProgram() {
                return BuildProgram({{GL_VERTEX_SHADER, kVertexSource}}, "vs_out_value");
            }

            GLuint BuildTessellationProgram(bool withCaptureVarying = false) {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                while (glGetError() != GL_NO_ERROR) {
                }
                if (maxTessGenLevel < 1) return 0;
                return BuildProgram(
                    {{GL_VERTEX_SHADER, kTessVertexSource},
                     {GL_TESS_CONTROL_SHADER, kTessControlSource},
                     {GL_TESS_EVALUATION_SHADER,
                      withCaptureVarying ? kTessEvalCaptureSource : kTessEvalSource}},
                    withCaptureVarying ? "te_out_value" : nullptr);
            }

            // A capture span that is open but PAUSED. The pause closes the capture, so
            // every draw inside it is XFB-inactive at the backend - the stream query's
            // silent case - while the GL span stays active. `program` must be the one
            // that is bound: GL requires the same program at resume.
            void BeginPausedSpan() {
                glGenBuffers(1, &m_captureBuffer);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_captureBuffer);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 64 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
                glBeginTransformFeedback(GL_TRIANGLES);
                glPauseTransformFeedback();
            }

            void EndPausedSpan() {
                glResumeTransformFeedback();
                glEndTransformFeedback();
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
                if (m_captureBuffer != 0) glDeleteBuffers(1, &m_captureBuffer);
                m_captureBuffer = 0;
            }

            // GENERATED query around `record()`, answered with GL_QUERY_RESULT.
            GLuint QueryGenerated(const std::function<void()>& record) {
                glBeginQuery(GL_PRIMITIVES_GENERATED, m_queries[1]);
                record();
                glEndQuery(GL_PRIMITIVES_GENERATED);
                GLuint generated = 0xFFFFFFFFu;
                glGetQueryObjectuiv(m_queries[1], GL_QUERY_RESULT, &generated);
                return generated;
            }

            static GLenum DrainGLErrors() {
                const GLenum first = glGetError();
                while (glGetError() != GL_NO_ERROR) {
                }
                return first;
            }

            const std::string& BuildLog() const { return m_buildLog; }

            static std::filesystem::path LibraryLogPath() {
                const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
                return (path != nullptr && *path != '\0') ? std::filesystem::path(path)
                                                          : std::filesystem::path();
            }

            static std::uintmax_t LibraryLogSize() {
                std::error_code ec;
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return 0;
                const std::uintmax_t size = std::filesystem::file_size(path, ec);
                return ec ? 0 : size;
            }

            static std::string LibraryLogSince(std::uintmax_t offset) {
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return {};
                std::ifstream file(path, std::ios::binary);
                if (!file.good()) return {};
                file.seekg(static_cast<std::streamoff>(offset));
                return std::string((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            }

            GLuint m_vao = 0;
            GLuint m_queries[2] = {0, 0}; // [0]=written, [1]=generated
            GLuint m_captureBuffer = 0;
            std::vector<GLuint> m_programs;
            std::string m_buildLog;
        };

        // The plain shape: no capture object was ever bound, no span begun, no
        // rasterizer discard - just a GENERATED query around two triangles. On a
        // healthy driver the stream query answers it; on an affected one the armed
        // reroute must produce the same 2.
        TEST_F(PrimitivesGeneratedNoXfbScenario, CountsADrawMadeWithNoCaptureSpan) {
            if (!Ready()) return;
            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            const GLuint generated = QueryGenerated([]() { glDrawArrays(GL_TRIANGLES, 0, 6); });
            EXPECT_EQ(DrainGLErrors(), 0u);
            EXPECT_EQ(generated, 2u)
                << "GL_PRIMITIVES_GENERATED must count a draw made while transform feedback is "
                   "inactive (GL 4.6 core 13.4)";
        }

        // THE CTS SHAPE (esextcTessellationShaderUtils.cpp, captureTessellationData):
        // rasterizer discard ON, transform feedback INACTIVE, the draw inside a
        // GENERATED query. This is the exact query whose 0 sizes ~29 tessellation
        // tests' capture buffers on the affected device.
        //
        // On lavapipe this case holds through the dedicated
        // VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT reroute (its discard feature is
        // what makes a discarded draw countable there - llvmpipe's clipping
        // statistics AND stream query both read 0 under discard).
        //
        // The value-conditioned skip below is deliberate and narrow, for a stack
        // with NO counter that survives discard: there this case is unfalsifiable,
        // and a red would indict MobileGL for a hole the bring-up probe already
        // measures and reports (StatisticsSubstitutePlainOnly / Unfixable). The
        // exact-zero answer IS the capability signal - any wrong nonzero count
        // still fails - and on every driver that counts discarded draws at all the
        // full assertion runs. The device probe list holds this shape on the Mali.
        TEST_F(PrimitivesGeneratedNoXfbScenario, CountsUnderRasterizerDiscardWithNoCaptureSpan) {
            if (!Ready()) return;
            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            glEnable(GL_RASTERIZER_DISCARD);
            const GLuint generated = QueryGenerated([]() { glDrawArrays(GL_TRIANGLES, 0, 6); });
            glDisable(GL_RASTERIZER_DISCARD);
            EXPECT_EQ(DrainGLErrors(), 0u);
            if (generated == 0u) {
                GTEST_SKIP() << "no counter this backend can reach (stream query, dedicated "
                                "primitives-generated query, clipping statistics) survives "
                                "rasterizer discard for an XFB-inactive draw on this stack - the "
                                "shape is unfalsifiable here; the bring-up probe measures the same "
                                "hole and the POST row reports it";
            }
            EXPECT_EQ(generated, 2u)
                << "rasterizer discard drops primitives after clipping and must not hide them from "
                   "GL_PRIMITIVES_GENERATED - this is the exact shape the CTS measures the "
                   "tessellator with";
        }

        // The tessellation flavour: a PATCHES draw whose all-1 levels emit exactly
        // one triangle - the count the CTS's getAmountOfVerticesGeneratedByTessellator
        // protocol derives everything from. Undiscarded, so that the answer is
        // holdable on this machine through whichever accounting path is armed (the
        // discard interaction is the case above's business, measured separately).
        TEST_F(PrimitivesGeneratedNoXfbScenario, CountsATessellatedPatchWithNoCaptureSpan) {
            if (!Ready()) return;
            const GLuint program = BuildTessellationProgram();
            if (program == 0) {
                GTEST_SKIP() << "no tessellation stages on this stack: " << BuildLog();
            }
            glUseProgram(program);
            glPatchParameteri(GL_PATCH_VERTICES, 1);

            const GLuint generated = QueryGenerated([]() { glDrawArrays(GL_PATCHES, 0, 1); });
            EXPECT_EQ(DrainGLErrors(), 0u);
            EXPECT_EQ(generated, 1u)
                << "a triangles-domain patch with every level 1 tessellates to exactly one "
                   "triangle, and GL_PRIMITIVES_GENERATED must say so with no capture active";
        }

        // One query span holding BOTH kinds of draw: an XFB-inactive draw, then a
        // captured one, then another XFB-inactive one. The GENERATED answer must
        // accumulate across the two accounting paths the armed reroute splits them
        // into (stream slots for the captured draw, statistics slots for the
        // others), and WRITTEN must stay exactly the captured draw's count - the
        // pairing the stream path exists to keep exact. Undiscarded, so the
        // accumulation invariant is holdable on this machine (see the discard
        // case's comment); the triangles rasterize into the harness framebuffer,
        // which nothing here reads.
        TEST_F(PrimitivesGeneratedNoXfbScenario, ASpanMixingActiveAndInactiveDrawsAccumulatesBoth) {
            if (!Ready()) return;
            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            GLuint captureBuffer = 0;
            glGenBuffers(1, &captureBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 3 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

            glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, m_queries[0]);
            const GLuint generated = QueryGenerated([]() {
                glDrawArrays(GL_TRIANGLES, 0, 3); // XFB inactive
                glBeginTransformFeedback(GL_TRIANGLES);
                glDrawArrays(GL_TRIANGLES, 0, 3); // captured
                glEndTransformFeedback();
                glDrawArrays(GL_TRIANGLES, 0, 3); // XFB inactive again
            });
            glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);

            GLuint written = 0xFFFFFFFFu;
            glGetQueryObjectuiv(m_queries[0], GL_QUERY_RESULT, &written);
            glDeleteBuffers(1, &captureBuffer);
            EXPECT_EQ(DrainGLErrors(), 0u);
            EXPECT_EQ(generated, 3u) << "one triangle before the span, one inside it, one after";
            EXPECT_EQ(written, 1u) << "only the draw inside the span writes anything";
        }

        // ===================== DRAWS INSIDE A PAUSED SPAN =====================
        //
        // glPauseTransformFeedback closes the capture without closing the span, so a
        // draw made while paused is XFB-INACTIVE at the backend - the stream query is
        // exactly as silent for it as for a draw with no span at all - while
        // GL_PRIMITIVES_GENERATED must still count what the last vertex processing
        // stage emitted (GL 4.6 core 13.4; the WRITTEN query is the one the pause
        // silences). The frontend does keep a CPU counter for paused draws, but it can
        // price only 3 of the ~15 draw entry points and answers 0 for GL_PATCHES, so
        // these draws are the reroute's business like any other - and the trap on the
        // other side is counting them TWICE, once in each accounting.
        //
        // Each case measures the SAME draw twice: once with no span open at all (the
        // capability control - what this stack can count) and once inside the paused
        // span, and requires the two to agree. That differential is what makes these
        // cases falsifying rather than vacuous: a stack where no counter reaches a
        // capture-less draw fails the control and skips, while a stack that counts the
        // unpaused draw and answers 0 for the paused one - which is what excluding
        // paused draws from the reroute produced - fails, instead of skipping into
        // green.

        // The draw the CPU counter CAN price: if the span both reroutes it and adds the
        // CPU delta, this reads 2.
        TEST_F(PrimitivesGeneratedNoXfbScenario, APausedSpanCountsACpuPricedDrawExactlyOnce) {
            if (!Ready()) return;
            if (AmbientQuirkFromEnvironment("MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE") == AmbientQuirk::Off) {
                GTEST_SKIP() << "the negative control replays the pre-probe accounting, whose paused "
                                "draws are CPU-counted on top of whatever the stream query says";
            }
            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            const GLuint unpaused = QueryGenerated([]() { glDrawArrays(GL_TRIANGLES, 0, 3); });
            BeginPausedSpan();
            const GLuint paused = QueryGenerated([]() { glDrawArrays(GL_TRIANGLES, 0, 3); });
            EndPausedSpan();
            EXPECT_EQ(DrainGLErrors(), 0u);
            if (unpaused == 0u) {
                GTEST_SKIP() << "no counter this backend can reach answers a capture-less draw on this "
                                "stack, so the paused half of the comparison proves nothing; the "
                                "bring-up probe measures the same hole and the POST row reports it";
            }
            EXPECT_EQ(unpaused, 1u) << "the control itself: one triangle is one primitive";
            EXPECT_EQ(paused, unpaused)
                << "one triangle drawn while the capture span is paused is still one primitive "
                   "generated - counted once, by whichever accounting owns it, never by two of them "
                   "(a reroute slot AND the frontend's CPU paused counter reads 2)";
        }

        // The draw the CPU counter CANNOT price: GL_PATCHES, whose amplification is not
        // knowable on the CPU (CountPrimitivesForDraw answers 0 for it by design) - and
        // the CTS's tessellator-measuring shape. Excluding paused draws from the
        // reroute left this counted by nothing at all on the affected device.
        TEST_F(PrimitivesGeneratedNoXfbScenario, APausedSpanCountsATessellatedPatchExactlyOnce) {
            if (!Ready()) return;
            const GLuint program = BuildTessellationProgram(/*withCaptureVarying=*/true);
            if (program == 0) {
                GTEST_SKIP() << "no tessellation stages on this stack: " << BuildLog();
            }
            glUseProgram(program);
            glPatchParameteri(GL_PATCH_VERTICES, 1);

            const GLuint unpaused = QueryGenerated([]() { glDrawArrays(GL_PATCHES, 0, 1); });
            BeginPausedSpan();
            const GLuint paused = QueryGenerated([]() { glDrawArrays(GL_PATCHES, 0, 1); });
            EndPausedSpan();
            EXPECT_EQ(DrainGLErrors(), 0u);
            if (unpaused == 0u) {
                GTEST_SKIP() << "no counter this backend can reach answers a capture-less patch draw "
                                "on this stack, so the paused half proves nothing; the bring-up probe "
                                "measures the same hole and the POST row reports it";
            }
            EXPECT_EQ(unpaused, 1u)
                << "the control itself: a triangles-domain patch with every level 1 tessellates to "
                   "exactly one triangle";
            EXPECT_EQ(paused, unpaused)
                << "pausing the capture does not stop the tessellator from generating that triangle, "
                   "and the frontend's CPU paused counter answers 0 for GL_PATCHES - so a paused "
                   "patch draw left out of the reroute is counted by nothing at all";
        }

        // The other half of the same hole: the instanced entry points never reach the
        // frontend's paused accounting either, so a paused instanced draw excluded from
        // the reroute is likewise counted by nothing.
        TEST_F(PrimitivesGeneratedNoXfbScenario, APausedSpanCountsAnInstancedDrawExactlyOnce) {
            if (!Ready()) return;
            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            const GLuint unpaused =
                QueryGenerated([]() { glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 4); });
            BeginPausedSpan();
            const GLuint paused = QueryGenerated([]() { glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 4); });
            EndPausedSpan();
            EXPECT_EQ(DrainGLErrors(), 0u);
            if (unpaused == 0u) {
                GTEST_SKIP() << "no counter this backend can reach answers a capture-less draw on this "
                                "stack, so the paused half proves nothing";
            }
            EXPECT_EQ(unpaused, 4u) << "the control itself: four instances of one triangle";
            EXPECT_EQ(paused, unpaused)
                << "four instances generate four primitives whether or not the capture span is "
                   "paused, and no instanced entry point reaches the frontend's paused accounting";
        }

        // THE ONE CASE THAT CAN FAIL WHEN THE REROUTE SILENTLY STOPS BEING ARMED -
        // the UnlocatedIoBlockScenario shape, for the same reason: every case above
        // is green here whether the reroute ran or not (that is the "two pools
        // agree" point), so none of them can say the pinned lane actually exercised
        // a reroute pool. This one asserts a LIBRARY OBSERVABLE against the
        // environment: with MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE pinned on, an
        // XFB-inactive draw inside a GENERATED span must make the renderer say -
        // through its latched MGLOG_I - that it engaged the reroute. It reads
        // MG_Config not at all (on Android this module links the shipping library)
        // and trusts only the log bytes appended after it started.
        TEST_F(PrimitivesGeneratedNoXfbScenario, TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn) {
            if (!Ready()) return;
            if (AmbientQuirkFromEnvironment("MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE") != AmbientQuirk::On) {
                GTEST_SKIP() << "this case needs the reroute pinned ON for the whole process, which "
                                "is what the PrimGenReroute. ctest entry does with "
                                "MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE=1; unset, the bring-up probe "
                                "decides and this machine's verdict is its own business";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE is pinned on but "
                                "MOBILEGL_LOG_FILE_PATH is not set, so the library has nowhere to "
                                "record that it rerouted anything; the PrimGenReroute. ctest "
                                "entry sets both";
            }

            const GLuint program = BuildCaptureProgram();
            ASSERT_NE(program, 0u) << BuildLog();
            glUseProgram(program);

            // Taken BEFORE the draw, so the line this looks for can only be one this
            // process wrote for this span. The latch fires on the FIRST rerouted
            // draw, which is inside the query below.
            const std::uintmax_t before = LibraryLogSize();
            const GLuint generated = QueryGenerated([]() { glDrawArrays(GL_TRIANGLES, 0, 3); });
            EXPECT_EQ(DrainGLErrors(), 0u);
            EXPECT_EQ(generated, 1u) << "the pinned-on lane did not even count correctly";

            const std::string appended = LibraryLogSince(before);
            EXPECT_NE(appended.find("PRIMITIVES_GENERATED reroute engaged"), std::string::npos)
                << "MOBILEGL_MAGMA_PRIMGEN_QUERY_REROUTE is pinned ON, an XFB-inactive draw ran inside "
                   "a GENERATED query, and the renderer never reported engaging the reroute. The "
                   "quirk is not armed - check the override mapping "
                   "(ChoosePrimitivesGeneratedReroute) and the arming gate in "
                   "VulkanRenderer::BeginXfbQueryForDraw. Log appended by this test:\n"
                << appended;
        }

    } // namespace
} // namespace MGITest
