// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PixelStoreSweepScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - PIXEL-STORE MODES RESTORE, and FRAMEBUFFER CHURN STAYS EXACT.
//
// Both cases here replay the shape of KHR-GL3x.packed_pixels.varied_rectangle, the single
// heaviest polluter in the GL CTS: for each of 46 (pixel-store mode, value) pairs it uploads a
// gradient into a fresh texture, attaches that texture to a FRESH framebuffer, reads it back and
// deletes both - ~3300 texture+framebuffer pairs per test case.
//
// What that found: DirectGLES had no destructor for BackendFramebufferObject (nor for the
// renderbuffer and sampler twins), so every frontend glDeleteFramebuffers leaked one driver
// framebuffer for the process lifetime. On an Adreno 830 the CTS run walked the driver to 1.2 GB
// of dead objects, and from that point on EVERY readback through a freshly attached framebuffer
// came back with someone else's pixels - which is what made ~1,500 otherwise-correct cases fail
// depending only on how much ran before them. The unit-level pin for the missing destructors is
// MG_Test/SanityTest.cpp (DirectGLESBackendFramebuffer/Renderbuffer/Sampler); this file pins the
// end-to-end behaviour they protect.
//
// The mode sweep is the second half of the same story: 46 modes are set and reset per case, so a
// mode that fails to restore is indistinguishable from the leak in a full-batch CTS run. The
// assertion here is RESTORATION - after every single mode is set and put back, a readback at
// default state must be byte-identical to one taken before the sweep ever started.
//
// Backend-agnostic on purpose: both bugs this guards against are frontend/backend bookkeeping,
// and DirectVulkan is the built-in control.

#include <cstdint>
#include <cstring>
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

        // Small enough that the table's row lengths (10, 15) and image heights are all >= the
        // image, which is the shape the CTS uses (its gradient is 7x3).
        constexpr int kTexSize = 8;
        // Every buffer handed to GL is this big regardless of the image size: with row length 15,
        // two skipped rows/pixels and alignment 8 the driver strides well past the natural image
        // extent, and a tight buffer would be an out-of-bounds access rather than a test. (It was:
        // the first version of this scenario passed its assertions and then segfaulted at
        // teardown, because glReadPixels had written past a 1 KiB destination.)
        constexpr std::size_t kScratchBytes = 64 * 1024;

        // Every pixel-store mode GL 4.0 has, so a reset provably covers the whole state and not
        // just the subset a particular test happened to touch.
        struct PixelStoreMode {
            GLenum name;
            GLint defaultValue;
        };
        const PixelStoreMode kAllModes[] = {
            {GL_UNPACK_SWAP_BYTES, 0},   {GL_UNPACK_LSB_FIRST, 0},   {GL_UNPACK_ROW_LENGTH, 0},
            {GL_UNPACK_IMAGE_HEIGHT, 0}, {GL_UNPACK_SKIP_ROWS, 0},   {GL_UNPACK_SKIP_PIXELS, 0},
            {GL_UNPACK_SKIP_IMAGES, 0},  {GL_UNPACK_ALIGNMENT, 4},   {GL_PACK_SWAP_BYTES, 0},
            {GL_PACK_LSB_FIRST, 0},      {GL_PACK_ROW_LENGTH, 0},    {GL_PACK_IMAGE_HEIGHT, 0},
            {GL_PACK_SKIP_ROWS, 0},      {GL_PACK_SKIP_PIXELS, 0},   {GL_PACK_SKIP_IMAGES, 0},
            {GL_PACK_ALIGNMENT, 4},
        };

        // The CTS table verbatim (glcPackedPixelsTests.cpp VariedRectangleTest::iterate): 32
        // common cases plus the 14 core-only ones ES has no equivalent for and MobileGL therefore
        // honours on the CPU. IMAGE_WIDTH_1/2 and IMAGE_HEIGHT_1/2 are the CTS's 10 and 15.
        struct SweepCase {
            GLenum mode;
            GLint value;
        };
        const SweepCase kSweep[] = {
            {GL_UNPACK_ROW_LENGTH, 0},     {GL_UNPACK_ROW_LENGTH, 10},   {GL_UNPACK_ROW_LENGTH, 15},
            {GL_UNPACK_SKIP_ROWS, 0},      {GL_UNPACK_SKIP_ROWS, 1},     {GL_UNPACK_SKIP_ROWS, 2},
            {GL_UNPACK_SKIP_PIXELS, 0},    {GL_UNPACK_SKIP_PIXELS, 1},   {GL_UNPACK_SKIP_PIXELS, 2},
            {GL_UNPACK_ALIGNMENT, 1},      {GL_UNPACK_ALIGNMENT, 2},     {GL_UNPACK_ALIGNMENT, 4},
            {GL_UNPACK_ALIGNMENT, 8},      {GL_UNPACK_IMAGE_HEIGHT, 0},  {GL_UNPACK_IMAGE_HEIGHT, 10},
            {GL_UNPACK_IMAGE_HEIGHT, 15},  {GL_UNPACK_SKIP_IMAGES, 0},   {GL_UNPACK_SKIP_IMAGES, 1},
            {GL_UNPACK_SKIP_IMAGES, 2},    {GL_PACK_ROW_LENGTH, 0},      {GL_PACK_ROW_LENGTH, 10},
            {GL_PACK_ROW_LENGTH, 15},      {GL_PACK_SKIP_ROWS, 0},       {GL_PACK_SKIP_ROWS, 1},
            {GL_PACK_SKIP_ROWS, 2},        {GL_PACK_SKIP_PIXELS, 0},     {GL_PACK_SKIP_PIXELS, 1},
            {GL_PACK_SKIP_PIXELS, 2},      {GL_PACK_ALIGNMENT, 1},       {GL_PACK_ALIGNMENT, 2},
            {GL_PACK_ALIGNMENT, 4},        {GL_PACK_ALIGNMENT, 8},
            // core-only, no ES equivalent
            {GL_UNPACK_SWAP_BYTES, GL_FALSE}, {GL_UNPACK_SWAP_BYTES, GL_TRUE},
            {GL_UNPACK_LSB_FIRST, GL_FALSE},  {GL_UNPACK_LSB_FIRST, GL_TRUE},
            {GL_PACK_SWAP_BYTES, GL_FALSE},   {GL_PACK_SWAP_BYTES, GL_TRUE},
            {GL_PACK_LSB_FIRST, GL_FALSE},    {GL_PACK_LSB_FIRST, GL_TRUE},
            {GL_PACK_IMAGE_HEIGHT, 0},        {GL_PACK_IMAGE_HEIGHT, 10},
            {GL_PACK_IMAGE_HEIGHT, 15},       {GL_PACK_SKIP_IMAGES, 0},
            {GL_PACK_SKIP_IMAGES, 1},         {GL_PACK_SKIP_IMAGES, 2},
        };

        std::size_t ImageBytes(int size) { return static_cast<std::size_t>(size) * size * 4; }

        // Padded to kScratchBytes so it is safe to hand to an upload running under any of the
        // sweep's stride/skip settings.
        std::vector<std::uint8_t> MakeGradient(int size, unsigned seed) {
            std::vector<std::uint8_t> pixels(kScratchBytes, 0);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const std::size_t base = (static_cast<std::size_t>(y) * size + x) * 4;
                    pixels[base + 0] = static_cast<std::uint8_t>((x * 11 + seed) & 0xFF);
                    pixels[base + 1] = static_cast<std::uint8_t>((y * 13 + seed) & 0xFF);
                    pixels[base + 2] = static_cast<std::uint8_t>((x * y + seed) & 0xFF);
                    pixels[base + 3] = 0xFF;
                }
            }
            return pixels;
        }

        void ResetAllPixelStoreModes() {
            for (const PixelStoreMode& mode : kAllModes) {
                glPixelStorei(mode.name, mode.defaultValue);
            }
        }

        // The one operation the CTS repeats: a fresh texture, a fresh framebuffer, one readback,
        // both deleted. Returns the readback; `outStatus` carries the completeness answer so a
        // caller can tell an incomplete framebuffer apart from wrong pixels.
        std::vector<std::uint8_t> UploadAndReadBack(const std::vector<std::uint8_t>& source, int size,
                                                    GLenum* outStatus) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, source.data());

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            *outStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

            std::vector<std::uint8_t> read(kScratchBytes, 0);
            if (*outStatus == GL_FRAMEBUFFER_COMPLETE) {
                glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &texture);
            return read;
        }

        // Index of the first differing byte within the image, or `bytes` when they agree.
        std::size_t FirstDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                                    std::size_t bytes) {
            for (std::size_t i = 0; i < bytes; ++i) {
                if (a[i] != b[i]) return i;
            }
            return bytes;
        }

        class PixelStoreSweepScenario : public ScenarioTest {};
        class FramebufferChurnScenario : public ScenarioTest {};

    } // namespace

    // Every mode in the CTS table is set, exercised and put back; the readback at default state
    // afterwards must be bit-identical to the one taken before the sweep. A mode that silently
    // fails to restore corrupts every later case in the batch, which is exactly how the CTS
    // failures presented (the FIRST sub-case, at default state, is what failed).
    TEST_F(PixelStoreSweepScenario, DefaultStateSurvivesTheFullModeSweep) {
        if (!Ready()) return;

        ResetAllPixelStoreModes();
        ASSERT_EQ(FirstGLError(), 0u) << "resetting the pixel-store modes must be legal on a GL 4.0 context";

        const std::vector<std::uint8_t> gradient = MakeGradient(kTexSize, 0);
        GLenum status = 0;
        const std::vector<std::uint8_t> baseline = UploadAndReadBack(gradient, kTexSize, &status);
        ASSERT_EQ(status, static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<std::uint8_t> scratchSource(kScratchBytes, 0x5A);

        for (const SweepCase& sweep : kSweep) {
            glPixelStorei(sweep.mode, sweep.value);
            ASSERT_EQ(FirstGLError(), 0u) << "glPixelStorei(0x" << std::hex << sweep.mode << std::dec << ", "
                                          << sweep.value << ") must be accepted";

            // Exercise the mode: an upload and a readback that both run with it in force.
            GLenum sweepStatus = 0;
            (void)UploadAndReadBack(scratchSource, kTexSize, &sweepStatus);

            ResetAllPixelStoreModes();

            GLenum afterStatus = 0;
            const std::vector<std::uint8_t> after = UploadAndReadBack(gradient, kTexSize, &afterStatus);
            ASSERT_EQ(afterStatus, static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const std::size_t diff = FirstDifference(baseline, after, ImageBytes(kTexSize));
            ASSERT_EQ(diff, ImageBytes(kTexSize))
                << "default-state readback changed after setting and resetting 0x" << std::hex << sweep.mode
                << std::dec << " = " << sweep.value << "; first differing byte " << diff << " (baseline "
                << static_cast<int>(baseline[diff]) << ", now " << static_cast<int>(after[diff]) << ")";
        }

        // And the modes themselves must read back as the defaults the reset asked for.
        for (const PixelStoreMode& mode : kAllModes) {
            GLint value = -1;
            glGetIntegerv(mode.name, &value);
            EXPECT_EQ(value, mode.defaultValue)
                << "pixel-store mode 0x" << std::hex << mode.name << std::dec << " did not return to its default";
        }
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(PixelStoreSweepScenario, ReadPixelsSwapsUnsignedShortComponentsAndPreservesPackGaps) {
        if (!Ready()) return;
        ResetAllPixelStoreModes();
        GLuint texture = 0, framebuffer = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        // RGBA8 expands to repeated bytes in a ushort and cannot expose a missing swap.
        const GLfloat color[] = {0.1f, 0.2f, 0.3f, 0.4f};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1, 0, GL_RGBA, GL_FLOAT, color);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        std::uint8_t native[8]{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT, native);
        ASSERT_NE(native[0], native[1]) << "the source must contain a non-symmetric ushort";
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        std::uint8_t tight[8]{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT, tight);
        glPixelStorei(GL_PACK_ROW_LENGTH, 3);
        glPixelStorei(GL_PACK_SKIP_ROWS, 1);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
        std::vector<std::uint8_t> scattered(64, 0xCD);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT, scattered.data());
        for (std::size_t i = 0; i < sizeof(native); ++i) {
            EXPECT_EQ(tight[i], native[i ^ 1]);
            EXPECT_EQ(scattered[32 + i], native[i ^ 1]);
        }
        for (std::size_t i = 0; i < scattered.size(); ++i) {
            if (i < 32 || i >= 40) EXPECT_EQ(scattered[i], 0xCD);
        }
        EXPECT_EQ(FirstGLError(), 0u);
        ResetAllPixelStoreModes();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
    }

    // THE ONE READBACK DESTINATION NOTHING COVERED (P3b/P4b espryt D1 slice 4): a bound
    // GL_PIXEL_PACK_BUFFER, with a non-default GL_PACK_* state, read back afterwards through
    // glMapBufferRange.
    //
    // The case above reads into a CLIENT POINTER, which under a transport is served by
    // ScatterTightReadbackIntoPackState - a pure function with unit cover of its own. A PBO
    // destination is a different half of the same seam and had none: the server reads with
    // NEUTRAL pack state into a tight w*h*bpp reply (ID-49), and the CLIENT then services the
    // PBO itself after the reply lands, uploading the tight rows one at a time at the stride the
    // application's pack state implies (EmitTables.cpp's `if (pbo)` branch). Nothing in the tree
    // ran that branch, and it is arithmetic - a stride, an alignment round-up, two skips and a
    // buffer offset - which is the kind of code that is either exactly right or quietly wrong.
    //
    // WHAT MAKES IT A TEST RATHER THAN A SMOKE CHECK is the poison. Every byte of the buffer
    // starts 0xCD; the rows land where the pack state says and NOTHING ELSE MOVES. A branch that
    // uploaded the reply whole, or ignored the buffer offset, or rounded the stride the other
    // way, writes over poison that this asserts is still there.
    //
    // The numbers are chosen so each term does something: ROW_LENGTH 5 at 4 bytes per pixel is
    // 20, which ALIGNMENT 8 rounds up to a 24-byte stride (a stride equal to the row length would
    // not tell a missing round-up from a present one); SKIP_ROWS 1 and SKIP_PIXELS 2 move the
    // first pixel to byte 24 + 8 = 32 of the image; and the read is issued at buffer offset 16,
    // so the first pixel lands at 48 and a branch that dropped the offset would land at 32.
    TEST_F(PixelStoreSweepScenario, ReadPixelsIntoAPackBufferHonoursPackStateAndLeavesTheGapsAlone) {
        if (!Ready()) return;
        ResetAllPixelStoreModes();

        constexpr int kW = 2;
        constexpr int kH = 2;
        constexpr GLint kAlignment = 8;
        constexpr GLint kRowLength = 5;
        constexpr GLint kSkipRows = 1;
        constexpr GLint kSkipPixels = 2;
        constexpr std::size_t kBpp = 4;
        constexpr std::size_t kOffset = 16;
        constexpr std::size_t kPboBytes = 256;
        constexpr std::uint8_t kPoison = 0xCD;
        // The same arithmetic GL defines and EmitTables.cpp implements, written out here rather
        // than borrowed, so the test cannot agree with the code by construction.
        constexpr std::size_t kStride = ((kRowLength * kBpp) + kAlignment - 1) / kAlignment * kAlignment;
        constexpr std::size_t kFirst = kOffset + kSkipRows * kStride + kSkipPixels * kBpp;
        static_assert(kStride == 24, "the alignment round-up must actually round");
        static_assert(kFirst == 48, "the skips and the buffer offset must all move the first pixel");

        // Four distinct opaque colours, so a row or a pixel landing in the wrong place is a
        // wrong VALUE and not just a wrong offset.
        const std::uint8_t texels[kW * kH * kBpp] = {
            0x11, 0x22, 0x33, 0xFF, 0x44, 0x55, 0x66, 0xFF,
            0x77, 0x88, 0x99, 0xFF, 0xAA, 0xBB, 0xCC, 0xFF,
        };

        GLuint texture = 0, framebuffer = 0, pbo = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<std::uint8_t> poison(kPboBytes, kPoison);
        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(kPboBytes), poison.data(), GL_STREAM_READ);
        ASSERT_EQ(FirstGLError(), 0u);

        glPixelStorei(GL_PACK_ALIGNMENT, kAlignment);
        glPixelStorei(GL_PACK_ROW_LENGTH, kRowLength);
        glPixelStorei(GL_PACK_SKIP_ROWS, kSkipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, kSkipPixels);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE,
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(kOffset)));
        EXPECT_EQ(FirstGLError(), 0u);
        ResetAllPixelStoreModes();

        std::vector<std::uint8_t> readback(kPboBytes, 0);
        const void* mapped =
            glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(kPboBytes), GL_MAP_READ_BIT);
        ASSERT_NE(mapped, nullptr) << "the pack buffer could not be mapped for readback";
        std::memcpy(readback.data(), mapped, kPboBytes);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

        // Every byte the pack state names carries its texel; every other byte is still poison.
        std::vector<bool> written(kPboBytes, false);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                for (std::size_t c = 0; c < kBpp; ++c) {
                    const std::size_t at = kFirst + static_cast<std::size_t>(y) * kStride +
                                           static_cast<std::size_t>(x) * kBpp + c;
                    ASSERT_LT(at, kPboBytes);
                    written[at] = true;
                    EXPECT_EQ(readback[at], texels[(y * kW + x) * kBpp + c])
                        << "pixel (" << x << ", " << y << ") component " << c << " at buffer byte " << at
                        << (readback[at] == kPoison ? " is still the poison: nothing was written here" : "");
                }
            }
        }
        for (std::size_t i = 0; i < kPboBytes; ++i) {
            if (written[i]) continue;
            EXPECT_EQ(readback[i], kPoison)
                << "buffer byte " << i << " is outside every row the pack state names and must not have moved";
        }

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteBuffers(1, &pbo);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
    }

    // The leak regression. Each iteration is one complete CTS inner step, and every readback has
    // to be exactly the gradient THIS iteration uploaded - never the previous one's. Before the
    // missing destructors were added, the driver-side framebuffer count grew without bound here.
    TEST_F(FramebufferChurnScenario, RepeatedFramebufferReadbackStaysExact) {
        if (!Ready()) return;

        ResetAllPixelStoreModes();
        constexpr int kSize = 8;
        constexpr int kIterations = 1024;

        for (int i = 0; i < kIterations; ++i) {
            // A distinct gradient per iteration: a stale attachment or a recycled driver name
            // reads back the PREVIOUS iteration's image, which a constant fill could not tell
            // apart from a correct read.
            const std::vector<std::uint8_t> gradient = MakeGradient(kSize, static_cast<unsigned>(i * 7 + 1));
            GLenum status = 0;
            const std::vector<std::uint8_t> read = UploadAndReadBack(gradient, kSize, &status);
            ASSERT_EQ(status, static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE)) << "iteration " << i;
            const std::size_t diff = FirstDifference(gradient, read, ImageBytes(kSize));
            ASSERT_EQ(diff, ImageBytes(kSize))
                << "iteration " << i << " read back a different image than it uploaded; first differing byte "
                << diff << " (uploaded " << static_cast<int>(gradient[diff]) << ", read "
                << static_cast<int>(read[diff]) << ")";
            ASSERT_EQ(FirstGLError(), 0u) << "iteration " << i;
        }
    }

} // namespace MGITest
