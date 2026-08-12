// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/FragmentOutputArrayIndexScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - DYNAMICALLY INDEXED FRAGMENT OUTPUT ARRAYS, on a live driver.
//
// The bug: GLSL ES requires a *constant integral expression* to index a fragment output array
// (GLSL ES 3.00 4.3.6); SPIR-V has no such rule. A shader that writes `coeff[i]` from a loop
// therefore travels through glslang and SPIRV-Cross intact and lands on the ES driver as ESSL it
// refuses outright - "array indexes for fragment outputs must be constant integral expressions".
// The program links nothing and every draw that uses it becomes a silent no-op. That is the whole
// of improved-transparency-minecraft-26.3 on the Android DirectGLES lane: Minecraft 26.3's OIT
// coefficient shader has exactly this shape, and losing it empties the entire translucent layer
// (clouds and water) while the opaque geometry stays pixel-exact.
//
// WHY THIS SCENARIO EXISTS RATHER THAN A UNIT TEST. The unit tests in MG_Test/Program (see
// ProgramUtilTest, LoopDerivedFragmentOutputIndexFoldsToConstantIndices and its
// genuinely-dynamic sibling) prove the SPIR-V comes out with constant indices, validates, and
// decompiles to ESSL with only literal indices. What they cannot prove is that a real driver
// then ACCEPTS and RUNS it - and acceptance is the whole failure mode, because Mesa accepts the
// illegal form too. Only a live glCompileShader/glLinkProgram followed by a draw can tell the two
// apart, and only reading the pixels back can tell "linked" from "wrote the right attachment".
//
// Both backends run this: on DirectVulkan the original module is already legal (the legalization
// is DirectGLES-only, deliberately), so this doubles as the check that the two backends agree
// about what such a shader means.

#include <cmath>
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

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // The Minecraft 26.3 OIT coefficient shape: both the attachment index and the component
        // index come from loop counters, so nothing but the loop bounds decides where each value
        // lands. Attachment 0 gets (0.0, 0.1, 0.2, 0.3) and attachment 1 gets (0.5, 0.6, 0.7, 0.8) -
        // values that are only correct if the two indices were folded to the RIGHT constants, not
        // merely to some constant.
        constexpr const char* kLoopIndexedFS = R"(#version 330 core
out vec4 coeff[2];
void main() {
    for (int attachmentIndex = 0; attachmentIndex < 2; ++attachmentIndex) {
        for (int i = 0; i < 4; ++i) {
            coeff[attachmentIndex][i] = float(attachmentIndex) * 0.5 + float(i) * 0.1;
        }
    }
}
)";

        // No loop can fold this one: the index arrives in a uniform. It exercises the fallback
        // lowering (a switch over the array range for the write, constant-indexed loads and a
        // select for the read) and it checks the untargeted attachment is left ALONE, which a
        // lowering that wrote every element unconditionally would break.
        constexpr const char* kUniformIndexedFS = R"(#version 330 core
uniform int uTarget;
out vec4 coeff[2];
void main() {
    coeff[0] = vec4(0.25, 0.25, 0.25, 1.0);
    coeff[1] = vec4(0.75, 0.75, 0.75, 1.0);
    coeff[uTarget] = coeff[uTarget] + vec4(0.25, 0.0, 0.0, 0.0);
}
)";

        constexpr int kSize = 8;

        class FragmentOutputArrayIndexScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                for (int i = 0; i < 2; ++i) {
                    glGenTextures(1, &m_color[i]);
                    glBindTexture(GL_TEXTURE_2D, m_color[i]);
                    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                           m_color[i], 0);
                }
                const GLenum drawBuffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                glDrawBuffers(2, drawBuffers);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                          static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

                const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                glViewport(0, 0, kSize, kSize);
            }

            void TearDown() override {
                if (Ready()) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &m_fbo);
                    glDeleteTextures(2, m_color);
                    glDeleteBuffers(1, &m_vbo);
                    glDeleteVertexArrays(1, &m_vao);
                }
                ScenarioTest::TearDown();
            }

            // Clears both attachments to a colour no shader below writes, so an attachment that
            // was never written reads back as the sentinel rather than as a plausible value.
            void ClearToSentinel() {
                glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }

            std::vector<float> ReadAttachment(int index) {
                std::vector<unsigned char> bytes(static_cast<std::size_t>(kSize) * kSize * 4, 0);
                glReadBuffer(GL_COLOR_ATTACHMENT0 + index);
                glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
                std::vector<float> centre(4, -1.0f);
                // The middle pixel: the quad covers the whole target, so every pixel is the same,
                // and the middle one cannot be a rasterization edge case.
                const std::size_t offset = (static_cast<std::size_t>(kSize / 2) * kSize + kSize / 2) * 4;
                for (int i = 0; i < 4; ++i) {
                    centre[static_cast<std::size_t>(i)] = static_cast<float>(bytes[offset + i]) / 255.0f;
                }
                return centre;
            }

            GLuint m_fbo = 0;
            GLuint m_color[2] = {0, 0};
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

        // The gate for the whole defect: before the legalization this program did not link on a
        // strict ES driver (ANGLE), so the draw wrote nothing and BOTH attachments kept the
        // sentinel. Now each attachment must carry the value its loop iteration produced.
        TEST_F(FragmentOutputArrayIndexScenario, LoopIndexedOutputArrayWritesEveryAttachment) {
            if (!Ready() || IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kVS, kLoopIndexedFS, &error);
            ASSERT_NE(program, 0u) << "a loop-indexed fragment output array must compile and link: "
                                   << error;

            ClearToSentinel();
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            const std::vector<float> first = ReadAttachment(0);
            EXPECT_NEAR(first[0], 0.0f, 0.02f) << "attachment 0 red";
            EXPECT_NEAR(first[1], 0.1f, 0.02f) << "attachment 0 green";
            EXPECT_NEAR(first[2], 0.2f, 0.02f)
                << "attachment 0 blue - a sentinel 1.0 here means the draw never ran";
            EXPECT_NEAR(first[3], 0.3f, 0.02f) << "attachment 0 alpha";

            const std::vector<float> second = ReadAttachment(1);
            EXPECT_NEAR(second[0], 0.5f, 0.02f)
                << "attachment 1 red - the second loop iteration must reach the second draw buffer";
            EXPECT_NEAR(second[1], 0.6f, 0.02f) << "attachment 1 green";
            EXPECT_NEAR(second[2], 0.7f, 0.02f) << "attachment 1 blue";
            EXPECT_NEAR(second[3], 0.8f, 0.02f) << "attachment 1 alpha";

            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The fallback half, on a live driver, for both values of the uniform: the targeted
        // attachment is read, incremented and written back; the other one keeps exactly what the
        // constant-indexed store put there.
        TEST_F(FragmentOutputArrayIndexScenario, UniformIndexedOutputArrayWritesOnlyTheSelectedAttachment) {
            if (!Ready() || IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kVS, kUniformIndexedFS, &error);
            ASSERT_NE(program, 0u) << "a uniform-indexed fragment output array must compile and link: "
                                   << error;
            const GLint targetLocation = glGetUniformLocation(program, "uTarget");
            ASSERT_GE(targetLocation, 0);
            glUseProgram(program);

            for (int target = 0; target < 2; ++target) {
                ClearToSentinel();
                glUniform1i(targetLocation, target);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                const std::vector<float> first = ReadAttachment(0);
                const std::vector<float> second = ReadAttachment(1);
                EXPECT_NEAR(first[0], target == 0 ? 0.5f : 0.25f, 0.02f)
                    << "attachment 0 red with uTarget=" << target;
                EXPECT_NEAR(first[1], 0.25f, 0.02f) << "attachment 0 green with uTarget=" << target;
                EXPECT_NEAR(second[0], target == 1 ? 1.0f : 0.75f, 0.02f)
                    << "attachment 1 red with uTarget=" << target;
                EXPECT_NEAR(second[1], 0.75f, 0.02f) << "attachment 1 green with uTarget=" << target;
            }

            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

    } // namespace
} // namespace MGITest
