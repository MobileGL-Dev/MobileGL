// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SampledSetStalenessScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TEXTURE THAT BECOMES COMPLETE WITHOUT A REBIND MUST RE-ENTER THE SAMPLED SET.
//
// DirectVulkan does not bind a texture GL calls incomplete: it substitutes a fallback so the
// sampler reads (0,0,0,1) instead of losing the draw. That decision is made twice per draw - once
// by CollectSampledTextures, which builds the list SetupDraw syncs, materialises pending clears
// for and transitions to a sampled layout BEFORE the render pass opens, and once by the descriptor
// resolve inside the pass. Both ask SamplesAsIncompleteTexture.
//
// The per-draw memo that lets the first of those be skipped was keyed only on the program, the
// transform flags and the texture BIND generation. Completeness is not a function of any of them:
// it moves on a filter change (glTexParameteri / glSamplerParameteri), on a level-range change,
// and on an upload that fills the mip chain - none of which bind anything. So a texture that went
// incomplete -> complete under a fixed binding kept being answered out of the memo as "not in the
// set", and the work SetupDraw does for the set never happened for it:
//
//   * its queued clear was never materialised, so the draw sampled pre-clear content - wrong
//     pixels, no validation layer needed, which is what the case below detects; and
//   * its layout transition moved into the descriptor resolve, which records
//     vkCmdPipelineBarrier inside an already-open render pass whose subpass declares no
//     self-dependency - the exact hazard CollectSampledTextures exists to prevent.
//
// The fix adds the sampling-resolution generation to that memo key, which is the counter the
// codebase already maintains for "what a unit resolves to changed without a bind" and which both
// TextureObjectBase::BumpShapeVersion and SamplerObject::BumpVersion move.

#include <cstddef>
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

        constexpr int kFboSize = 32;
        constexpr int kTexSize = 8;

        constexpr const char* kQuadVertexSource = R"(#version 430 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        // texelFetch, not texture(): the point is WHICH image is sampled, and a fetch cannot be
        // explained away by filtering.
        constexpr const char* kSampleFragmentSource = R"(#version 430 core
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    o_color = texelFetch(u_tex, ivec2(0, 0), 0);
}
)";

        class SampledSetStalenessScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                m_target = MakeColorFbo(kFboSize, kFboSize);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                glGenVertexArrays(1, &m_vao);
                std::string error;
                m_program = CompileProgram(kQuadVertexSource, kSampleFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                // The sampled texture: ONE level, and no glTexParameteri at all, so MIN_FILTER
                // keeps its initial GL_NEAREST_MIPMAP_LINEAR and GL calls it mipmap-incomplete.
                glGenTextures(1, &m_texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                std::vector<unsigned char> green(static_cast<std::size_t>(kTexSize * kTexSize * 4), 0);
                for (std::size_t i = 0; i < green.size(); i += 4) {
                    green[i + 1] = 255;
                    green[i + 3] = 255;
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTexSize, kTexSize, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             green.data());
                ASSERT_EQ(FirstGLError(), 0u) << "defining the sampled texture raised a GL error";
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                glUseProgram(0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                DestroyColorFbo(m_target);
                ScenarioTest::TearDown();
            }

            // One draw of the fullscreen quad sampling texel (0,0) of whatever unit 0 holds, and
            // NO readback. That matters: a readback submits and waits, which ends the command
            // buffer and resets the per-draw memos with it - so a case that read back between its
            // two draws would never leave a stale entry to catch. The two draws here have to land
            // in one recording.
            void DrawOnly() {
                BindFbo(m_target);
                glBindVertexArray(m_vao);
                glUseProgram(m_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                const GLint location = glGetUniformLocation(m_program, "u_tex");
                if (location != -1) glUniform1i(location, 0);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                EXPECT_EQ(FirstGLError(), 0u) << "the sampling draw raised a GL error";
            }

            ColorFbo m_target{};
            GLuint m_vao = 0;
            GLuint m_texture = 0;
            unsigned int m_program = 0;
        };

    } // namespace

    // The full sequence, ordered so the ONLY state change between the two draws is the filter.
    TEST_F(SampledSetStalenessScenario, AQueuedClearIsMaterialisedWhenAFilterChangeCompletesTheTexture) {
        if (!Ready() || IsSkipped()) return;

        // 1. Queue a clear on the texture through an FBO and take it straight back out, with no
        //    draw in between - the "attach -> clear -> detach" shape that leaves the clear
        //    pending for whoever samples the texture next.
        GLuint clearFbo = 0;
        glGenFramebuffers(1, &clearFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, clearFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 0, red);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &clearFbo);
        ASSERT_EQ(FirstGLError(), 0u) << "queueing the clear raised a GL error";

        BindFbo(m_target);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);

        // 2. Draw while the texture is still incomplete. The backend substitutes its fallback,
        //    and the per-draw memo records the resulting sampled set.
        DrawOnly();

        // 3. Make it complete. No bind, no upload, no program change - one filter write, which is
        //    exactly the state the old memo key could not see.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ASSERT_EQ(FirstGLError(), 0u) << "changing the filter raised a GL error";

        // 4. Draw again, into the same recording, and only now read back. The texture is in the
        //    sampled set now, so its queued clear has to be materialised before the pass opens and
        //    the fetch has to see RED. Reading the green the texture was uploaded with means the
        //    clear was never materialised, i.e. the texture never entered the set - the stale-memo
        //    bug. Black means the fallback was still being handed out.
        DrawOnly();
        const Image afterFlip = ReadPixels(kFboSize, kFboSize);
        ASSERT_FALSE(afterFlip.Empty()) << "the readback came back empty";
        EXPECT_TRUE(RegionIsMostly(afterFlip, 0, kFboSize - 1, 0, kFboSize - 1, "red", 0.0,
                                   "the draw after the completeness flip"))
            << "green means the queued clear was never materialised, so the texture never re-entered "
               "the sampled set after the filter change; blue means the draw did not happen at all";
    }

    // The same flip driven from a SAMPLER OBJECT rather than the texture's own parameters. It is
    // the other half of what feeds the completeness predicate, it moves the same generation, and
    // it likewise binds nothing.
    TEST_F(SampledSetStalenessScenario, AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture) {
        if (!Ready() || IsSkipped()) return;

        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        // Bound BEFORE the first draw, still carrying the mipmapping default, so binding it is
        // not what changes between the two draws.
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        glBindSampler(0, sampler);
        ASSERT_EQ(FirstGLError(), 0u) << "binding the sampler object raised a GL error";

        GLuint clearFbo = 0;
        glGenFramebuffers(1, &clearFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, clearFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 0, red);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &clearFbo);
        ASSERT_EQ(FirstGLError(), 0u) << "queueing the clear raised a GL error";

        BindFbo(m_target);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawOnly();

        // One parameter write on an ALREADY-BOUND sampler object.
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ASSERT_EQ(FirstGLError(), 0u) << "changing the sampler filter raised a GL error";

        DrawOnly();
        const Image afterFlip = ReadPixels(kFboSize, kFboSize);
        ASSERT_FALSE(afterFlip.Empty()) << "the readback came back empty";
        EXPECT_TRUE(RegionIsMostly(afterFlip, 0, kFboSize - 1, 0, kFboSize - 1, "red", 0.0,
                                   "the draw after the sampler-object flip"))
            << "green means the queued clear was never materialised after the sampler parameter change";

        glBindSampler(0, 0);
        glDeleteSamplers(1, &sampler);
    }

} // namespace MGITest
