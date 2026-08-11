// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PipelineFailureScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// "The draw had no pipeline, so we bound null."
//
// DirectVulkan's SetupDraw called GetOrCreatePipeline - a function that DOCUMENTS a
// VK_NULL_HANDLE return - and passed the result straight to vkCmdBindPipeline. When the
// Adreno driver answered vkCreateGraphicsPipelines with VK_ERROR_UNKNOWN, the next
// instruction dereferenced null inside the driver: SIGSEGV at fault addr 0x8, and that one
// shape accounted for 9 of the 15 process deaths in the 2026-08-10 GL-CTS run
// (KHR-GL33/GL40.shaders.struct.uniform.sampler_array_vertex, six
// KHR-GL42.shader_image_load_store cases, one shader_storage_buffer_object case).
//
// It was made permanent by a second defect: PipelineFactory memoized the failure, so the
// null was served for the rest of the process. Every later draw with the same state died
// too, which is why a single bad program took whole CTS groups down with it.
//
// What this scenario pins, on both backends:
//   1. The GL program shape the CTS crashed on (an array of structs each containing a
//      sampler, sampled from the VERTEX stage) draws without killing the process.
//   2. It draws AGAIN and produces the identical image. A second draw is the only thing
//      that can tell a working pipeline apart from a poisoned cache entry: if the first
//      creation had failed and been memoized, the second draw is where the null would be
//      served back.
//
// A deterministic driver-side pipeline-creation FAILURE is not reachable from the GL API on
// the llvmpipe/lavapipe lanes - both accept every pipeline these scenarios can describe - so
// the guard itself is proven structurally (PipelineFactory returns before it can emplace a
// VK_NULL_HANDLE, SetupDraw returns false before it can bind one) and this scenario holds
// the surrounding path honest.

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

        // Lifted from KHR-GL33.shaders.struct.uniform.sampler_array_vertex (the QPA records the
        // source verbatim): an array of structs, each carrying an opaque sampler, sampled in the
        // vertex stage. The fragment sibling of this case only FAILS on Magma; only the vertex one
        // takes the process down, so the stage matters and is kept.
        constexpr const char* kSamplerArrayVertexSource = R"(#version 330 core
struct S {
    float a;
    vec3 b;
    sampler2D c;
};
uniform S s[2];
in vec2 aPos;
out vec4 vColor;
void main() {
    vec2 coords = aPos * 0.5 + 0.5;
    vColor = vec4(texture(s[1].c, coords * s[0].b.xy + s[1].b.z).rgb, s[0].a);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kPassthroughFragmentSource = R"(#version 330 core
in vec4 vColor;
out vec4 oColor;
void main() {
    oColor = vColor;
}
)";

        struct Vertex {
            float x, y;
        };

        std::vector<Vertex> FullscreenTriangleStrip() {
            return {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
        }

        class PipelineFailureScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kSamplerArrayVertexSource, kPassthroughFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                const std::vector<Vertex> vertices = FullscreenTriangleStrip();
                m_vertexCount = static_cast<int>(vertices.size());
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(),
                             GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
                glBindVertexArray(0);

                // A solid red 2x2 texture, so the sampled colour is the same wherever the
                // (deliberately degenerate) coordinates land.
                const unsigned char red[] = {255, 0, 0, 255, 255, 0, 0, 255,
                                             255, 0, 0, 255, 255, 0, 0, 255};
                glGenTextures(1, &m_texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glUseProgram(m_program);
                const int samplerLocation = glGetUniformLocation(m_program, "s[1].c");
                if (samplerLocation >= 0) glUniform1i(samplerLocation, 0);
                const int alphaLocation = glGetUniformLocation(m_program, "s[0].a");
                if (alphaLocation >= 0) glUniform1f(alphaLocation, 1.0f);
                glUseProgram(0);

                m_target = MakeColorFbo(Gl().Width(), Gl().Height());
                ASSERT_NE(m_target.fbo, 0u) << "offscreen FBO is not framebuffer-complete";

                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                DestroyColorFbo(m_target);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            Image DrawOnce() {
                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(m_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, m_vertexCount);
                glBindVertexArray(0);
                return ReadPixels(m_target.width, m_target.height);
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
            unsigned int m_texture = 0;
            int m_vertexCount = 0;
            ColorFbo m_target;
        };

        // Reaching the assertion at all is most of the point: the shipped code SIGSEGV'd inside
        // the driver on this draw.
        TEST_F(PipelineFailureScenario, SamplerArrayInAStructDrawsWithoutKillingTheProcess) {
            const Image drawn = DrawOnce();
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_TRUE(RegionIsMostly(drawn, 2, drawn.Width() - 3, 2, drawn.Height() - 3, "red", 0.0,
                                       "sampler-array-in-struct draw"));
        }

        // The second draw is what a poisoned cache entry cannot survive: a memoized
        // VK_NULL_HANDLE is served on every subsequent lookup, so a run that dies (or silently
        // stops drawing) on the second draw and not the first is exactly the "failed pipeline was
        // cached" defect.
        TEST_F(PipelineFailureScenario, TheSameDrawRepeatsIdenticallyWithNoPoisonedPipelineCache) {
            const Image first = DrawOnce();
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the first draw already errored";
            Gl().EndFrame();
            const Image second = DrawOnce();
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the second draw errored";

            EXPECT_TRUE(RegionIsMostly(second, 2, second.Width() - 3, 2, second.Height() - 3, "red", 0.0,
                                       "second draw"));
            EXPECT_TRUE(second == first) << "the second draw differs from the first in "
                                         << second.ByteDiffCount(first) << " bytes - the pipeline the second "
                                            "draw resolved is not the one the first draw used";
        }

    } // namespace
} // namespace MGITest
