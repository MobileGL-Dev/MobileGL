// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PackBufferReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P9 (W1, MG_Remote/CONTRACT-P9.md §1): READS INTO A PIXEL PACK BUFFER ARE ANSWERED BY NOTHING.
//
// Under a transport a glReadPixels / glGetTexImage whose destination is a bound
// GL_PIXEL_PACK_BUFFER no longer waits for its pixels: it crosses as read_pixels_to_buffer /
// get_texture_image_to_buffer, the server lands the rows in the buffer at the client's PACK
// layout, and the client marks the buffer GPU-written so that the next CPU read of it - a
// glGetBufferSubData, a glMapBufferRange, or a texture upload that uses it as the UNPACK source -
// fetches the bytes through resource_readback.
//
// THE CASES ARE ORDINARY GL AND RUN ON THE MONOLITH ARMS TOO. Each one compares the pack buffer
// against the same read into client memory under the same pack state, both destinations
// pre-filled with the same byte, so the padding the pack state skips has to survive as well: the
// monolith and every split arm must agree with themselves, and therefore with each other. The
// one exception is the last case, which is about the WIRE (the read posted no reply and marked
// the buffer) and skips where there is no wire to ask.
//
// Every image is POSITION-DEPENDENT, so a row landed at the wrong stride, a band at the wrong
// offset or a byte swapped the wrong way cannot look right.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr GLubyte kFill = 0xAB;

        // A texel that names its own position (and its layer), so nothing misplaced can match.
        std::array<GLubyte, 4> PatternRgba8(int x, int y, int layer) {
            return {static_cast<GLubyte>(x * 7 + layer * 31), static_cast<GLubyte>(y * 11 + layer),
                    static_cast<GLubyte>((x ^ y) + layer * 5), static_cast<GLubyte>(0x80 | (x + y))};
        }

        // Integers below 2^24 in every component, so float storage is exact and a byte swap of
        // any of them is visibly different from the value.
        std::array<GLfloat, 4> PatternRgba32f(int x, int y) {
            return {static_cast<GLfloat>(x + 1), static_cast<GLfloat>(y + 3),
                    static_cast<GLfloat>(x * 64 + y), static_cast<GLfloat>(1000 + x * y)};
        }

        struct PackState {
            const char* name;
            GLint alignment;
            GLint rowLength;
            GLint skipPixels;
            GLint skipRows;
            GLboolean swapBytes;
        };

        // Neutral, then each PACK parameter that changes the layout, then all of them at once.
        constexpr PackState kPackStates[] = {
            {"neutral", 4, 0, 0, 0, GL_FALSE},
            {"alignment 1", 1, 0, 0, 0, GL_FALSE},
            {"alignment 8", 8, 0, 0, 0, GL_FALSE},
            {"row length 53", 4, 53, 0, 0, GL_FALSE},
            {"skip pixels 3, skip rows 2", 4, 0, 3, 2, GL_FALSE},
            {"everything", 8, 47, 5, 3, GL_FALSE},
        };

        class PackBufferReadbackScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glDisable(GL_SCISSOR_TEST);
                ResetPack();
            }

            void TearDown() override {
                if (!Ready()) return;
                ResetPack();
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                for (GLuint buffer : m_buffers) glDeleteBuffers(1, &buffer);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                for (GLuint texture : m_textures) glDeleteTextures(1, &texture);
                m_buffers.clear();
                m_textures.clear();
                m_fbo = 0;
                DrainErrors();
                ScenarioTest::TearDown();
            }

            static void ResetPack() {
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
                glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
                glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }

            static void ApplyPack(const PackState& pack) {
                glPixelStorei(GL_PACK_ALIGNMENT, pack.alignment);
                glPixelStorei(GL_PACK_ROW_LENGTH, pack.rowLength);
                glPixelStorei(GL_PACK_SKIP_PIXELS, pack.skipPixels);
                glPixelStorei(GL_PACK_SKIP_ROWS, pack.skipRows);
                glPixelStorei(GL_PACK_SWAP_BYTES, pack.swapBytes);
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            // The bytes a pack state makes a width x height read occupy, from the first byte the
            // read may touch (GL 4.6 §8.4.4.1 / §18.2): the skips, then the padded rows, the last
            // row tight.
            static std::size_t PackedExtent(const PackState& pack, int width, int height, std::size_t bpp) {
                const std::size_t rowPixels = pack.rowLength > 0 ? static_cast<std::size_t>(pack.rowLength)
                                                                 : static_cast<std::size_t>(width);
                const std::size_t align = static_cast<std::size_t>(pack.alignment);
                const std::size_t stride = (rowPixels * bpp + align - 1) / align * align;
                return static_cast<std::size_t>(pack.skipRows) * stride + static_cast<std::size_t>(pack.skipPixels) * bpp +
                       static_cast<std::size_t>(height - 1) * stride + static_cast<std::size_t>(width) * bpp;
            }

            GLuint NewTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            GLuint NewBuffer(GLenum target, std::size_t size, GLubyte fill) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                m_buffers.push_back(buffer);
                glBindBuffer(target, buffer);
                const std::vector<GLubyte> initial(size, fill);
                glBufferData(target, static_cast<GLsizeiptr>(size), initial.data(), GL_DYNAMIC_READ);
                return buffer;
            }

            // An RGBA8 texture holding the pattern, attached as COLOR_ATTACHMENT0 for reading.
            GLuint AttachPatternRgba8(int width, int height) {
                std::vector<GLubyte> texels(static_cast<std::size_t>(width) * height * 4);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const auto t = PatternRgba8(x, y, 0);
                        std::memcpy(texels.data() + (static_cast<std::size_t>(y) * width + x) * 4, t.data(), 4);
                    }
                const GLuint texture = NewTexture();
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
                EXPECT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "pattern upload";
                return texture;
            }

            // glReadPixels of (x, y, width, height) under `pack` into client memory and into a
            // pack buffer at `offset`, both pre-filled with kFill; the buffer's bytes are then
            // read back by `readBack` and the two must be identical, byte for byte, padding
            // included - and the bytes in front of `offset` must still be kFill.
            template <typename ReadBack>
            void ExpectPackBufferMatchesClientMemory(const PackState& pack, int x, int y, int width, int height,
                                                     GLenum type, std::size_t bpp, ReadBack&& readBack) {
                ApplyPack(pack);
                const std::size_t extent = PackedExtent(pack, width, height, bpp);
                std::vector<GLubyte> host(extent, kFill);
                glReadPixels(x, y, width, height, GL_RGBA, type, host.data());
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << pack.name << ": client-memory read";

                constexpr std::size_t kOffset = 64;
                NewBuffer(GL_PIXEL_PACK_BUFFER, kOffset + extent, kFill);
                glReadPixels(x, y, width, height, GL_RGBA, type, reinterpret_cast<void*>(kOffset));
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << pack.name << ": pack-buffer read";
                std::vector<GLubyte> actual(kOffset + extent, 0);
                readBack(actual);
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << pack.name << ": buffer read-back";
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                ResetPack();

                std::size_t lead = 0;
                for (std::size_t i = 0; i < kOffset; ++i) lead += actual[i] != kFill ? 1 : 0;
                EXPECT_EQ(lead, 0u) << pack.name << ": " << lead << " bytes in front of the read's offset moved";
                std::size_t bad = 0, first = 0;
                for (std::size_t i = 0; i < extent; ++i) {
                    if (actual[kOffset + i] != host[i] && bad++ == 0) first = i;
                }
                EXPECT_EQ(bad, 0u) << pack.name << ": " << bad << " of " << extent
                                   << " bytes differ from the client-memory read, first at byte " << first
                                   << " (buffer " << static_cast<int>(actual[kOffset + first]) << ", host "
                                   << static_cast<int>(host[first]) << ")";
            }

            GLuint m_fbo = 0;
            std::vector<GLuint> m_buffers;
            std::vector<GLuint> m_textures;
        };

        TEST_F(PackBufferReadbackScenario, ReadPixelsIntoAPackBufferMatchesClientMemoryUnderEveryPackState) {
            if (!Ready()) return;
            AttachPatternRgba8(37, 23);
            if (HasFatalFailure()) return;
            for (const PackState& pack : kPackStates) {
                ExpectPackBufferMatchesClientMemory(pack, 2, 1, 31, 19, GL_UNSIGNED_BYTE, 4, [](std::vector<GLubyte>& out) {
                    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(out.size()), out.data());
                });
                if (HasFatalFailure()) return;
            }
        }

        // The map is the OTHER CPU reader, and the one an application that streams readbacks
        // through a PBO actually uses: glMapBufferRange(GL_MAP_READ_BIT) must see the pixels the
        // server landed, not the bytes the client shadow held before the read.
        TEST_F(PackBufferReadbackScenario, MapBufferRangeAfterAPackBufferReadSeesThePixels) {
            if (!Ready()) return;
            AttachPatternRgba8(37, 23);
            if (HasFatalFailure()) return;
            for (const PackState& pack : {kPackStates[0], kPackStates[5]}) {
                ExpectPackBufferMatchesClientMemory(pack, 0, 0, 37, 23, GL_UNSIGNED_BYTE, 4, [](std::vector<GLubyte>& out) {
                    void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(out.size()),
                                                    GL_MAP_READ_BIT);
                    ASSERT_NE(mapped, nullptr);
                    std::memcpy(out.data(), mapped, out.size());
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                });
                if (HasFatalFailure()) return;
            }
        }

        // GL_PACK_SWAP_BYTES reverses every component's bytes; the client carries the group
        // width and the server swaps, so a wrong width or a skipped swap is a different float.
        TEST_F(PackBufferReadbackScenario, SwapBytesIntoAPackBufferMatchesClientMemory) {
            if (!Ready()) return;
            constexpr int kWidth = 13, kHeight = 9;
            std::vector<GLfloat> texels(static_cast<std::size_t>(kWidth) * kHeight * 4);
            for (int y = 0; y < kHeight; ++y)
                for (int x = 0; x < kWidth; ++x) {
                    const auto t = PatternRgba32f(x, y);
                    std::memcpy(texels.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4, t.data(), sizeof(t));
                }
            const GLuint texture = NewTexture();
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, kWidth, kHeight);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, texels.data());
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const PackState swapped{"swap bytes, row length 17", 4, 17, 1, 1, GL_TRUE};
            ExpectPackBufferMatchesClientMemory(swapped, 0, 0, kWidth, kHeight, GL_FLOAT, 16, [](std::vector<GLubyte>& out) {
                glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(out.size()), out.data());
            });
        }

        // glGetTexImage into a pack buffer, 2D and layered: the layered case is the one whose
        // layout has an image stride (GL_PACK_IMAGE_HEIGHT) and a skip in the third dimension.
        TEST_F(PackBufferReadbackScenario, GetTexImageIntoAPackBufferMatchesClientMemory) {
            if (!Ready()) return;
            constexpr int kWidth = 11, kHeight = 7, kLayers = 3;
            std::vector<GLubyte> texels(static_cast<std::size_t>(kWidth) * kHeight * kLayers * 4);
            for (int layer = 0; layer < kLayers; ++layer)
                for (int y = 0; y < kHeight; ++y)
                    for (int x = 0; x < kWidth; ++x) {
                        const auto t = PatternRgba8(x, y, layer);
                        std::memcpy(texels.data() + ((static_cast<std::size_t>(layer) * kHeight + y) * kWidth + x) * 4,
                                    t.data(), 4);
                    }
            const GLuint flat = NewTexture();
            glBindTexture(GL_TEXTURE_2D, flat);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kWidth, kHeight);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
            const GLuint layered = NewTexture();
            glBindTexture(GL_TEXTURE_2D_ARRAY, layered);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kWidth, kHeight, kLayers);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kWidth, kHeight, kLayers, GL_RGBA, GL_UNSIGNED_BYTE,
                            texels.data());
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture upload";

            struct Shape {
                const char* name;
                GLenum target;
                GLuint texture;
                int depth;
                GLint rowLength, alignment, imageHeight, skipImages;
            };
            const Shape shapes[] = {
                {"2D, row length 13, alignment 8", GL_TEXTURE_2D, flat, 1, 13, 8, 0, 0},
                {"2D array, image height 9, skip images 1", GL_TEXTURE_2D_ARRAY, layered, kLayers, 0, 4, 9, 1},
            };
            for (const Shape& shape : shapes) {
                glBindTexture(shape.target, shape.texture);
                glPixelStorei(GL_PACK_ROW_LENGTH, shape.rowLength);
                glPixelStorei(GL_PACK_ALIGNMENT, shape.alignment);
                glPixelStorei(GL_PACK_IMAGE_HEIGHT, shape.imageHeight);
                glPixelStorei(GL_PACK_SKIP_IMAGES, shape.skipImages);
                const std::size_t rowPixels = shape.rowLength > 0 ? static_cast<std::size_t>(shape.rowLength) : kWidth;
                const std::size_t stride = (rowPixels * 4 + shape.alignment - 1) / shape.alignment * shape.alignment;
                const std::size_t imageRows = shape.imageHeight > 0 ? static_cast<std::size_t>(shape.imageHeight) : kHeight;
                const std::size_t imageStride = stride * imageRows;
                const std::size_t extent = static_cast<std::size_t>(shape.skipImages) * imageStride +
                                           static_cast<std::size_t>(shape.depth - 1) * imageStride +
                                           static_cast<std::size_t>(kHeight - 1) * stride + kWidth * 4u;
                std::vector<GLubyte> host(extent, kFill);
                glGetTexImage(shape.target, 0, GL_RGBA, GL_UNSIGNED_BYTE, host.data());
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << shape.name << ": client-memory read";

                constexpr std::size_t kOffset = 32;
                NewBuffer(GL_PIXEL_PACK_BUFFER, kOffset + extent, kFill);
                glGetTexImage(shape.target, 0, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(kOffset));
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << shape.name << ": pack-buffer read";
                std::vector<GLubyte> actual(kOffset + extent, 0);
                glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(actual.size()), actual.data());
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                ResetPack();
                std::size_t bad = 0, first = 0;
                for (std::size_t i = 0; i < kOffset; ++i) bad += actual[i] != kFill ? 1 : 0;
                EXPECT_EQ(bad, 0u) << shape.name << ": bytes in front of the offset moved";
                bad = 0;
                for (std::size_t i = 0; i < extent; ++i)
                    if (actual[kOffset + i] != host[i] && bad++ == 0) first = i;
                EXPECT_EQ(bad, 0u) << shape.name << ": " << bad << " of " << extent
                                   << " bytes differ from the client-memory read, first at byte " << first;
            }
        }

        // THE UNPACK HALF. A read into a buffer followed by an upload FROM that buffer is a
        // GPU-to-GPU copy an application expects to work; under a transport the upload reads the
        // client's shadow of the buffer, which the pack read did not touch, so this is exactly
        // the case that goes wrong if the upload path forgets that the buffer was GPU-written.
        TEST_F(PackBufferReadbackScenario, APackBufferReadFeedsAnUploadFromTheSameBuffer) {
            if (!Ready()) return;
            constexpr int kWidth = 29, kHeight = 17;
            AttachPatternRgba8(kWidth, kHeight);
            if (HasFatalFailure()) return;
            const GLuint buffer = NewBuffer(GL_PIXEL_PACK_BUFFER, static_cast<std::size_t>(kWidth) * kHeight * 4, kFill);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            const GLuint copy = NewTexture();
            glBindTexture(GL_TEXTURE_2D, copy);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kWidth, kHeight);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "read into the buffer, then upload from it";

            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy, 0);
            std::vector<GLubyte> actual(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
            std::size_t bad = 0;
            std::string first;
            for (int y = 0; y < kHeight; ++y)
                for (int x = 0; x < kWidth; ++x) {
                    const auto expected = PatternRgba8(x, y, 0);
                    if (std::memcmp(actual.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4, expected.data(), 4) != 0 &&
                        bad++ == 0)
                        first = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
                }
            EXPECT_EQ(bad, 0u) << bad << " texels of the uploaded copy differ from the pattern, first at " << first;
        }

        // ORDER AGAINST LATER WRITES. A glBufferSubData into part of the buffer AFTER the read
        // must win over the read's bytes there and leave the rest of the read alone - on the
        // server the pixels and the later write are two queued ranges of one store, drained in
        // record order.
        TEST_F(PackBufferReadbackScenario, ALaterBufferSubDataWinsOverThePackReadWhereTheyOverlap) {
            if (!Ready()) return;
            constexpr int kWidth = 16, kHeight = 8;
            AttachPatternRgba8(kWidth, kHeight);
            if (HasFatalFailure()) return;
            const std::size_t size = static_cast<std::size_t>(kWidth) * kHeight * 4;
            std::vector<GLubyte> host(size, kFill);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, host.data());
            NewBuffer(GL_PIXEL_PACK_BUFFER, size, kFill);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const std::vector<GLubyte> overwrite(40, 0x5A);
            glBufferSubData(GL_PIXEL_PACK_BUFFER, 100, static_cast<GLsizeiptr>(overwrite.size()), overwrite.data());
            std::vector<GLubyte> actual(size, 0);
            glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), actual.data());
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
            std::memcpy(host.data() + 100, overwrite.data(), overwrite.size());
            std::size_t bad = 0, first = 0;
            for (std::size_t i = 0; i < size; ++i)
                if (actual[i] != host[i] && bad++ == 0) first = i;
            EXPECT_EQ(bad, 0u) << bad << " bytes differ, first at " << first;
        }

        // THE WIRE HALF, and the one case that is about the transport rather than the pixels:
        // under a transport a pack-buffer read posts NO reply-slot record and marks the buffer
        // GPU-written, once per read. MOBILEGL_IPC_PBO_READBACK_SYNC=1 puts the reply form back,
        // and this case must then fail on both counters - that is its red-once, recorded in
        // docs/Disaggregated/notes/p9/.
        TEST_F(PackBufferReadbackScenario, APackBufferReadPostsNoReplyAndMarksTheBuffer) {
            if (!Ready()) return;
            const std::string split = SplitRuntimeSkipReason();
            if (!split.empty()) GTEST_SKIP() << split;
            constexpr int kWidth = 24, kHeight = 12;
            AttachPatternRgba8(kWidth, kHeight);
            if (HasFatalFailure()) return;
            NewBuffer(GL_PIXEL_PACK_BUFFER, static_cast<std::size_t>(kWidth) * kHeight * 4, kFill);
            // A warm-up read, so every state record the read's fill has to publish once is out
            // of the way before the counted window opens.
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
            constexpr int kReads = 4;
            const SplitRuntimeState before = PeekSplitRuntime();
            for (int i = 0; i < kReads; ++i) glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const SplitRuntimeState after = PeekSplitRuntime();
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(after.replyPostings - before.replyPostings, 0u)
                << "a read into a pack buffer published a reply-slot record: it took the reply form "
                   "(MOBILEGL_IPC_PBO_READBACK_SYNC=1 does this on purpose)";
            EXPECT_EQ(after.packBufferReadbackMarks - before.packBufferReadbackMarks,
                      static_cast<unsigned long long>(kReads + 1))
                << "every pack-buffer read marks its buffer GPU-written exactly once";
            // And the bytes are still right after all of that.
            std::vector<GLubyte> actual(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
            glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(actual.size()), actual.data());
            const auto corner = PatternRgba8(kWidth - 1, kHeight - 1, 0);
            EXPECT_EQ(std::memcmp(actual.data() + actual.size() - 4, corner.data(), 4), 0);
        }

    } // namespace
} // namespace MGITest
