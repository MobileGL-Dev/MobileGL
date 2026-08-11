// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/FragCoordOriginScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - gl_FragCoord ON THE DEFAULT FRAMEBUFFER CARRIES GL'S WINDOW ORIGIN.
//
// GL measures gl_FragCoord.y from the BOTTOM of the window. Vulkan's gl_FragCoord.y is the
// framebuffer ROW being written, and DirectVulkan stores the default framebuffer display-side-up
// (compensating for vertices by negating gl_Position.y), so a fragment's reported Y there was
// `height - y_GL` - flipped, and for a viewport that does not span the full height, outside the
// range GL promises entirely. GL CTS
// `KHR-GL42.shader_image_load_store.basic-{allTargets-atomic,glsl-earlyFragTests,glsl-misc}`
// caught it: each sets a small viewport at GL y=0 and does
// `imageStore(image, ivec2(gl_FragCoord.xy), ...)` into an image exactly that size, so on a
// 256-tall surface every store addressed rows 224..255 of a 32-row image and was dropped.
//
// The shader here paints each row with its own GL window Y, which is the whole claim in one
// value: row j of the readback must be j, for a full-height viewport and for a half-height one
// (the case where a flip and an offset can no longer hide each other). DirectGLES is the
// built-in control - a native GL driver gets this right by construction, so a failure there
// would mean the test, not the backend.

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

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        // floor(gl_FragCoord.y) is the fragment's window row; 1/255 steps survive an RGBA8
        // round trip exactly, so the readback byte IS the row the shader believes it is on.
        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(floor(gl_FragCoord.y) / 255.0, 0.0, 0.0, 1.0); }
)";

        class FragCoordOriginScenario : public ScenarioTest {};

        // A quad covering the whole viewport, drawn with attribute 0 = aPos.
        void DrawFullViewportQuad(unsigned int program) {
            static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        // Paints `viewportHeight` rows starting at GL y=0 and returns the red byte of each row.
        std::vector<int> RowsPaintedWithTheirOwnWindowY(unsigned int program, int width, int viewportHeight) {
            BindDefaultFramebuffer();
            glViewport(0, 0, width, viewportHeight);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
            DrawFullViewportQuad(program);

            const Image image = ReadPixelsRect(0, 0, width, viewportHeight);
            std::vector<int> rows;
            rows.reserve(static_cast<std::size_t>(viewportHeight));
            for (int y = 0; y < viewportHeight; ++y) {
                rows.push_back(image.At(width / 2, y).r);
            }
            return rows;
        }

        ::testing::AssertionResult RowsAreTheirOwnIndex(const std::vector<int>& rows, const char* when) {
            for (std::size_t y = 0; y < rows.size(); ++y) {
                if (rows[y] != static_cast<int>(y)) {
                    return ::testing::AssertionFailure()
                           << when << ": GL window row " << y << " reported gl_FragCoord.y = " << rows[y]
                           << " (expected " << y << "). Rows 0.." << (rows.size() - 1) << " read back as ["
                           << rows.front() << " .. " << rows.back() << "].";
                }
            }
            return ::testing::AssertionSuccess();
        }

    } // namespace

    TEST_F(FragCoordOriginScenario, DefaultFramebufferFragCoordCountsFromTheBottom) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        // 1/255 steps only stay distinguishable while the row index fits in a byte.
        const int width = gl.Width();
        const int fullHeight = std::min(gl.Height(), 256);
        ASSERT_GE(fullHeight, 8) << "the harness surface is too small to tell rows apart";

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        // Full height first: this one passed even before the fix (a flip alone maps the row set
        // onto itself), so it is the control that the shader and the readback agree at all.
        EXPECT_TRUE(RowsAreTheirOwnIndex(RowsPaintedWithTheirOwnWindowY(program, width, fullHeight),
                                         "full-height viewport"));

        // Half height at GL y=0: the case the CTS failures were made of. A backend that reports
        // the stored row here answers `height - y` for every row - off the bottom of the range,
        // not merely reversed within it.
        const int halfHeight = fullHeight / 2;
        EXPECT_TRUE(RowsAreTheirOwnIndex(RowsPaintedWithTheirOwnWindowY(program, width, halfHeight),
                                         "half-height viewport at GL y=0"));

        glUseProgram(0);
        glDeleteProgram(program);
        glViewport(0, 0, gl.Width(), gl.Height());
        EXPECT_EQ(FirstGLError(), 0u);
    }

} // namespace MGITest
