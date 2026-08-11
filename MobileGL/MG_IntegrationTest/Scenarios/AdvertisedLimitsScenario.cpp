// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/AdvertisedLimitsScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// "The limit we advertise is a promise, and an application will hold us to it."
//
// DirectVulkan copied Vulkan descriptor limits straight into the GL limit table. Those are not
// the same quantity: Adreno answers maxPerStageDescriptorUniformBuffers at descriptor-indexing
// scale, and GL_MAX_COMPUTE_UNIFORM_BLOCKS is a count an app will allocate. KHR-GL44.multi_bind
// .dispatch_bind_buffers_base does exactly that - createsO(limit) buffers and splices O(limit)
// UBO declarations into one compute shader - and spent ~14 s allocating before dying on
// std::bad_alloc. Its sibling dispatch_bind_buffers_range hard-codes 4 buffers and passes.
//
// Two failure modes, one table:
//   - too LARGE: an unusable promise (the OOM above).
//   - too SMALL or negative: a uint32 limit that lost its top bit on the way to a signed Int -
//     UINT32_MAX arrived as -1, which every downstream std::min then accepted as "small enough".
//     A conformant GL 4.x implementation may never advertise below the spec minimum either.
//
// Every bound below is checked on BOTH backends, because the loader casts are shared and the
// DirectGLES lane is the control: it takes its limits from a driver that already reports GL
// quantities, so an entry that only fails on DirectVulkan is a translation bug and one that
// fails on both is a table bug.

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

        struct LimitBound {
            GLenum pname;
            const char* name;
            // The GL 4.x required minimum. A value below this is a conformance failure in its own
            // right, and is what a sign-flipped uint32 looks like.
            int minimum;
            // The largest value this implementation is willing to promise. Chosen well above every
            // desktop driver's answer, so it can only catch a descriptor-scale number.
            int ceiling;
        };

        const std::vector<LimitBound>& BufferLimitTable() {
            static const std::vector<LimitBound> table = {
                {GL_MAX_UNIFORM_BUFFER_BINDINGS, "GL_MAX_UNIFORM_BUFFER_BINDINGS", 36, 256},
                {GL_MAX_COMPUTE_UNIFORM_BLOCKS, "GL_MAX_COMPUTE_UNIFORM_BLOCKS", 12, 256},
                {GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, "GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS", 8, 256},
                {GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, "GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS", 8, 256},
                {GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS", 8, 256},
                {GL_MAX_TEXTURE_BUFFER_SIZE, "GL_MAX_TEXTURE_BUFFER_SIZE", 65536, 1 << 27},
                {GL_MAX_UNIFORM_BLOCK_SIZE, "GL_MAX_UNIFORM_BLOCK_SIZE", 16384, 1 << 30},
                // Already clamped before this campaign; in the table so a regression there is
                // caught by the same case.
                {GL_MAX_SHADER_STORAGE_BLOCK_SIZE, "GL_MAX_SHADER_STORAGE_BLOCK_SIZE", 1 << 24, 512 * 1024 * 1024},
                {GL_MAX_TEXTURE_IMAGE_UNITS, "GL_MAX_TEXTURE_IMAGE_UNITS", 16, 32},
                {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS", 48, 192},
            };
            return table;
        }

        class AdvertisedLimitsScenario : public ScenarioTest {};

        TEST_F(AdvertisedLimitsScenario, EveryBufferLimitIsWithinItsAdvertisedRange) {
            for (const LimitBound& bound : BufferLimitTable()) {
                GLint value = -424242;
                glGetIntegerv(bound.pname, &value);
                const unsigned int error = FirstGLError();
                EXPECT_EQ(error, GLenum(GL_NO_ERROR))
                    << bound.name << " is not answerable: " << GLErrorName(error);
                if (error != GL_NO_ERROR) continue;

                EXPECT_GE(value, bound.minimum)
                    << bound.name << " = " << value << " is below the GL required minimum "
                    << bound.minimum << " (a negative or tiny value here is a uint32 limit that lost "
                                        "its top bit on the way to a signed Int)";
                EXPECT_LE(value, bound.ceiling)
                    << bound.name << " = " << value << " exceeds the ceiling " << bound.ceiling
                    << " this implementation is willing to promise - an application that allocates "
                       "what we advertise will run out of memory";
            }
        }

        // The OOM case in isolation, because it is the one with a known CTS victim and the one a
        // future refactor is most likely to reintroduce by copying the Vulkan limit back.
        TEST_F(AdvertisedLimitsScenario, ComputeUniformBlocksIsAnAmountAnApplicationCouldActuallyAllocate) {
            GLint blocks = -1;
            glGetIntegerv(GL_MAX_COMPUTE_UNIFORM_BLOCKS, &blocks);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_GE(blocks, 12);
            EXPECT_LE(blocks, 256) << "KHR-GL44.multi_bind.dispatch_bind_buffers_base creates one GL buffer "
                                      "and one UBO declaration per advertised block";

            GLint blockSize = -1;
            glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &blockSize);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_GT(blockSize, 0);
            // GL_MAX_COMBINED_COMPUTE_UNIFORM_COMPONENTS is derived from the product of these two,
            // so their product has to stay representable.
            EXPECT_LE(static_cast<long long>(blocks) * blockSize,
                      static_cast<long long>(2147483647))
                << "blocks(" << blocks << ") * blockSize(" << blockSize << ") overflows the GLint the "
                   "derived component limits are computed in";
        }

    } // namespace
} // namespace MGITest
