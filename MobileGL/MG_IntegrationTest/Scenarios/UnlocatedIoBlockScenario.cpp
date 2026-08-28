// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UnlocatedIoBlockScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN INTER-STAGE INTERFACE BLOCK STILL FINDS ITS OTHER END WITH ITS LOCATION
// QUALIFIER REMOVED.
//
// The Mali-G1-Ultra ES driver delivers NOTHING through an interface block that carries an
// explicit layout(location=) once a tessellation or geometry stage is in the pipeline: the
// stages compile, the program links with an empty info log, the draw runs, and the consuming
// stage reads zeroes. Measured with no MobileGL in the process - a bare EGL/GLES 3.2 program
// built from the five ESSL stages MobileGL emits reproduces it, and removing the qualifier
// from the blocks (and changing nothing else) makes the same program carry its payload. The
// locations are not the application's in the first place: these shaders declare none, and
// glslang's cross-stage IO resolver invents them.
//
// DirectGLES answers by dropping the decoration for those programs (StripIoBlockLocationsPass),
// leaving ES to match the blocks by block name and member sequence. THAT is what this scenario
// guards: with the strip forced on, a five-stage pipeline whose four block boundaries carry no
// location must still deliver its payload end to end. It is the assertion the affected device
// cannot make about itself in CI, and the one the healthy machines here CAN make - which is
// the opposite of IoBlockNameCollisionScenario's position, where the machines that run it
// cannot reproduce the defect at all.
//
// The strip is armed for this suite by MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1 on the ctest
// entry, because llvmpipe carries a located block correctly and the driver POST would
// therefore never turn the emulation on here. The SAME cases also run under the ambient
// registrations with the emulation off, so both spellings of the interface are covered and a
// regression in either shows up.
//
// Colour code, so a failure names its own cause:
//   green  - the payload crossed all four stage boundaries, which is the pass.
//   blue   - the clear colour: nothing was drawn at all (the program did not link, or the
//            backend program was rejected and every draw became a no-op).
//   red    - the pipeline ran but the plain (non-block) varying did not arrive, i.e. the
//            failure is not about interface blocks.
//   black  - the pipeline ran, the plain varying arrived, and the BLOCK payload came back
//            zeroed. That is what an interface whose two ends stopped matching looks like.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

        // NOTHING in these five stages declares a location. Every location the emitted ESSL
        // carries is invented by the cross-stage resolver, which is exactly the shape the
        // affected driver mishandles and exactly what the strip removes.
        //
        // Two members per block, of different types, because an interface that is matched by
        // name and member sequence rather than by location has to agree on the sequence too -
        // a repair that silently reordered or dropped a member would still light up green with
        // one member in the block.
        const char* const kVertexSource = R"(#version 420 core
out VsData {
    vec4 payload;
    vec2 tint;
} vs_out;
out float vs_tcs_alive;
void main()
{
    vs_out.payload = vec4(0.0, 1.0, 0.0, 1.0);
    vs_out.tint = vec2(0.25, 0.5);
    vs_tcs_alive = 1.0;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kTessControlSource = R"(#version 420 core
layout(vertices = 1) out;
in VsData {
    vec4 payload;
    vec2 tint;
} tcs_in[];
in float vs_tcs_alive[];
out TcsData {
    vec4 payload;
    vec2 tint;
} tcs_out[];
out float tcs_tes_alive[];
void main()
{
    tcs_out[gl_InvocationID].payload = tcs_in[gl_InvocationID].payload;
    tcs_out[gl_InvocationID].tint = tcs_in[gl_InvocationID].tint;
    tcs_tes_alive[gl_InvocationID] = vs_tcs_alive[gl_InvocationID];
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelOuter[3] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_TessLevelInner[1] = 1.0;
}
)";

        // Distinct block names, so this case is about the LOCATION and nothing else; the
        // one-name-in-both-directions shape is the case below.
        const char* const kDistinctTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;
in TcsData {
    vec4 payload;
    vec2 tint;
} tes_in[];
in float tcs_tes_alive[];
out TesData {
    vec4 payload;
    vec2 tint;
} tes_out;
out float tes_gs_alive;
void main()
{
    tes_out.payload = tes_in[0].payload;
    tes_out.tint = tes_in[0].tint;
    tes_gs_alive = tcs_tes_alive[0];
}
)";

        // The 420pack shape: ONE name for the block this stage consumes and the block it
        // produces. Legal desktop GLSL, and the case where the two repairs have to compose -
        // the rename gives the two blocks one spelling per producing stage, the strip takes
        // their locations off, and the interfaces still have to meet.
        const char* const kCollidingTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;
in TcsData {
    vec4 payload;
    vec2 tint;
} tes_in[];
in float tcs_tes_alive[];
out TcsData {
    vec4 payload;
    vec2 tint;
} tes_out;
out float tes_gs_alive;
void main()
{
    tes_out.payload = tes_in[0].payload;
    tes_out.tint = tes_in[0].tint;
    tes_gs_alive = tcs_tes_alive[0];
}
)";

        // One geometry source per evaluation stage, because the block it consumes is named
        // after the block the evaluation stage produced.
        const char* const kDistinctGeometrySource = R"(#version 420 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in TesData {
    vec4 payload;
    vec2 tint;
} gs_in[];
in float tes_gs_alive[];
out GsData {
    vec4 payload;
    vec2 tint;
} gs_out;
out float gs_fs_alive;
void EmitCorner(vec2 corner)
{
    gs_out.payload = gs_in[0].payload;
    gs_out.tint = gs_in[0].tint;
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

        const char* const kCollidingGeometrySource = R"(#version 420 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in TcsData {
    vec4 payload;
    vec2 tint;
} gs_in[];
in float tes_gs_alive[];
out GsData {
    vec4 payload;
    vec2 tint;
} gs_out;
out float gs_fs_alive;
void EmitCorner(vec2 corner)
{
    gs_out.payload = gs_in[0].payload;
    gs_out.tint = gs_in[0].tint;
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

        // Green ONLY when both block members arrived: a repair that kept the first member and
        // lost the second would otherwise pass. Red when the plain varying is missing too, so
        // "the pipeline is broken" and "the block is broken" cannot be confused.
        const char* const kFragmentSource = R"(#version 420 core
in GsData {
    vec4 payload;
    vec2 tint;
} fs_in;
in float gs_fs_alive;
out vec4 fragColor;
void main()
{
    if (gs_fs_alive <= 0.5) {
        fragColor = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (abs(fs_in.tint.x - 0.25) > 0.01 || abs(fs_in.tint.y - 0.5) > 0.01) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        fragColor = fs_in.payload;
    }
}
)";

        class UnlocatedIoBlockScenario : public ScenarioTest {
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

            // Same calibration IoBlockNameCollisionScenario uses, and for the same reason:
            // GL_MAX_TESS_GEN_LEVEL is a real backend answer while GL_MAX_GEOMETRY_* are
            // frontend constants, so a stack with no five-stage pipeline is recognised by
            // trying to build one, not by asking.
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

            // The library log this process is writing, or an empty path when none was
            // configured. MOBILEGL_LOG_FILE_PATH is read at log-init, before anything this
            // fixture can reach, so the ctest entry sets it and this only reads it back.
            static std::filesystem::path LibraryLogPath() {
                const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
                return (path != nullptr && *path != '\0') ? std::filesystem::path(path)
                                                          : std::filesystem::path();
            }

            // How many bytes the library log already holds. Everything this fixture asserts on
            // is searched from here forward, because the file is APPENDED to by every process
            // in the lane and a line left behind by an earlier one would otherwise satisfy the
            // assertion without this process having done anything at all.
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

        TEST_F(UnlocatedIoBlockScenario, BlocksCarryTheirPayloadThroughFiveStages) {
            if (!Ready()) return;

            const GLuint program = BuildPipeline(kDistinctTessEvalSource, kDistinctGeometrySource);
            if (program == 0) {
                GTEST_SKIP() << "this stack cannot build a five-stage tessellation+geometry program on "
                             << Gl().BackendName() << ", so there is no block to carry through: "
                             << BuildLog();
            }

            const Rgba8 centre = DrawAndReadCentre(program);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_TRUE(IsGreen(centre))
                << "a four-boundary interface-block chain did not deliver its payload: " << centre
                << " (blue: nothing drew; red: the plain varying was lost too; black: a block "
                   "member arrived wrong, i.e. the interface stopped matching)";
        }

        // The two repairs together. The rename is what makes the evaluation stage's two
        // TcsData blocks one spelling per producing stage; the strip then takes the locations
        // off the names the rename just settled. Either one alone leaves a working program on
        // these machines, so this case is here to catch the two of them disagreeing.
        TEST_F(UnlocatedIoBlockScenario, BlocksNamedInBothDirectionsStillMeetWithoutLocations) {
            if (!Ready()) return;

            if (BuildPipeline(kDistinctTessEvalSource, kDistinctGeometrySource) == 0) {
                GTEST_SKIP() << "this stack cannot build a five-stage tessellation+geometry program on "
                             << Gl().BackendName() << ", so there is no block to carry through: "
                             << BuildLog();
            }

            const GLuint program = BuildPipeline(kCollidingTessEvalSource, kCollidingGeometrySource);
            ASSERT_NE(program, 0u)
                << "an interface block name reused across the two directions of one stage is legal "
                   "desktop GLSL, but the program did not build: "
                << BuildLog();

            const Rgba8 centre = DrawAndReadCentre(program);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_TRUE(IsGreen(centre))
                << "the renamed-and-unlocated interface chain lost its payload: " << centre;
        }

        // THE ONE CASE THAT CAN FAIL WHEN THE REPAIR SILENTLY STOPS BEING ARMED.
        //
        // Everything above renders green on llvmpipe whether the blocks were stripped or not -
        // this machine carries a located block correctly - so those cases pin that the strip
        // does no HARM and can say nothing about whether it happened. That leaves the arming
        // itself untested, and the arming is where the cheap mistake lives: Loader.cpp maps
        // MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS onto the capability INVERTED (forcing the
        // emulation on means declaring located blocks UNSUPPORTED), and a one-line swap of
        // those two arms would disable the device repair with every test here still green.
        //
        // So this case asserts a LIBRARY OBSERVABLE against the environment, the shape
        // AsyncCompileScenario::ExtensionStringMatchesTheConfiguration uses: the environment
        // says the emulation is pinned on, therefore the library must SAY it stripped
        // something. The observable is the latched MGLOG_I DirectGLES emits the first time the
        // pass fires (Managers.cpp); it is INFO rather than DEBUG precisely so that this
        // assertion is possible in the builds CI runs.
        //
        // Two things it deliberately does NOT do: it does not read MG_Config (on Android this
        // module links the shipping library, which exports nothing internal - the reason
        // ViewportArrayScenario's control moved to the environment), and it does not trust the
        // whole log file, only the bytes appended after this test started.
        TEST_F(UnlocatedIoBlockScenario, TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn) {
            if (!Ready()) return;

            if (AmbientQuirkFromEnvironment("MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS") != AmbientQuirk::On) {
                GTEST_SKIP() << "this case needs the emulation pinned ON for the whole process, which "
                                "is what the UnlocatedIoBlocks. ctest entry does with "
                                "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS=1; with the variable unset the "
                                "driver POST decides, and on this machine it decides the blocks are "
                                "fine - so there would be nothing to observe";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS is pinned on but "
                                "MOBILEGL_LOG_FILE_PATH is not set, so the library has nowhere to "
                                "record that it stripped anything; the UnlocatedIoBlocks. ctest entry "
                                "sets both";
            }
            if (Gl().BackendName() != std::string("DirectGLES")) {
                GTEST_SKIP() << "the strip is DirectGLES's; " << Gl().BackendName()
                             << " hands the module to the driver as SPIR-V, where Location is how "
                                "interfaces are matched";
            }

            // Taken BEFORE the program is built, so the line this looks for can only be one
            // this process wrote. The latch means it is emitted at the FIRST stage of the
            // FIRST affected program, which is inside the build below.
            const std::uintmax_t before = LibraryLogSize();

            const GLuint program = BuildPipeline(kDistinctTessEvalSource, kDistinctGeometrySource);
            if (program == 0) {
                GTEST_SKIP() << "this stack cannot build a five-stage tessellation+geometry program on "
                             << Gl().BackendName() << ", so nothing would arm the strip: " << BuildLog();
            }
            // Drawn as well as built, so a stack that defers its backend program to first use
            // still reaches the transpile this is asserting about.
            const Rgba8 centre = DrawAndReadCentre(program);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_TRUE(IsGreen(centre)) << "the pinned-on lane did not even render correctly: " << centre;

            const std::string appended = LibraryLogSince(before);
            EXPECT_NE(appended.find("WITHOUT their layout(location) qualifier"), std::string::npos)
                << "MOBILEGL_ESPRYT_UNLOCATED_IO_BLOCKS is pinned ON, a five-stage program with four "
                   "interface-block boundaries was built and drawn, and DirectGLES never reported "
                   "stripping a single location. The emulation is not armed - check the override "
                   "mapping in Loader.cpp (it is inverted on purpose) and the arming gate in "
                   "Managers.cpp. Log appended by this test:\n"
                << appended;
        }

    } // namespace
} // namespace MGITest
