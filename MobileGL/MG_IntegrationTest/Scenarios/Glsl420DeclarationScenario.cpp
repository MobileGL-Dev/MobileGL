// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/Glsl420DeclarationScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GLSL 4.20 DECLARATIONS THE FRONTEND USED TO REJECT OR COLLAPSE.
//
// GLSL 4.20 gives an array of opaque uniforms or of block instances CONSECUTIVE binding
// points: "layout(binding = 1) uniform sampler2D goku[7]" puts goku[0] on texture unit 1
// and goku[6] on unit 7, and the same rule holds for "layout(binding = 2) uniform GOKU
// {...} goku[14]" over uniform buffer binding points 2..15 (GLSL 4.20 4.4.5, GL 4.6 7.6.2).
// One qualifier, N bindings - which is exactly the part that is easy to get wrong, because
// every element shares one declaration and one reflection record.
//
// Three separate mechanisms all collapsed that array down to its first element, and the
// three cases below pin one each:
//
//   * the SAMPLER array (Espryt): reflection names an array after its first element at
//     every location it spans, so the backend resolved "goku[0]" once per element, got one
//     backend location N times, and the per-draw pass's last glUniform1i was the only one
//     that survived. goku[0] ended up holding the LAST element's unit and goku[1..N-1] kept
//     unit 0 - so every element sampled whatever was bound to unit 0.
//   * the uniform BLOCK array (both backends): glslang reports the declared binding for
//     every expanded instance, so nothing added the element offset. glGetActiveUniformBlockiv
//     answered the base binding for all of them, and since both backends feed a block from
//     that same number at draw time, all instances also read one buffer.
//   * 'invariant' on a non-vertex stage's INPUT: legal desktop GLSL at every version, and
//     ignored where it is written, but glslang rejected it from 4.20 up - so a shader that
//     compiled as "#version 400" stopped compiling as "#version 420".
//
// The fourth case is the same species as the third - a legal 4.20 shader the frontend
// refused - and lives here for that reason: atomicCounterIncrement() was rejected because
// glslang applied its atomicAdd() extension gate to the atomicAdd() its own Vulkan-relaxed
// lowering had just synthesized.
//
// Conformance cases behind these: KHR-GL42.shading_language_420pack.binding_sampler_array,
// .binding_uniform_block_array, .qualifier_order[_block]_test_id_*, and
// KHR-GL42.shader_image_load_store.advanced-sso-atomicCounters.

#include <cstdint>
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

        constexpr int kElements = 4;

        // No vertex attributes: the quad comes from gl_VertexID, so nothing here depends on
        // the harness's attribute pinning and the fragment stage is the only thing under test.
        constexpr const char* kQuadVS = R"(#version 420 core
void main()
{
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); break;
      default: gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); break;
    }
}
)";

        // The red channel comes back as a BITMASK of which elements read the wrong thing, so
        // a failure names the element instead of just saying "not green". float(bad)/255.0
        // round-trips exactly through an RGBA8 target for every mask this can produce.
        constexpr const char* kSamplerArrayFS = R"(#version 420 core
layout(binding = 1) uniform sampler2D goku[4];
out vec4 o_color;
void main()
{
    const vec2 uv = vec2(0.5, 0.5);
    int bad = 0;
    if (texture(goku[0], uv) != vec4(1.0, 0.0, 0.0, 1.0)) bad |= 1;
    if (texture(goku[1], uv) != vec4(0.0, 0.0, 1.0, 1.0)) bad |= 2;
    if (texture(goku[2], uv) != vec4(1.0, 1.0, 0.0, 1.0)) bad |= 4;
    if (texture(goku[3], uv) != vec4(0.0, 1.0, 1.0, 1.0)) bad |= 8;
    o_color = vec4(float(bad) / 255.0, bad == 0 ? 1.0 : 0.0, 0.0, 1.0);
}
)";

        // Same declaration one dimension deeper. GLSL 4.30 arrays of arrays are legal here, and
        // the elements still take consecutive units (1..4) in declaration order - but the two
        // reflections disagree about how to count them, which is the whole point of this case.
        constexpr const char* kSamplerArrayOfArraysFS = R"(#version 430 core
layout(binding = 1) uniform sampler2D goku[2][2];
out vec4 o_color;
void main()
{
    const vec2 uv = vec2(0.5, 0.5);
    int bad = 0;
    if (texture(goku[0][0], uv) != vec4(1.0, 0.0, 0.0, 1.0)) bad |= 1;
    if (texture(goku[0][1], uv) != vec4(0.0, 0.0, 1.0, 1.0)) bad |= 2;
    if (texture(goku[1][0], uv) != vec4(1.0, 1.0, 0.0, 1.0)) bad |= 4;
    if (texture(goku[1][1], uv) != vec4(0.0, 1.0, 1.0, 1.0)) bad |= 8;
    o_color = vec4(float(bad) / 255.0, bad == 0 ? 1.0 : 0.0, 0.0, 1.0);
}
)";

        constexpr const char* kBlockArrayFS = R"(#version 420 core
layout(std140, binding = 2) uniform GOKU
{
    vec4 gohan;
} goku[4];
out vec4 o_color;
void main()
{
    int bad = 0;
    if (goku[0].gohan != vec4(1.0, 0.0, 0.0, 1.0)) bad |= 1;
    if (goku[1].gohan != vec4(0.0, 0.0, 1.0, 1.0)) bad |= 2;
    if (goku[2].gohan != vec4(1.0, 1.0, 0.0, 1.0)) bad |= 4;
    if (goku[3].gohan != vec4(0.0, 1.0, 1.0, 1.0)) bad |= 8;
    o_color = vec4(float(bad) / 255.0, bad == 0 ? 1.0 : 0.0, 0.0, 1.0);
}
)";

        // The producing stage declares the varying invariant (always legal) and the consuming
        // stage redeclares it (the part that regressed at 4.20). The qualifier ORDER is the
        // shuffled one 420pack exists to allow, so this also covers the parse path the
        // qualifier_order cases exercise.
        constexpr const char* kInvariantInVS = R"(#version 420 core
smooth invariant out highp vec4 v_data;
void main()
{
    v_data = vec4(0.0, 1.0, 0.0, 1.0);
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); break;
      default: gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); break;
    }
}
)";

        constexpr const char* kInvariantInFS = R"(#version 420 core
highp in smooth invariant vec4 v_data;
out vec4 o_color;
void main() { o_color = v_data; }
)";

        // atomicCounterIncrement() is core GLSL from 4.20 and needs no extension. MobileGL
        // parses under Vulkan-relaxed rules, which rewrite it into an atomicAdd() on a buffer
        // block - and glslang then applied to its OWN rewrite the desktop-below-430 gate that
        // demands GL_ARB_shader_storage_buffer_object for atomicAdd, rejecting a shader it had
        // just accepted. The shape is lifted from
        // KHR-GL42.shader_image_load_store.advanced-sso-atomicCounters.
        constexpr const char* kAtomicCounterVS = R"(#version 420 core
layout(binding = 0, offset = 0) uniform atomic_uint g_counter;
out flat uint v_index;
void main()
{
    v_index = atomicCounterIncrement(g_counter);
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); break;
      default: gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); break;
    }
}
)";

        constexpr const char* kAtomicCounterFS = R"(#version 420 core
in flat uint v_index;
out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        class Glsl420DeclarationScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                if (!m_textures.empty()) glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                if (!m_buffers.empty()) glDeleteBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
                for (GLuint p : m_programs) glDeleteProgram(p);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_textures.clear();
                m_buffers.clear();
                m_programs.clear();
                m_vao = 0;
            }

            GLuint Build(const char* vs, const char* fs) {
                std::string error;
                const GLuint program = CompileProgram(vs, fs, &error);
                if (program == 0) {
                    ADD_FAILURE() << "program did not build: " << error;
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            }

            // One 1x1 RGBA8 texture per element, each a colour whose channels are exactly 0 or
            // 255 so the shader's == comparisons are exact.
            void MakeElementTextures(const std::uint8_t colors[kElements][4]) {
                m_textures.assign(kElements, 0);
                glGenTextures(kElements, m_textures.data());
                for (int i = 0; i < kElements; ++i) {
                    glActiveTexture(GL_TEXTURE0 + 1 + i);
                    glBindTexture(GL_TEXTURE_2D, m_textures[i]);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colors[i]);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                }
                glActiveTexture(GL_TEXTURE0);
            }

            void MakeElementBuffers(const float values[kElements][4], GLuint firstBinding) {
                m_buffers.assign(kElements, 0);
                glGenBuffers(kElements, m_buffers.data());
                for (int i = 0; i < kElements; ++i) {
                    glBindBuffer(GL_UNIFORM_BUFFER, m_buffers[i]);
                    glBufferData(GL_UNIFORM_BUFFER, 4 * sizeof(float), values[i], GL_STATIC_DRAW);
                    glBindBufferBase(GL_UNIFORM_BUFFER, firstBinding + i, m_buffers[i]);
                }
                glBindBuffer(GL_UNIFORM_BUFFER, 0);
            }

            // Draws the full-screen quad and hands back the centre pixel.
            Rgba8 DrawAndRead(GLuint program) {
                HeadlessGL& gl = Gl();
                if (m_vao == 0) glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(program);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                const Image image = ReadPixels(gl.Width(), gl.Height());
                glUseProgram(0);
                return image.At(gl.Width() / 2, gl.Height() / 2);
            }

            // An array of ARRAYS is declined by Magma (ProgramFactory::ReflectLayout logs it and
            // VkProgramObject::declinedDescriptors then refuses every draw), which is a defined
            // outcome the case below can assert. Espryt has no such gate: it bakes the units the
            // frontend reports into its ESSL, and since the binding-qualifier seeding does not
            // walk the inner dimension every element reports unit 0 - so it samples one texture
            // four times and paints a mismatch. That gap is in the FRONTEND, one level below
            // either backend, and fixing it is the feature that would make this shape work
            // everywhere; it is not part of wiring descriptor arrays through Magma, so the
            // Espryt arm is SCOPED and the reflection half is asserted on both backends.
            bool MultiDimensionalSamplerArraysAreDeclined() const { return Gl().BackendName() == "DirectVulkan"; }

            // Same shape, different gap: with the compile fixed, this shader now links on
            // both backends but paints nothing on Magma - the atomic counter becomes a
            // buffer descriptor there and that half is not wired up yet (the conformance
            // case KHR-GL42.shader_image_load_store.advanced-sso-atomicCounters is where it
            // is measured). The regression this case exists for is the COMPILE, which is
            // asserted on both backends above; only the paint is scoped.
            bool AtomicCounterDrawsAreSupported() const { return Gl().BackendName() != "DirectVulkan"; }

            static std::string BadElements(std::uint8_t mask) {
                if (mask == 0) return "none";
                std::string out;
                for (int i = 0; i < kElements; ++i) {
                    if ((mask & (1u << i)) == 0) continue;
                    if (!out.empty()) out += ", ";
                    out += "[" + std::to_string(i) + "]";
                }
                return out;
            }

            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_buffers;
            std::vector<GLuint> m_programs;
            GLuint m_vao = 0;
        };

    } // namespace

    // Element k of a sampler array samples texture unit N+k - both as the API reports it and,
    // the part that was actually broken, as the draw behaves.
    TEST_F(Glsl420DeclarationScenario, SamplerArrayElementsSampleConsecutiveTextureUnits) {
        if (!Ready()) return;

        static const std::uint8_t colors[kElements][4] = {
            {255, 0, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}};
        MakeElementTextures(colors);

        const GLuint program = Build(kQuadVS, kSamplerArrayFS);
        if (program == 0) return;

        // The reported unit is the shadow the frontend seeds from the qualifier. It was
        // already right when the draw was wrong, so checking only this would have passed
        // straight through the bug - it is here to separate a reflection regression from a
        // backend one if this case ever fails again.
        glUseProgram(program);
        for (int i = 0; i < kElements; ++i) {
            const std::string name = "goku[" + std::to_string(i) + "]";
            const GLint location = glGetUniformLocation(program, name.c_str());
            ASSERT_GE(location, 0) << name << " has no location";
            GLint unit = -1;
            glGetUniformiv(program, location, &unit);
            EXPECT_EQ(unit, 1 + i) << name << " should default to texture unit " << (1 + i);
        }
        glUseProgram(0);

        const Rgba8 centre = DrawAndRead(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(centre.r, 0) << "sampler array elements that read the wrong texture: " << BadElements(centre.r);
        EXPECT_EQ(centre.g, 255) << "the draw did not reach the fragment stage at all";
    }

    // An array of ARRAYS of samplers is the shape the two reflections count differently:
    // SPIRV-Reflect reports one binding of 4 flattened descriptors, while the frontend hands out
    // uniform locations along the outer dimension only and keys the uniform by its full
    // "goku[0][0]" spelling. Magma therefore cannot address elements 1..3 of that binding, and
    // the contract this case pins is that it says so and DECLINES - the failure it must never
    // return to is resolving those elements onto whatever uniform got the next locations, which
    // is a silently wrong texture rather than a missing draw.
    //
    // Deliberately weak on the pixels for that reason: what is asserted on every backend is that
    // the program builds, the draw raises no GL error, and the process survives. Where the
    // descriptors do resolve, the colours are checked too.
    TEST_F(Glsl420DeclarationScenario, AnArrayOfSamplerArraysIsHonouredOrDeclinedCleanly) {
        if (!Ready()) return;

        static const std::uint8_t colors[kElements][4] = {
            {255, 0, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}};
        MakeElementTextures(colors);

        std::string error;
        const GLuint program = CompileProgram(kQuadVS, kSamplerArrayOfArraysFS, &error);
        if (program == 0) {
            GTEST_SKIP() << "the frontend does not build an array of sampler arrays: " << error;
        }
        m_programs.push_back(program);

        // The reflection DOES reserve one location per flattened element, in the order
        // SPIRV-Reflect flattens them - which is the whole reason baseLocation + element is the
        // right addressing rule for a descriptor array, and would be right for this shape too.
        // What is missing is one level up: the `layout(binding = 1)` unit seeding walks the outer
        // dimension only, so all four elements report unit 0 instead of 1..4. That is why this
        // shape is declined rather than supported, and it is asserted here because the day the
        // seeding learns arrays of arrays, the decline should be revisited rather than kept.
        glUseProgram(program);
        for (int outer = 0; outer < 2; ++outer) {
            for (int inner = 0; inner < 2; ++inner) {
                const std::string name = "goku[" + std::to_string(outer) + "][" + std::to_string(inner) + "]";
                EXPECT_EQ(glGetUniformLocation(program, name.c_str()), outer * 2 + inner)
                    << name << " should hold the flattened element's own location";
            }
        }
        glUseProgram(0);

        const Rgba8 centre = DrawAndRead(program);
        EXPECT_EQ(FirstGLError(), 0u) << "declining a descriptor array must not raise a GL error";

        if (!MultiDimensionalSamplerArraysAreDeclined()) {
            GTEST_SKIP() << "the frontend's binding-qualifier seeding does not walk an array of arrays, so "
                         << Gl().BackendName() << " samples unit 0 for every element; the locations "
                         << "asserted above are the half of this case it can answer";
        }

        // Three outcomes are possible and only two are acceptable. Green means every element
        // sampled its own unit. Black - the untouched clear - means the program was declined and
        // painted nothing, which is the documented Magma outcome. A non-zero red channel is the
        // third: the draw DID reach the fragment stage and elements read the wrong textures,
        // which is exactly the silent mismatch this decline exists to prevent.
        if (centre.g == 255) {
            EXPECT_EQ(centre.r, 0) << "elements of the array of arrays that read the wrong texture: "
                                   << BadElements(centre.r);
            return;
        }
        EXPECT_EQ(centre.r, 0) << "the array of arrays was not resolved, but the draw still painted "
                                  "a mismatch instead of being declined: " << BadElements(centre.r);
    }

    // Instance k of a uniform block array sits on buffer binding point N+k - again both as
    // reported and as fed to the shader.
    TEST_F(Glsl420DeclarationScenario, UniformBlockArrayInstancesTakeConsecutiveBindings) {
        if (!Ready()) return;

        static const float values[kElements][4] = {
            {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f, 1.0f}};
        constexpr GLuint kFirstBinding = 2;
        MakeElementBuffers(values, kFirstBinding);

        const GLuint program = Build(kQuadVS, kBlockArrayFS);
        if (program == 0) return;

        for (int i = 0; i < kElements; ++i) {
            const std::string name = "GOKU[" + std::to_string(i) + "]";
            const GLuint index = glGetUniformBlockIndex(program, name.c_str());
            ASSERT_NE(index, static_cast<GLuint>(GL_INVALID_INDEX)) << name << " is not an active block";
            GLint binding = -1;
            glGetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_BINDING, &binding);
            EXPECT_EQ(binding, static_cast<GLint>(kFirstBinding) + i)
                << name << " should start on binding point " << (kFirstBinding + i);
        }
        EXPECT_EQ(FirstGLError(), 0u) << "the block queries left a GL error behind";

        const Rgba8 centre = DrawAndRead(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(centre.r, 0) << "block array instances that read the wrong buffer: " << BadElements(centre.r);
        EXPECT_EQ(centre.g, 255) << "the draw did not reach the fragment stage at all";
    }

    // 'invariant' written on a fragment input at #version 420. The same source compiles at
    // #version 400 on any implementation, so a version-dependent rejection is the defect.
    TEST_F(Glsl420DeclarationScenario, InvariantIsAcceptedOnANonVertexStageInput) {
        if (!Ready()) return;

        const GLuint program = Build(kInvariantInVS, kInvariantInFS);
        if (program == 0) return;

        const Rgba8 centre = DrawAndRead(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(centre.g, 255) << "the invariant-qualified varying did not arrive";
        EXPECT_EQ(centre.r, 0);
    }

    // A #version 420 shader may call atomicCounterIncrement() with no extension at all. The
    // assertion is deliberately the COMPILE, because the defect was a compile-time gate on
    // glslang's own atomic-counter lowering; the draw that follows only checks the shader
    // survives the rest of the pipeline without leaving an error behind.
    TEST_F(Glsl420DeclarationScenario, AnAtomicCounterCompilesWithoutTheSsboExtension) {
        if (!Ready()) return;

        const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(shader, 1, &kAtomicCounterVS, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE) {
            char log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            glDeleteShader(shader);
            FAIL() << "atomicCounterIncrement() at #version 420 core did not compile: " << log;
        }
        glDeleteShader(shader);

        const GLuint program = Build(kAtomicCounterVS, kAtomicCounterFS);
        if (program == 0) return;

        GLuint counter = 0;
        glGenBuffers(1, &counter);
        m_buffers.push_back(counter);
        const GLuint zero = 0;
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counter);
        glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), &zero, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, counter);
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);

        if (!AtomicCounterDrawsAreSupported()) {
            GTEST_SKIP() << "atomic-counter draws do not paint on " << Gl().BackendName()
                         << " yet; the compile above is what this case pins";
        }

        const Rgba8 centre = DrawAndRead(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(centre.g, 255) << "the atomic-counter shader linked but painted nothing";
    }

} // namespace MGITest
