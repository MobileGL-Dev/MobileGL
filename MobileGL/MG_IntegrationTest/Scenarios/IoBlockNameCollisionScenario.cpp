// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/IoBlockNameCollisionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - ONE BLOCK NAME USED IN BOTH DIRECTIONS BY ONE STAGE STILL CARRIES ITS PAYLOAD.
//
// Desktop GLSL keeps SEPARATE name namespaces for input and output interface blocks, so a
// single stage may legally write
//
//     in  TcsData { ... } tes_in[];
//     out TcsData { ... } tes_out;
//
// The tessellation evaluation stage of both interface-block tests in
// KHR-GL42/43.shading_language_420pack does exactly that, and MobileGL's backend used to
// hand the shape straight through: SPIRV-Cross splits the namespace the same way glslang
// does (block_input_names vs block_output_names) and re-emits BOTH blocks under the name
// TcsData, so the generated ESSL declares two different blocks of one name in one shader.
// Adreno's ES compiler keeps them apart. Mali's does not - the stage compiles, the program
// links, and the evaluation stage's writes never reach the geometry stage, which is all 22
// of that group's Mali failures and none of Adreno's or DirectVulkan's.
//
// Both cases below drive the SAME five-stage pipeline (vertex -> tessellation control ->
// tessellation evaluation -> geometry -> fragment) and differ only in whether the
// evaluation stage reuses one name. The distinct-name case is the negative control: it is
// what says a red pixel in the colliding case is about the name and not about this machine's
// tessellation, its geometry stage, or the block mechanism in general.
//
// Colour code, so a failure names its own cause:
//   green  - the payload crossed all four stage boundaries, which is the pass.
//   blue   - the clear colour: nothing was drawn at all (the program did not link, or the
//            backend program was rejected and every draw became a no-op).
//   red    - the pipeline ran but the plain (non-block) varying did not arrive, i.e. the
//            failure is not about interface blocks.
//   black  - the pipeline ran, the plain varying arrived, and the BLOCK payload came back
//            zeroed or garbage. That is the defect this scenario exists for.
//
// llvmpipe and lavapipe run this faithfully but do NOT reproduce the original defect - the
// aliasing is a Mali ES compiler behaviour. Read a green run here as "the rename did not
// break the ordinary path"; the claim it pins on the device is the CTS group above.

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

namespace MGITest {
    namespace {

        // The payload starts here and is copied, unmodified, through every block below.
        const char* const kVertexSource = R"(#version 420 core
out VsData {
    vec4 payload;
} vs_out;
void main()
{
    vs_out.payload = vec4(0.0, 1.0, 0.0, 1.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kTessControlSource = R"(#version 420 core
layout(vertices = 1) out;
in VsData {
    vec4 payload;
} tcs_in[];
out TcsData {
    vec4 payload;
} tcs_out[];
void main()
{
    tcs_out[gl_InvocationID].payload = tcs_in[gl_InvocationID].payload;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelOuter[3] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_TessLevelInner[1] = 1.0;
}
)";

        // THE CASE UNDER TEST: one name, both directions, in one stage.
        const char* const kCollidingTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;
in TcsData {
    vec4 payload;
} tes_in[];
out TcsData {
    vec4 payload;
} tes_out;
out float tes_gs_alive;
void main()
{
    tes_out.payload = tes_in[0].payload;
    tes_gs_alive = 1.0;
}
)";

        // The negative control: byte-identical but for the output block's name.
        const char* const kDistinctTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;
in TcsData {
    vec4 payload;
} tes_in[];
out TesData {
    vec4 payload;
} tes_out;
out float tes_gs_alive;
void main()
{
    tes_out.payload = tes_in[0].payload;
    tes_gs_alive = 1.0;
}
)";

        // One geometry source per evaluation stage, because the block it consumes is named
        // after the block the evaluation stage produced.
        const char* const kCollidingGeometrySource = R"(#version 420 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in TcsData {
    vec4 payload;
} gs_in[];
in float tes_gs_alive[];
out GsData {
    vec4 payload;
} gs_out;
out float gs_fs_alive;
void EmitCorner(vec2 corner)
{
    gs_out.payload = gs_in[0].payload;
    gs_fs_alive = tes_gs_alive[0];
    gl_Position = vec4(corner, 0.0, 1.0);
    EmitVertex();
}
void main()
{
    EmitCorner(vec2(-1.0, -1.0));
    EmitCorner(vec2(-1.0,  1.0));
    EmitCorner(vec2( 1.0, -1.0));
    EmitCorner(vec2( 1.0,  1.0));
}
)";

        const char* const kDistinctGeometrySource = R"(#version 420 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in TesData {
    vec4 payload;
} gs_in[];
in float tes_gs_alive[];
out GsData {
    vec4 payload;
} gs_out;
out float gs_fs_alive;
void EmitCorner(vec2 corner)
{
    gs_out.payload = gs_in[0].payload;
    gs_fs_alive = tes_gs_alive[0];
    gl_Position = vec4(corner, 0.0, 1.0);
    EmitVertex();
}
void main()
{
    EmitCorner(vec2(-1.0, -1.0));
    EmitCorner(vec2(-1.0,  1.0));
    EmitCorner(vec2( 1.0, -1.0));
    EmitCorner(vec2( 1.0,  1.0));
}
)";

        // Red when the PLAIN varying did not arrive, so "the pipeline is broken" and "the
        // block payload is broken" cannot be confused for one another.
        const char* const kFragmentSource = R"(#version 420 core
in GsData {
    vec4 payload;
} fs_in;
in float gs_fs_alive;
out vec4 fragColor;
void main()
{
    fragColor = gs_fs_alive > 0.5 ? fs_in.payload : vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        class IoBlockNameCollisionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                if (!BackendHostsTessellationAndGeometry()) {
                    GTEST_SKIP() << "no tessellation/geometry stages on " << Gl().BackendName() << " ("
                                 << Gl().RendererString() << "); there is no five-stage pipeline to "
                                 << "carry a block through";
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (const GLuint program : m_programs) {
                    glDeleteProgram(program);
                }
                m_programs.clear();
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
            }

            // GL_MAX_TESS_GEN_LEVEL is a real backend answer, not a frontend constant: it
            // reads 0 on a DirectGLES driver without GL_EXT_tessellation_shader and on a
            // DirectVulkan device without the tessellationShader feature. There is no
            // five-stage pipeline to assert about on such a stack.
            static bool BackendHostsTessellationAndGeometry() {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxTessGenLevel >= 1 && maxGeometryOutputVertices >= 4;
            }

            GLuint BuildPipeline(const char* tessEvalSource, const char* geometrySource) {
                const GLenum stages[] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER,
                                         GL_TESS_EVALUATION_SHADER, GL_GEOMETRY_SHADER,
                                         GL_FRAGMENT_SHADER};
                const char* const sources[] = {kVertexSource, kTessControlSource, tessEvalSource,
                                               geometrySource, kFragmentSource};

                GLuint shaders[5] = {0, 0, 0, 0, 0};
                bool ok = true;
                for (int i = 0; i < 5; ++i) {
                    shaders[i] = glCreateShader(stages[i]);
                    glShaderSource(shaders[i], 1, &sources[i], nullptr);
                    glCompileShader(shaders[i]);
                    GLint compiled = 0;
                    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
                    if (!compiled) {
                        m_buildLog = InfoLog(shaders[i], true);
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    for (const GLuint shader : shaders) {
                        if (shader != 0) glDeleteShader(shader);
                    }
                    return 0;
                }

                const GLuint program = glCreateProgram();
                for (const GLuint shader : shaders) {
                    glAttachShader(program, shader);
                }
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                for (const GLuint shader : shaders) {
                    glDeleteShader(shader);
                }
                if (!linked) {
                    m_buildLog = InfoLog(program, false);
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            // Clears to BLUE, so "the draw painted nothing" is a colour of its own rather
            // than something that could be mistaken for a zeroed payload.
            Rgba8 DrawAndReadCentre(GLuint program) const {
                glViewport(0, 0, Gl().Width(), Gl().Height());
                glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program);
                glPatchParameteri(GL_PATCH_VERTICES, 1);
                glDrawArrays(GL_PATCHES, 0, 1);

                Rgba8 pixel{};
                glReadPixels(Gl().Width() / 2, Gl().Height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
                return pixel;
            }

            static bool IsGreen(const Rgba8& pixel) {
                return pixel.r < 64 && pixel.g > 192 && pixel.b < 64;
            }

            const std::string& BuildLog() const { return m_buildLog; }

            static GLenum FirstGLError() {
                const GLenum first = glGetError();
                while (glGetError() != GL_NO_ERROR) {
                }
                return first;
            }

        private:
            static std::string InfoLog(GLuint object, bool isShader) {
                GLint length = 0;
                if (isShader) {
                    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
                } else {
                    glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
                }
                std::vector<char> log(static_cast<std::size_t>(length > 1 ? length : 1), '\0');
                if (isShader) {
                    glGetShaderInfoLog(object, static_cast<GLsizei>(log.size()), nullptr, log.data());
                } else {
                    glGetProgramInfoLog(object, static_cast<GLsizei>(log.size()), nullptr, log.data());
                }
                return std::string(log.data());
            }

            GLuint m_vao = 0;
            std::vector<GLuint> m_programs;
            std::string m_buildLog;
        };

        // The negative control, and it runs first on purpose: if this one is not green there
        // is nothing to conclude from the case below it.
        //
        // It is also the CALIBRATION. GL_MAX_TESS_GEN_LEVEL answers for the tessellation
        // stages honestly, but nothing MobileGL reports answers for the geometry stage the
        // same way (GL_MAX_GEOMETRY_* are frontend constants and an ES driver may legitimately
        // report zero geometry storage blocks while having geometry shaders), so a stack that
        // cannot build a five-stage program at all is recognised here, by trying.
        TEST_F(IoBlockNameCollisionScenario, DistinctlyNamedBlocksCarryThePayloadThroughFiveStages) {
            if (!Ready()) return;

            const GLuint program = BuildPipeline(kDistinctTessEvalSource, kDistinctGeometrySource);
            if (program == 0) {
                GTEST_SKIP() << "this stack cannot build a five-stage tessellation+geometry program on "
                             << Gl().BackendName() << ", so there is no block to carry through: "
                             << BuildLog();
            }

            const Rgba8 centre = DrawAndReadCentre(program);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_TRUE(IsGreen(centre)) << "the control pipeline did not deliver its payload: " << centre;
        }

        TEST_F(IoBlockNameCollisionScenario, OneBlockNameInBothDirectionsStillCarriesThePayload) {
            if (!Ready()) return;

            // Same calibration as the case above, and for the same reason: a five-stage program
            // this stack cannot build at all is not evidence about block names. Only once the
            // DISTINCT-name build succeeds does a failure of the colliding one mean something.
            if (BuildPipeline(kDistinctTessEvalSource, kDistinctGeometrySource) == 0) {
                GTEST_SKIP() << "this stack cannot build a five-stage tessellation+geometry program on "
                             << Gl().BackendName() << ", so there is no block to carry through: "
                             << BuildLog();
            }

            // Legal desktop GLSL: input and output block names live in separate namespaces, so
            // the evaluation stage below declares TcsData twice and must still compile. The
            // control above having built is what makes this assertion about the NAME.
            const GLuint program = BuildPipeline(kCollidingTessEvalSource, kCollidingGeometrySource);
            ASSERT_NE(program, 0u)
                << "an interface block name reused across the two directions of one stage is legal "
                   "desktop GLSL, but the program did not build: "
                << BuildLog();

            const Rgba8 centre = DrawAndReadCentre(program);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_TRUE(IsGreen(centre))
                << "the payload did not survive the stage that names its input and output block "
                   "the same: "
                << centre << " (blue: nothing drew; red: the plain varying was lost too; black: "
                             "the block arrived empty)";
        }

    } // namespace
} // namespace MGITest
