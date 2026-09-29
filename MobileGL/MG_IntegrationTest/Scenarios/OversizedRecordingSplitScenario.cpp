// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/OversizedRecordingSplitScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A FRAME WITH MORE DRAWS THAN ONE COMMAND BUFFER MAY HOLD IS SPLIT, AND LOSES NOTHING.
//
// DirectVulkan records a frame into one command buffer, and used to submit it only at Present.
// A driver keeps a command buffer's GPU memory until the buffer is freed, and the Adreno 830
// driver maps that memory into the process in 16 KiB chunks. Minecraft rd12's loading frame
// records 981,654 draws (3.1M commands) into one buffer. It grew to ~40k such mappings and
// aborted inside the driver's own calloc once vm.max_map_count (65530) was spent. The renderer
// now submits the open buffer after MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER draws and goes
// on recording into a fresh one (VulkanRenderer::SplitOversizedRecording).
//
// Each draw here writes exactly one pixel of a 32x32 target, in its own colour, selected by a
// per-draw uniform, so every draw carries its own uniform state, like Minecraft's chunk draws.
// After 1024 draws every pixel must hold its own draw's colour. Each of these split bugs leaves
// a pixel wrong: a re-begun render pass that does not LOAD, a draw recorded into the retired
// buffer, or state that is not re-emitted on the fresh one.
//
// The ambient registrations run the same draws unsplit, because the default budget is far above
// 1024; that is the control. The DirectVulkan.SplitRecording. entry pins the budget to 64, so
// the frame is split 15 times. There the case also asserts the renderer's latched MGLOG_I, the
// one assertion that is red on a renderer that never splits.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

        constexpr int kSide = 32;
        constexpr int kDraws = kSide * kSide;

        // aPos spans the unit square; the uniform moves it onto cell (i % 32, i / 32) of a
        // 32x32 grid, which is exactly one pixel of a 32x32 target.
        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
uniform int uIndex;
void main() {
    vec2 cell = vec2(float(uIndex % 32), float(uIndex / 32));
    gl_Position = vec4((cell + aPos) / 16.0 - 1.0, 0.0, 1.0);
}
)";

        // Multiples of 8/255 survive an RGBA8 round trip exactly, so the readback bytes are
        // (8x, 8y, 255, 255) for the pixel at (x, y) and nothing else can produce them.
        constexpr const char* kFS = R"(#version 330 core
uniform int uIndex;
out vec4 o_color;
void main() {
    o_color = vec4(float(uIndex % 32) * 8.0 / 255.0, float(uIndex / 32) * 8.0 / 255.0, 1.0, 1.0);
}
)";

        // The budget the process was launched with, or 0 when it is unset or not a number.
        unsigned long PinnedDrawBudget() {
            const char* value = std::getenv("MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER");
            if (value == nullptr || *value == '\0') return 0;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            return (end != nullptr && *end == '\0') ? parsed : 0;
        }

        // The library log, for the arming assertion. Same machinery and reasoning as
        // UnlocatedIoBlockScenario: MOBILEGL_LOG_FILE_PATH is read at log-init, every process in
        // the lane appends to the file, and only bytes appended after the snapshot count.
        std::filesystem::path LibraryLogPath() {
            const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
            return (path != nullptr && *path != '\0') ? std::filesystem::path(path) : std::filesystem::path();
        }

        std::uintmax_t LibraryLogSize() {
            std::error_code ec;
            const std::filesystem::path path = LibraryLogPath();
            if (path.empty()) return 0;
            const std::uintmax_t size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }

        std::string LibraryLogSince(std::uintmax_t offset) {
            const std::filesystem::path path = LibraryLogPath();
            if (path.empty()) return {};
            std::ifstream file(path, std::ios::binary);
            if (!file.good()) return {};
            file.seekg(static_cast<std::streamoff>(offset));
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        ::testing::AssertionResult EveryPixelHoldsItsOwnDraw(const Image& image) {
            int wrong = 0;
            int firstX = -1, firstY = -1;
            Rgba8 firstSeen{};
            for (int y = 0; y < kSide; ++y) {
                for (int x = 0; x < kSide; ++x) {
                    const Rgba8 expected{static_cast<std::uint8_t>(8 * x), static_cast<std::uint8_t>(8 * y), 255, 255};
                    const Rgba8 seen = image.At(x, y);
                    if (seen != expected) {
                        if (wrong == 0) {
                            firstX = x;
                            firstY = y;
                            firstSeen = seen;
                        }
                        ++wrong;
                    }
                }
            }
            if (wrong == 0) {
                return ::testing::AssertionSuccess();
            }
            return ::testing::AssertionFailure()
                   << wrong << " of " << kDraws << " pixels do not hold their own draw's colour. First: (" << firstX
                   << ", " << firstY << ") = " << firstSeen << ", drawn by draw " << (firstY * kSide + firstX)
                   << " (a pixel no draw reached still holds the clear colour 0,0,0,0)";
        }

        class OversizedRecordingSplitScenario : public ScenarioTest {
        protected:
            // kDraws single-pixel quads into a fresh target, one uniform value per draw, with no
            // readback, flush or rebind in between, so the renderer sees one uninterrupted
            // recording. Then the whole target is read back.
            Image DrawEveryPixelWithItsOwnDraw(std::string* error) {
                const unsigned int program = CompileProgram(kVS, kFS, error);
                if (program == 0) return Image();
                ColorFbo target = MakeColorFbo(kSide, kSide);
                if (target.fbo == 0) {
                    *error = "MakeColorFbo failed";
                    glDeleteProgram(program);
                    return Image();
                }

                static const float kUnitQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
                GLuint vao = 0, vbo = 0;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

                BindFbo(target);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
                glUseProgram(program);
                const GLint indexLocation = glGetUniformLocation(program, "uIndex");
                for (int i = 0; i < kDraws; ++i) {
                    glUniform1i(indexLocation, i);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
                Image image = ReadPixels(kSide, kSide);

                glUseProgram(0);
                glBindVertexArray(0);
                glDeleteBuffers(1, &vbo);
                glDeleteVertexArrays(1, &vao);
                BindDefaultFramebuffer();
                DestroyColorFbo(target);
                glDeleteProgram(program);
                return image;
            }
        };

    } // namespace

    TEST_F(OversizedRecordingSplitScenario, EveryDrawOfTheFrameLandsAcrossCommandBufferSplits) {
        if (!Ready()) return;

        // Taken before the draws, so the line the arming check looks for can only have been
        // written by them. The renderer latches it at its first split.
        const std::uintmax_t logBefore = LibraryLogSize();

        std::string error;
        const Image image = DrawEveryPixelWithItsOwnDraw(&error);
        ASSERT_FALSE(image.Empty()) << error;
        EXPECT_TRUE(EveryPixelHoldsItsOwnDraw(image));
        EXPECT_EQ(FirstGLError(), 0u);

        // Arming: only where the budget is pinned below this frame's draw count, on the backend
        // that has the budget, and with a log to read. Everywhere else this case is the unsplit
        // control and stops here.
        const unsigned long budget = PinnedDrawBudget();
        const bool splitExpected = budget > 0 && budget < static_cast<unsigned long>(kDraws) &&
                                   Gl().BackendName() == std::string("DirectVulkan") && !LibraryLogPath().empty();
        RecordProperty("arming_checked", splitExpected ? "yes" : "no");
        if (!splitExpected) {
            return;
        }
        const std::string appended = LibraryLogSince(logBefore);
        EXPECT_NE(appended.find("draws into one command buffer; submitting it"), std::string::npos)
            << "MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER=" << budget << " and one frame recorded " << kDraws
            << " draws, but DirectVulkan never reported splitting the recording. A renderer that does not "
               "split grows a single command buffer without bound (rd12 on Adreno: ~40k driver mappings, "
               "then SIGABRT). See VulkanRenderer::SplitOversizedRecording. Log appended by this test:\n"
            << appended;
    }

} // namespace MGITest
