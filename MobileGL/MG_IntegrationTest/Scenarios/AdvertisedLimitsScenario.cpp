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

        // A per-stage block count is an amount of BINDING POINTS an application will use, so it
        // can never exceed the number of binding points that exist. GL 4.6 Table 23.64 states the
        // relation the other way round (MAX_UNIFORM_BUFFER_BINDINGS >= MAX_COMBINED_UNIFORM_BLOCKS
        // >= every per-stage count), and DirectVulkan broke it by clamping the two families
        // independently: a device reporting 256 compute uniform blocks and 84 uniform binding
        // points passes both ceilings and still cannot serve
        // KHR-GL44.multi_bind.dispatch_bind_buffers_base, which reads the block count and binds
        // that many buffers in one glBindBuffersBase - INVALID_OPERATION before a single bind.
        TEST_F(AdvertisedLimitsScenario, PerStageBlockCountsFitInTheirBindingPoints) {
            struct Relation {
                GLenum blocks;
                const char* blocksName;
                GLenum bindings;
                const char* bindingsName;
            };
            const Relation relations[] = {
                {GL_MAX_COMPUTE_UNIFORM_BLOCKS, "GL_MAX_COMPUTE_UNIFORM_BLOCKS", GL_MAX_UNIFORM_BUFFER_BINDINGS,
                 "GL_MAX_UNIFORM_BUFFER_BINDINGS"},
                {GL_MAX_VERTEX_UNIFORM_BLOCKS, "GL_MAX_VERTEX_UNIFORM_BLOCKS", GL_MAX_UNIFORM_BUFFER_BINDINGS,
                 "GL_MAX_UNIFORM_BUFFER_BINDINGS"},
                {GL_MAX_FRAGMENT_UNIFORM_BLOCKS, "GL_MAX_FRAGMENT_UNIFORM_BLOCKS", GL_MAX_UNIFORM_BUFFER_BINDINGS,
                 "GL_MAX_UNIFORM_BUFFER_BINDINGS"},
                {GL_MAX_COMBINED_UNIFORM_BLOCKS, "GL_MAX_COMBINED_UNIFORM_BLOCKS", GL_MAX_UNIFORM_BUFFER_BINDINGS,
                 "GL_MAX_UNIFORM_BUFFER_BINDINGS"},
                {GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, "GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS",
                 GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS"},
                {GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, "GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS",
                 GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS"},
            };
            for (const Relation& relation : relations) {
                GLint blocks = -1;
                GLint bindings = -1;
                glGetIntegerv(relation.blocks, &blocks);
                glGetIntegerv(relation.bindings, &bindings);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << relation.blocksName;
                EXPECT_LE(blocks, bindings)
                    << relation.blocksName << " = " << blocks << " exceeds " << relation.bindingsName << " = "
                    << bindings << "; a shader may declare more blocks than there are binding points to bind them to";
            }
        }

        // KHR-GL44.multi_bind.functional_bind_buffers_range sizes each of an indexed target's
        // binding points at MAX_<target>_SIZE / MAX_<target>_BINDINGS and binds all of them in
        // one glBindBuffersRange. That quotient has to be a legal BindBufferRange size, which
        // makes the two limits of every indexed family a PAIR: advertise a size that does not
        // survive division by the binding count and the call fails with INVALID_VALUE before any
        // of it binds.
        TEST_F(AdvertisedLimitsScenario, IndexedTargetSizeSurvivesDivisionByItsBindingCount) {
            struct IndexedFamily {
                GLenum maxSize;
                const char* maxSizeName;
                GLenum maxBindings;
                const char* maxBindingsName;
                GLint sizeGranularity; // BindBufferRange's size rule for the target
            };
            const IndexedFamily families[] = {
                {GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE, "GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE",
                 GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, "GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS", 1},
                {GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, "GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS",
                 GL_MAX_TRANSFORM_FEEDBACK_BUFFERS, "GL_MAX_TRANSFORM_FEEDBACK_BUFFERS", 4},
                {GL_MAX_UNIFORM_BLOCK_SIZE, "GL_MAX_UNIFORM_BLOCK_SIZE", GL_MAX_UNIFORM_BUFFER_BINDINGS,
                 "GL_MAX_UNIFORM_BUFFER_BINDINGS", 1},
                {GL_MAX_SHADER_STORAGE_BLOCK_SIZE, "GL_MAX_SHADER_STORAGE_BLOCK_SIZE",
                 GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS", 1},
            };
            for (const IndexedFamily& family : families) {
                GLint maxSize = -1;
                GLint maxBindings = -1;
                glGetIntegerv(family.maxSize, &maxSize);
                glGetIntegerv(family.maxBindings, &maxBindings);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << family.maxSizeName;
                ASSERT_GT(maxBindings, 0) << family.maxBindingsName;
                const GLint perBinding = maxSize / maxBindings;
                EXPECT_GT(perBinding, 0)
                    << family.maxSizeName << " (" << maxSize << ") / " << family.maxBindingsName << " ("
                    << maxBindings << ") is zero, and BindBufferRange rejects a zero size";
                EXPECT_EQ(perBinding % family.sizeGranularity, 0)
                    << family.maxSizeName << " (" << maxSize << ") / " << family.maxBindingsName << " ("
                    << maxBindings << ") = " << perBinding << " is not a multiple of the "
                    << family.sizeGranularity << "-byte size granularity BindBufferRange requires for it";
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

        // ARB_viewport_array's own limits. They are advertised from three different places -
        // GL_MAX_VIEWPORTS from the frontend's indexed state width, the bounds range and the
        // subpixel bits from the backend caps table - and each backend fills that table from a
        // different source, so all three are checked on both lanes.
        //
        // GL_VIEWPORT_BOUNDS_RANGE is the one that shipped wrong: GLES has no such query, the
        // DirectGLES loader's glGetFloatv(GL_VIEWPORT_BOUNDS_RANGE) therefore raised
        // GL_INVALID_ENUM and left the probe's zero-initialized array in place, and MobileGL
        // advertised [0, 0] - a range that admits no viewport origin at all, and the check that
        // kept KHR-GL43.viewport_array.queries red on Espryt after the indexed-state work.
        TEST_F(AdvertisedLimitsScenario, ViewportArrayLimitsMeetTheirGL43Floors) {
            GLint maxViewports = -1;
            glGetIntegerv(GL_MAX_VIEWPORTS, &maxViewports);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_GE(maxViewports, 16) << "GL 4.3 core table 23.53 sets the MAX_VIEWPORTS minimum at 16";
            EXPECT_LE(maxViewports, 256) << "one viewport rectangle of indexed state is allocated per advertised "
                                            "viewport, and the CTS sizes its arrays off this number";

            GLfloat boundsRange[2] = {1.0f, -1.0f};
            glGetFloatv(GL_VIEWPORT_BOUNDS_RANGE, boundsRange);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_LE(boundsRange[0], -32768.0f)
                << "GL 4.6 core table 23.60 sets the VIEWPORT_BOUNDS_RANGE minimum at [-32768, 32767]; got ["
                << boundsRange[0] << ", " << boundsRange[1] << "]";
            EXPECT_GE(boundsRange[1], 32767.0f)
                << "GL 4.6 core table 23.60 sets the VIEWPORT_BOUNDS_RANGE minimum at [-32768, 32767]; got ["
                << boundsRange[0] << ", " << boundsRange[1] << "]";

            // KNOWN INFIDELITY, pinned here rather than hidden. MobileGL reports the driver's own
            // VIEWPORT_SUBPIXEL_BITS (4 on llvmpipe, i.e. 1/16-pixel viewport precision), but the
            // float viewport rectangle glViewportIndexedf stores is snapped to integers on its
            // way to both backends (ComputeGLViewport, DirectGLES SyncRenderState). The STATE
            // round trip is exact - which is all KHR-GL43.viewport_array.viewport_api checks, and
            // all this cluster set out to fix - so the gap is in rasterization only: a fractional
            // viewport origin rasterizes as if it had been rounded. Nothing in the suite or in
            // Minecraft sets one. Only the spec floor is asserted; tightening this to EQ(0) would
            // mean advertising no subpixel precision at all, which is a separate decision about a
            // limit MobileGL currently passes through from the driver.
            GLint subpixelBits = -1;
            glGetIntegerv(GL_VIEWPORT_SUBPIXEL_BITS, &subpixelBits);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_GE(subpixelBits, 0) << "GL 4.6 core table 23.60: VIEWPORT_SUBPIXEL_BITS has a minimum of 0, and "
                                          "a negative value is what a sign-flipped uint32 looks like";

            GLint viewportDims[2] = {-1, -1};
            glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewportDims);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            GLint maxRenderbufferSize = -1;
            glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbufferSize);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            // GL 4.6 core 13.6.1: MAX_VIEWPORT_DIMS must be at least as large as the largest
            // renderable surface, or a full-size framebuffer could not be fully viewported.
            EXPECT_GE(viewportDims[0], maxRenderbufferSize);
            EXPECT_GE(viewportDims[1], maxRenderbufferSize);
        }

    } // namespace
} // namespace MGITest
