// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ImageLoadStoreSsoScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - IMAGE UNIFORMS REACHED THROUGH A PROGRAM PIPELINE.
//
// KHR-GL42.shader_image_load_store.advanced-sso-simple reduced to its mechanism. An ARRAY of
// image uniforms lives in a separable FRAGMENT program; the application assigns each element its
// own image unit with glProgramUniform1i, on a program that is not current and whose pipeline is
// not even bound yet; the draw then goes through the pipeline, i.e. through the flattened
// composite program (MG_State/GLState/Core.cpp, GetProgramForDraw) rather than through the stage
// program the units were written to.
//
// Three separate things have to survive that indirection, and each one is a different mechanism:
//
//   1. the units themselves, which are per-program state on a DIFFERENT object from the one the
//      draw reads (the composite mirror carries them);
//   2. the units as seen by a backend that cannot take them at draw time - Espryt has to BAKE an
//      image unit into the ESSL it generates, because ES forbids glUniform1i on image uniforms,
//      so a change has to invalidate the generated program;
//   3. per-ELEMENT assignment, which is what makes this different from every sampler case: the
//      four elements of g_image[] are four locations with four different units, and nothing may
//      collapse them to the array's base.
//
// Two pipelines that SHARE their vertex stage program and differ only in the fragment one are
// used exactly as the conformance case does, because that is what makes the composite cache and
// the stage programs' separate uniform storage both load-bearing at once.

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

        constexpr const char* kSsoVS = R"(#version 420 core
out gl_PerVertex { vec4 gl_Position; };
void main()
{
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); break;
      case 3: gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); break;
    }
}
)";

        // The conformance case's two fragment programs: one with an explicit format qualifier,
        // one writeonly with none. Both write every element of a four-image array and discard.
        constexpr const char* kImageFS0 = R"(#version 420 core
layout(rgba32f) uniform image2D g_image[4];
void main()
{
    for (int i = 0; i < g_image.length(); ++i) {
        imageStore(g_image[i], ivec2(gl_FragCoord), vec4(1.0));
    }
    discard;
}
)";

        constexpr const char* kImageFS1 = R"(#version 420 core
writeonly uniform image2D g_image[4];
void main()
{
    for (int i = 0; i < g_image.length(); ++i) {
        imageStore(g_image[i], ivec2(gl_FragCoord), vec4(2.0));
    }
    discard;
}
)";

        class ImageLoadStoreSsoScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glBindProgramPipeline(0);
                glUseProgram(0);
                for (GLuint p : m_programs) glDeleteProgram(p);
                for (GLuint p : m_pipelines) glDeleteProgramPipelines(1, &p);
                m_programs.clear();
                m_pipelines.clear();
            }

            GLuint MakeSeparable(GLenum stage, const char* source) {
                const GLuint program = glCreateShaderProgramv(stage, 1, &source);
                if (program != 0) m_programs.push_back(program);
                EXPECT_EQ(FirstGLError(), 0u)
                    << "glCreateShaderProgramv(stage 0x" << std::hex << stage << std::dec << ") left a GL error";
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "glCreateShaderProgramv(stage 0x" << std::hex << stage << std::dec
                                  << ") did not link: " << log;
                    return 0;
                }
                return program;
            }

            GLuint MakePipeline() {
                GLuint pipeline = 0;
                glGenProgramPipelines(1, &pipeline);
                m_pipelines.push_back(pipeline);
                return pipeline;
            }

            // Espryt reaches the GPU through an ES driver, and ES forbids glUniform1i on an
            // image uniform: the unit has to be BAKED into the generated ESSL as
            // layout(binding = N) (RebindImageUniformsToFrontendUnits, MG_Backend/DirectGLES).
            // One qualifier is all an ARRAY declaration can carry, and ESSL then gives the
            // array's elements the CONSECUTIVE units N, N+1, N+2, ... - so a per-element
            // assignment that is not consecutive (the conformance case uses 0, 2, 4, 6) has no
            // spelling in a single declaration.
            //
            // RemapImageArrayElementUnits repairs it by WIDENING the array to cover every unit
            // from the lowest it needs to the highest and routing each subscript through a const
            // offset table. What that costs is image-uniform budget: units 0..6 need SEVEN
            // fragment image uniforms where the application declared four. So the gate is the
            // budget, not the backend - a driver that cannot afford the widening declines it
            // (and says so at ERROR level) rather than addressing the wrong units silently.
            // DirectVulkan has no such constraint and needs no widening at all.
            bool PerElementImageUnitsAreHonoured() const {
                if (Gl().BackendName() == "DirectVulkan") return true;
                GLint maxFragmentImageUniforms = 0;
                glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImageUniforms);
                while (glGetError() != GL_NO_ERROR) {
                }
                // The widest span either of the two fragment programs below needs: 0..6 and 1..7.
                return maxFragmentImageUniforms >= 7;
            }

            // The scenarios below need image load/store at all; a driver without it should skip
            // rather than fail.
            bool ImagesAreUsable() const {
                GLint maxImageUnits = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits >= 8;
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_pipelines;
        };

    } // namespace

    // The whole conformance shape in one case: two pipelines sharing a vertex stage, four image
    // array elements each pointed at a different unit through glProgramUniform1i, eight layers of
    // one array texture bound one per unit, and every layer checked.
    //
    // Layers alternate 1.0 / 2.0 because the two fragment programs interleave their units
    // (0,2,4,6 and 1,3,5,7) - so a defect that collapses an image array to its base element, or
    // that loses the units on the way to the composite, does not merely dim the result: it puts
    // the wrong VALUE in a layer and names which one.
    TEST_F(ImageLoadStoreSsoScenario, PerElementImageUnitsReachAPipelineDraw) {
        if (!Ready()) return;
        if (!ImagesAreUsable()) GTEST_SKIP() << "fewer than 8 image units";
        if (!PerElementImageUnitsAreHonoured()) {
            GTEST_SKIP() << "fewer than 7 fragment image uniforms: the widening that covers a "
                             "non-consecutive image array does not fit";
        }
        HeadlessGL& gl = Gl();

        constexpr int kWidth = 8;
        constexpr int kHeight = 8;
        constexpr int kLayers = 8;

        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSsoVS);
        const GLuint fs0 = MakeSeparable(GL_FRAGMENT_SHADER, kImageFS0);
        const GLuint fs1 = MakeSeparable(GL_FRAGMENT_SHADER, kImageFS1);
        if (vs == 0 || fs0 == 0 || fs1 == 0) return;

        // Per ELEMENT, by name, on programs that are neither current nor attached to a bound
        // pipeline yet - exactly the conformance call order.
        const int units0[4] = {0, 2, 4, 6};
        const int units1[4] = {1, 3, 5, 7};
        for (int i = 0; i < 4; ++i) {
            const std::string name = "g_image[" + std::to_string(i) + "]";
            const GLint loc0 = glGetUniformLocation(fs0, name.c_str());
            const GLint loc1 = glGetUniformLocation(fs1, name.c_str());
            ASSERT_NE(loc0, -1) << "fs0 has no location for " << name;
            ASSERT_NE(loc1, -1) << "fs1 has no location for " << name;
            glProgramUniform1i(fs0, loc0, units0[i]);
            glProgramUniform1i(fs1, loc1, units1[i]);
        }
        ASSERT_EQ(FirstGLError(), 0u) << "assigning image units with glProgramUniform1i errored";

        const GLuint pipeline0 = MakePipeline();
        const GLuint pipeline1 = MakePipeline();
        glUseProgramStages(pipeline0, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline0, GL_FRAGMENT_SHADER_BIT, fs0);
        glUseProgramStages(pipeline1, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline1, GL_FRAGMENT_SHADER_BIT, fs1);
        ASSERT_EQ(FirstGLError(), 0u) << "pipeline setup errored";

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::vector<float> zeros(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, 0.0f);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, kWidth, kHeight, kLayers, 0, GL_RGBA, GL_FLOAT, zeros.data());
        ASSERT_EQ(FirstGLError(), 0u) << "creating the RGBA32F array texture errored";

        // One LAYER of the array texture per unit, which is what makes each element's unit
        // independently observable in the readback.
        for (int unit = 0; unit < kLayers; ++unit) {
            glBindImageTexture(static_cast<GLuint>(unit), texture, 0, GL_FALSE, unit, GL_READ_WRITE, GL_RGBA32F);
        }
        ASSERT_EQ(FirstGLError(), 0u) << "glBindImageTexture errored";

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        BindDefaultFramebuffer();
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(0);

        glBindProgramPipeline(pipeline0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindProgramPipeline(pipeline1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        EXPECT_EQ(FirstGLError(), 0u) << "the two pipeline draws leaked a GL error";

        std::vector<float> readback(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, -1.0f);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_FLOAT, readback.data());
        ASSERT_EQ(FirstGLError(), 0u) << "reading the array texture back errored";

        // Even layers were written through fs0's units, odd layers through fs1's.
        for (int layer = 0; layer < kLayers; ++layer) {
            const float expected = (layer % 2) ? 2.0f : 1.0f;
            int offenders = 0;
            float firstSeen = 0.0f;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const size_t base =
                        (static_cast<size_t>(layer) * kHeight * kWidth + static_cast<size_t>(y) * kWidth + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        if (readback[base + c] != expected) {
                            if (offenders == 0) firstSeen = readback[base + c];
                            ++offenders;
                        }
                    }
                }
            }
            EXPECT_EQ(offenders, 0) << "layer " << layer << " (image unit " << layer << ") expected " << expected
                                    << " but " << offenders << " components differ; first was " << firstSeen;
        }

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &texture);
        gl.EndFrame();
    }

    // An image ARRAY sharing a program with another descriptor, which is the shape that makes
    // the SPIR-V binding remap load-bearing.
    //
    // The remap (ProgramFactory::RemapDescriptorBindingsForVulkan) is what unifies bindings
    // across stages and normalises every descriptor onto set 0; glslang hands it per-stage
    // numbering that starts at 0 in EACH stage. It used to refuse any descriptor array that was
    // not a UBO, and its only complaint was an assert that compiles out above DEBUG - so a
    // release build carried on with the un-remapped numbering and a program holding an image
    // array plus a second descriptor could see the two alias onto one binding, while a DEBUG
    // build trapped on the very same program.
    //
    // A case with ONE descriptor cannot see any of that: with a single resource there is nothing
    // to collide with and skipping the remap is indistinguishable from running it. Hence this
    // one - an image array AND a uniform block in the same fragment program, with the block
    // supplying the value that gets stored, so a mis-assigned binding shows up as the wrong
    // colour rather than as nothing at all.
    TEST_F(ImageLoadStoreSsoScenario, AnImageArrayAlongsideAnotherDescriptorKeepsBothBindings) {
        if (!Ready()) return;
        if (!ImagesAreUsable()) GTEST_SKIP() << "fewer than 8 image units";
        // The defect this guards is the SPIR-V descriptor remap, which only Magma has; the units
        // here are consecutive on purpose, so on Espryt this would exercise nothing the case
        // above does not. Scoped by what it TESTS rather than by the image-array widening, which
        // it deliberately never triggers.
        if (Gl().BackendName() != "DirectVulkan") {
            GTEST_SKIP() << "the descriptor binding remap under test is DirectVulkan's";
        }
        HeadlessGL& gl = Gl();

        constexpr int kWidth = 8;
        constexpr int kHeight = 8;
        constexpr int kLayers = 2;

        static const char* kMixedFS = R"(#version 420 core
layout(rgba32f) uniform image2D g_image[2];
layout(std140) uniform Value { vec4 u_value; };
void main()
{
    for (int i = 0; i < g_image.length(); ++i) {
        imageStore(g_image[i], ivec2(gl_FragCoord), u_value);
    }
    discard;
}
)";
        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSsoVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kMixedFS);
        if (vs == 0 || fs == 0) return;

        // Consecutive units here on purpose: this case is about the two descriptor KINDS
        // coexisting, not about non-consecutive assignment, which the case above covers.
        for (int i = 0; i < 2; ++i) {
            const std::string name = "g_image[" + std::to_string(i) + "]";
            const GLint loc = glGetUniformLocation(fs, name.c_str());
            ASSERT_NE(loc, -1) << "no location for " << name;
            glProgramUniform1i(fs, loc, i);
        }

        const GLfloat value[4] = {7.0f, 7.0f, 7.0f, 7.0f};
        GLuint ubo = 0;
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(value), value, GL_STATIC_DRAW);
        const GLuint blockIndex = glGetUniformBlockIndex(fs, "Value");
        ASSERT_NE(blockIndex, GL_INVALID_INDEX);
        glUniformBlockBinding(fs, blockIndex, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        ASSERT_EQ(FirstGLError(), 0u) << "uniform block setup errored";

        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::vector<float> zeros(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, 0.0f);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, kWidth, kHeight, kLayers, 0, GL_RGBA, GL_FLOAT, zeros.data());
        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        glBindImageTexture(1, texture, 0, GL_FALSE, 1, GL_READ_WRITE, GL_RGBA32F);
        ASSERT_EQ(FirstGLError(), 0u) << "image texture setup errored";

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        BindDefaultFramebuffer();
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(0);
        glBindProgramPipeline(pipeline);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        EXPECT_EQ(FirstGLError(), 0u) << "the mixed-descriptor pipeline draw leaked a GL error";

        std::vector<float> readback(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, -1.0f);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_FLOAT, readback.data());
        ASSERT_EQ(FirstGLError(), 0u) << "reading the array texture back errored";

        for (int layer = 0; layer < kLayers; ++layer) {
            int offenders = 0;
            float firstSeen = 0.0f;
            for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight * 4; ++i) {
                const size_t index = static_cast<size_t>(layer) * kHeight * kWidth * 4 + i;
                if (readback[index] != 7.0f) {
                    if (offenders == 0) firstSeen = readback[index];
                    ++offenders;
                }
            }
            EXPECT_EQ(offenders, 0) << "layer " << layer << ": " << offenders
                                    << " components are not the uniform block's value; first was " << firstSeen
                                    << " (an image-array binding and a uniform block did not both survive)";
        }

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &texture);
        glDeleteBuffers(1, &ubo);
        gl.EndFrame();
    }

    // The same units, reassigned BETWEEN draws through the same pipeline. This is the half that
    // the composite cache key change put weight on: the composite object now survives a
    // glProgramUniform1i, so nothing rebuilds by accident and the new unit has to be carried by
    // the refresh path (and, on Espryt, by regenerating the program the unit is baked into).
    TEST_F(ImageLoadStoreSsoScenario, ReassigningAnImageUnitBetweenDrawsReachesTheNextDraw) {
        if (!Ready()) return;
        if (!ImagesAreUsable()) GTEST_SKIP() << "fewer than 8 image units";
        HeadlessGL& gl = Gl();

        constexpr int kWidth = 8;
        constexpr int kHeight = 8;
        constexpr int kLayers = 2;

        static const char* kSingleImageFS = R"(#version 420 core
layout(rgba32f) uniform image2D g_image;
void main()
{
    imageStore(g_image, ivec2(gl_FragCoord), vec4(3.0));
    discard;
}
)";
        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSsoVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kSingleImageFS);
        if (vs == 0 || fs == 0) return;

        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::vector<float> zeros(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, 0.0f);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, kWidth, kHeight, kLayers, 0, GL_RGBA, GL_FLOAT, zeros.data());
        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
        glBindImageTexture(1, texture, 0, GL_FALSE, 1, GL_READ_WRITE, GL_RGBA32F);
        ASSERT_EQ(FirstGLError(), 0u) << "image texture setup errored";

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        BindDefaultFramebuffer();
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(0);
        glBindProgramPipeline(pipeline);

        const GLint location = glGetUniformLocation(fs, "g_image");
        ASSERT_NE(location, -1);

        // Draw one against unit 0 (layer 0)...
        glProgramUniform1i(fs, location, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // ...and draw two against unit 1 (layer 1), with the composite already built and cached.
        glProgramUniform1i(fs, location, 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        EXPECT_EQ(FirstGLError(), 0u) << "the two pipeline draws leaked a GL error";

        std::vector<float> readback(static_cast<size_t>(kWidth) * kHeight * kLayers * 4, -1.0f);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_FLOAT, readback.data());
        ASSERT_EQ(FirstGLError(), 0u) << "reading the array texture back errored";

        for (int layer = 0; layer < kLayers; ++layer) {
            int offenders = 0;
            float firstSeen = 0.0f;
            for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight * 4; ++i) {
                const size_t index = static_cast<size_t>(layer) * kHeight * kWidth * 4 + i;
                if (readback[index] != 3.0f) {
                    if (offenders == 0) firstSeen = readback[index];
                    ++offenders;
                }
            }
            EXPECT_EQ(offenders, 0) << "layer " << layer << " was not written; " << offenders
                                    << " components differ, first was " << firstSeen
                                    << " (the image unit reassignment did not reach the draw)";
        }

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &texture);
        gl.EndFrame();
    }
} // namespace MGITest
