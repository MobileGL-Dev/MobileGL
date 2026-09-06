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

#include <algorithm>
#include <string>
#include <vector>

#include "../Harness/BackendCapsPeek.h"
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
                // 84 is the GL 4.5 core table 23.64 minimum, and also the width of the state
                // layer's indexed-binding array - the two were made to coincide when the array
                // was widened from 36, which had made the clamp in GL_Getter degenerate.
                {GL_MAX_UNIFORM_BUFFER_BINDINGS, "GL_MAX_UNIFORM_BUFFER_BINDINGS", 84, 256},
                // 14 uniform blocks on each of the FIVE graphics stages. The sum used to count
                // three, and the two tessellation stages were simply missing from it.
                {GL_MAX_COMBINED_UNIFORM_BLOCKS, "GL_MAX_COMBINED_UNIFORM_BLOCKS", 70, 256},
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

            // THE MIDDLE TERM, which the relation quoted above always had and this case never
            // checked. It is the one that actually broke: widening the binding-point array to 84
            // raised what every PER-STAGE count clamps to, while the combined value was a
            // five-stage sum of 70 - so a device reporting descriptor-indexing-scale uniform
            // buffers (Adreno: maxPerStageDescriptorUniformBuffers = 16777216) advertised 84
            // compute uniform blocks inside a combined limit of 70. Per-stage <= combined is
            // exactly the assertion that says so, and it costs one glGetIntegerv per row.
            struct StageAgainstCombined {
                GLenum stage;
                const char* stageName;
                GLenum combined;
                const char* combinedName;
            };
            const StageAgainstCombined stageRelations[] = {
                {GL_MAX_COMPUTE_UNIFORM_BLOCKS, "GL_MAX_COMPUTE_UNIFORM_BLOCKS", GL_MAX_COMBINED_UNIFORM_BLOCKS,
                 "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_VERTEX_UNIFORM_BLOCKS, "GL_MAX_VERTEX_UNIFORM_BLOCKS", GL_MAX_COMBINED_UNIFORM_BLOCKS,
                 "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS, "GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS",
                 GL_MAX_COMBINED_UNIFORM_BLOCKS, "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS, "GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS",
                 GL_MAX_COMBINED_UNIFORM_BLOCKS, "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_GEOMETRY_UNIFORM_BLOCKS, "GL_MAX_GEOMETRY_UNIFORM_BLOCKS", GL_MAX_COMBINED_UNIFORM_BLOCKS,
                 "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_FRAGMENT_UNIFORM_BLOCKS, "GL_MAX_FRAGMENT_UNIFORM_BLOCKS", GL_MAX_COMBINED_UNIFORM_BLOCKS,
                 "GL_MAX_COMBINED_UNIFORM_BLOCKS"},
                {GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, "GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS",
                 GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, "GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS"},
                {GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, "GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS",
                 GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, "GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS"},
            };
            for (const StageAgainstCombined& relation : stageRelations) {
                GLint stage = -1;
                GLint combined = -1;
                glGetIntegerv(relation.stage, &stage);
                glGetIntegerv(relation.combined, &combined);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << relation.stageName;
                EXPECT_LE(stage, combined)
                    << relation.stageName << " = " << stage << " exceeds " << relation.combinedName << " = "
                    << combined << "; GL 4.6 table 23.64 orders MAX_*_BUFFER_BINDINGS >= MAX_COMBINED_*_BLOCKS >= "
                       "every per-stage count, and a single-stage program may use its whole per-stage allowance";
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

        // The GL 4.5 core minimums that had no case in the getter at all, or that were still
        // carrying an ES/GL3.3-tier number. Every one of these answered GL_INVALID_ENUM or a
        // too-small value against a context advertising 4.6, and each is the FIRST call its
        // conformance case makes - so the case died before it could measure anything.
        //
        // The cull pair is deliberately absent: zero is a legal answer there (a backend with no
        // cull-distance route MUST report it), so it is checked for answerability only, below.
        TEST_F(AdvertisedLimitsScenario, EveryGL45CoreMinimumIsMet) {
            const std::vector<LimitBound> table = {
                {GL_MAX_VARYING_VECTORS, "GL_MAX_VARYING_VECTORS", 15, 256},
                {GL_MAX_VERTEX_UNIFORM_VECTORS, "GL_MAX_VERTEX_UNIFORM_VECTORS", 256, 1 << 20},
                {GL_MAX_VARYING_COMPONENTS, "GL_MAX_VARYING_COMPONENTS", 60, 1 << 20},
                // GL_MAX_VERTEX_STREAMS is deliberately absent. GL 4.5 requires 4 and MobileGL
                // answers 1, which is a KNOWN non-conformance rather than an oversight: raising
                // the number un-gates two transform-feedback CTS cases per package across
                // KHR-GL40..GL46 that then fail, because no part of the shader pipeline supports
                // layout(stream = N). See the GL_MAX_VERTEX_STREAMS case in GL_Getter.cpp. Adding
                // a row here would pin a number the implementation cannot back.
                {GL_MAX_GEOMETRY_SHADER_INVOCATIONS, "GL_MAX_GEOMETRY_SHADER_INVOCATIONS", 32, 256},
                {GL_MAX_SUBROUTINES, "GL_MAX_SUBROUTINES", 256, 1 << 20},
                {GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS, "GL_MAX_SUBROUTINE_UNIFORM_LOCATIONS", 1024, 1 << 20},
                {GL_MAX_TESS_CONTROL_INPUT_COMPONENTS, "GL_MAX_TESS_CONTROL_INPUT_COMPONENTS", 128, 1 << 16},
                {GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS, "GL_MAX_TESS_CONTROL_OUTPUT_COMPONENTS", 128, 1 << 16},
                {GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS, "GL_MAX_TESS_CONTROL_TOTAL_OUTPUT_COMPONENTS", 4096,
                 1 << 20},
                {GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS, "GL_MAX_TESS_CONTROL_TEXTURE_IMAGE_UNITS", 16, 256},
                {GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS, "GL_MAX_TESS_CONTROL_UNIFORM_COMPONENTS", 1024, 1 << 20},
                {GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS, "GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS", 14, 256},
                {GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS, "GL_MAX_TESS_EVALUATION_INPUT_COMPONENTS", 128, 1 << 16},
                {GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS, "GL_MAX_TESS_EVALUATION_OUTPUT_COMPONENTS", 128, 1 << 16},
                {GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS, "GL_MAX_TESS_EVALUATION_TEXTURE_IMAGE_UNITS", 16, 256},
                {GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS, "GL_MAX_TESS_EVALUATION_UNIFORM_COMPONENTS", 1024,
                 1 << 20},
                {GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS, "GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS", 14, 256},
                {GL_MAX_TESS_PATCH_COMPONENTS, "GL_MAX_TESS_PATCH_COMPONENTS", 120, 1 << 16},
                {GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS, "GL_MAX_COMBINED_TESS_CONTROL_UNIFORM_COMPONENTS",
                 58368, 1 << 30},
                {GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS,
                 "GL_MAX_COMBINED_TESS_EVALUATION_UNIFORM_COMPONENTS", 58368, 1 << 30},
            };
            for (const LimitBound& bound : table) {
                GLint value = -424242;
                glGetIntegerv(bound.pname, &value);
                const unsigned int error = FirstGLError();
                EXPECT_EQ(error, GLenum(GL_NO_ERROR)) << bound.name << " is not answerable: " << GLErrorName(error);
                if (error != GL_NO_ERROR) continue;
                EXPECT_GE(value, bound.minimum) << bound.name << " = " << value << " is below the GL 4.5 minimum "
                                                << bound.minimum;
                EXPECT_LE(value, bound.ceiling) << bound.name << " = " << value << " exceeds the ceiling "
                                                << bound.ceiling;
            }

            // ARB_cull_distance's pair. Zero is honest on a backend with no cull-distance route,
            // so only answerability and the combined-limit ordering are checked here.
            GLint cull = -1;
            GLint clip = -1;
            GLint combined = -1;
            glGetIntegerv(GL_MAX_CULL_DISTANCES, &cull);
            glGetIntegerv(GL_MAX_CLIP_DISTANCES, &clip);
            glGetIntegerv(GL_MAX_COMBINED_CLIP_AND_CULL_DISTANCES, &combined);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the ARB_cull_distance queries must not error";
            EXPECT_GE(cull, 0);
            EXPECT_GE(combined, cull) << "GL 4.6 core 11.1.3.10: the combined limit is at least the cull one";
            EXPECT_GE(combined, clip) << "GL 4.6 core 11.1.3.10: the combined limit is at least the clip one";

            // GL_MAX_ELEMENT_INDEX is 64-bit state: the required 2^32-1 does not fit a GLint, so
            // the wide query must answer it and the narrow one must saturate rather than wrap.
            GLint64 elementIndex = -1;
            glGetInteger64v(GL_MAX_ELEMENT_INDEX, &elementIndex);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_GE(elementIndex, static_cast<GLint64>(4294967295LL))
                << "GL 4.5 core table 23.55 sets the GL_MAX_ELEMENT_INDEX minimum at 2^32-1";
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


        // THE INDEXED AND PER-PROGRAM QUERIES THAT NAME FRONTEND STATE, pinned on both lanes.
        //
        // Both backends used to carry their own arms for GL_SHADER_STORAGE_BUFFER_* and
        // GL_IMAGE_BINDING_* inside GLFunctionsTable::GetIntegeri_v, and their own
        // GetInteger64i_v / GetProgramiv table entries. None of it was reachable: GL_Getter and
        // GL_Program answer every one of these pnames from the frontend's own state and return
        // before the table is consulted. The duplicates did not even agree - the backend arms
        // clamped a bound range to the buffer's current storage, which GL 4.6 core tables
        // 23.4/23.5 do not permit - so the code was one refactor away from becoming the answer.
        // These cases pin what the frontend actually reports, so a future move of any of it back
        // behind the interface has to keep saying the same thing.
        TEST_F(AdvertisedLimitsScenario, IndexedBufferBindingsAreReportedVerbatimOnBothWidths) {
            GLuint buffer = 0;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glBufferData(GL_SHADER_STORAGE_BUFFER, 1024, nullptr, GL_DYNAMIC_DRAW);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            // A range that is NOT the whole buffer, so a clamp to the store would be visible.
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffer, 256, 512);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            GLint binding32 = -1;
            GLint start32 = -1;
            GLint size32 = -1;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &binding32);
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_START, 1, &start32);
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size32);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(binding32, static_cast<GLint>(buffer));
            EXPECT_EQ(start32, 256);
            EXPECT_EQ(size32, 512);

            // The 64-bit width has to agree pname for pname. It has no backend entry of its own
            // and derives everything from the 32-bit answer above plus its own buffer arm.
            GLint64 binding64 = -1;
            GLint64 start64 = -1;
            GLint64 size64 = -1;
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_BINDING, 1, &binding64);
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 1, &start64);
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &size64);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(binding64, static_cast<GLint64>(buffer));
            EXPECT_EQ(start64, static_cast<GLint64>(256));
            EXPECT_EQ(size64, static_cast<GLint64>(512));

            // An unbound index answers zero rather than erroring or leaking the driver's answer.
            GLint unbound = -1;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &unbound);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(unbound, 0);

            // THE ARM THAT SEPARATES VERBATIM FROM CLAMPED. GL 4.6 core tables 23.4/23.5 report
            // the size glBindBufferRange was ASKED for; it does not follow the buffer, so
            // shrinking the store underneath the binding must not move it. A clamp to the
            // current storage - which is exactly what both backends' deleted arms did - answers
            // 128 here, and answers 0 for the bind-then-allocate shape
            // KHR-GL43.shader_storage_buffer_object.basic-binding uses.
            glBufferData(GL_SHADER_STORAGE_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            GLint startAfterShrink = -1;
            GLint sizeAfterShrink = -1;
            GLint64 sizeAfterShrink64 = -1;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_START, 1, &startAfterShrink);
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &sizeAfterShrink);
            glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 1, &sizeAfterShrink64);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(startAfterShrink, 256)
                << "the bound range's start followed the buffer through a re-specification";
            EXPECT_EQ(sizeAfterShrink, 512)
                << "the bound range's size was clamped to the buffer's current 128-byte storage; the range is "
                   "state of the BINDING POINT and is reported verbatim";
            EXPECT_EQ(sizeAfterShrink64, static_cast<GLint64>(512))
                << "the 64-bit width disagreed with the 32-bit one about the same pname";

            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
            glDeleteBuffers(1, &buffer);
            (void)FirstGLError();
        }

        TEST_F(AdvertisedLimitsScenario, ImageUnitBindingsAreReportedFromTheFrontendState) {
            GLint maxImageUnits = 0;
            glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
            (void)FirstGLError();
            if (maxImageUnits < 2) GTEST_SKIP() << "no image units to bind on this lane";

            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 8, 8);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            glBindImageTexture(1, texture, 1, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            struct Expectation {
                GLenum pname;
                const char* name;
                GLint expected;
            };
            const Expectation expectations[] = {
                {GL_IMAGE_BINDING_NAME, "GL_IMAGE_BINDING_NAME", static_cast<GLint>(texture)},
                {GL_IMAGE_BINDING_LEVEL, "GL_IMAGE_BINDING_LEVEL", 1},
                {GL_IMAGE_BINDING_LAYERED, "GL_IMAGE_BINDING_LAYERED", GL_FALSE},
                {GL_IMAGE_BINDING_LAYER, "GL_IMAGE_BINDING_LAYER", 0},
                {GL_IMAGE_BINDING_ACCESS, "GL_IMAGE_BINDING_ACCESS", GL_READ_ONLY},
                {GL_IMAGE_BINDING_FORMAT, "GL_IMAGE_BINDING_FORMAT", GL_RGBA8},
            };
            for (const Expectation& expectation : expectations) {
                GLint value = -424242;
                glGetIntegeri_v(expectation.pname, 1, &value);
                EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << expectation.name;
                EXPECT_EQ(value, expectation.expected) << expectation.name;

                // Same pname through the wide width - it must not fall through to a driver that
                // knows nothing about MobileGL's image-unit state.
                GLint64 wide = -424242;
                glGetInteger64i_v(expectation.pname, 1, &wide);
                EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << expectation.name << " (64-bit)";
                EXPECT_EQ(wide, static_cast<GLint64>(expectation.expected)) << expectation.name << " (64-bit)";
            }

            glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
            glDeleteTextures(1, &texture);
            (void)FirstGLError();
        }

        // glGetProgramiv(GL_COMPUTE_WORK_GROUP_SIZE) is a LINK ARTIFACT of the program the
        // application wrote. DirectVulkan used to answer it from its own spirv-reflect cache and
        // DirectGLES by forwarding to the driver's ESSL program - neither of which the
        // application ever named - while GL_Program.cpp has always answered it from
        // ProgramObject::GetComputeLocalSize. This pins the declared local size on both lanes.
        TEST_F(AdvertisedLimitsScenario, ComputeLocalSizeComesFromTheLinkedProgram) {
            static const char* kSource = R"(#version 430 core
layout(local_size_x = 4, local_size_y = 3, local_size_z = 2) in;
layout(std430, binding = 0) buffer Output { uint g_data[]; };
void main() { g_data[gl_LocalInvocationIndex] = 1u; }
)";
            const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(shader, 1, &kSource, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_FALSE) {
                char log[2048] = {};
                glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                glDeleteShader(shader);
                (void)FirstGLError();
                GTEST_SKIP() << "no compute shader support on this lane: " << log;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, shader);
            glLinkProgram(program);
            glDeleteShader(shader);
            GLint linked = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked == GL_FALSE) {
                char log[2048] = {};
                glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                glDeleteProgram(program);
                (void)FirstGLError();
                GTEST_SKIP() << "the compute program did not link on this lane: " << log;
            }

            GLint localSize[3] = {-1, -1, -1};
            glGetProgramiv(program, GL_COMPUTE_WORK_GROUP_SIZE, localSize);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(localSize[0], 4);
            EXPECT_EQ(localSize[1], 3);
            EXPECT_EQ(localSize[2], 2);

            // A program with no compute stage must answer INVALID_OPERATION, not a stale or
            // defaulted (1, 1, 1) - the frontend's rule, and the one a backend that answers from
            // its own reflection cache cannot express.
            const GLuint empty = glCreateProgram();
            GLint ignored[3] = {0, 0, 0};
            glGetProgramiv(empty, GL_COMPUTE_WORK_GROUP_SIZE, ignored);
            EXPECT_EQ(FirstGLError(), GLenum(GL_INVALID_OPERATION))
                << "GL 4.6 core 7.13: the query is only defined for a linked program with a compute shader";

            glDeleteProgram(empty);
            glDeleteProgram(program);
            (void)FirstGLError();
        }

        // THE SIX COMPUTE LIMITS THAT OUTLIVE THE GETTER. GL_MAX_COMPUTE_WORK_GROUP_COUNT and
        // GL_MAX_COMPUTE_WORK_GROUP_SIZE, three axes each, are the only indexed pnames the
        // DEVICE answers rather than the frontend (glGetIntegeri_v on Espryt, VkPhysicalDevice-
        // Limits on Magma), and therefore the only ones that have to cross the MGPipe boundary
        // once GetIntegeri_v is retired (plan B section 4.4.6 / P0.5). They ride in MGPCaps by
        // inclusion, as DynamicBackendParameters::MaxComputeWorkGroupCount/Size, filled by both
        // backends at capability init. This case pins that the caps copy and the live getter
        // answer are one number - the getter floors the backend's raw answer at the GL 4.3
        // minimum, so the comparison is against the floored caps value - and pins the
        // GL-visible half on every lane: answerability, the floors, vector/indexed agreement
        // and the index bound. On a lane where the caps block is out of reach (Android links
        // the shipping .so) only the GL-visible half runs.
        TEST_F(AdvertisedLimitsScenario, ComputeWorkGroupLimitsAreTheCapsBlocksAnswer) {
            struct Axis {
                GLenum pname;
                const char* name;
                GLint minimum[3]; // GL 4.3 core table 23.60
            };
            const Axis axes[] = {
                {GL_MAX_COMPUTE_WORK_GROUP_COUNT, "GL_MAX_COMPUTE_WORK_GROUP_COUNT", {65535, 65535, 65535}},
                {GL_MAX_COMPUTE_WORK_GROUP_SIZE, "GL_MAX_COMPUTE_WORK_GROUP_SIZE", {1024, 1024, 64}},
            };
            int capsCount[3] = {0, 0, 0};
            int capsSize[3] = {0, 0, 0};
            const bool capsVisible = PeekComputeWorkGroupCaps(capsCount, capsSize);

            for (const Axis& axis : axes) {
                GLint indexed[3] = {-1, -1, -1};
                for (GLuint i = 0; i < 3; ++i) {
                    glGetIntegeri_v(axis.pname, i, &indexed[i]);
                    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << axis.name << "[" << i << "]";
                    EXPECT_GE(indexed[i], axis.minimum[i])
                        << axis.name << "[" << i << "] = " << indexed[i]
                        << " is below the GL 4.3 core table 23.60 minimum " << axis.minimum[i];
                }
                GLint vector[3] = {-1, -1, -1};
                glGetIntegerv(axis.pname, vector);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << axis.name;
                for (int i = 0; i < 3; ++i) {
                    EXPECT_EQ(vector[i], indexed[i])
                        << axis.name << "[" << i << "]: the vector query and the indexed query disagree";
                }
                GLint outOfRange = -424242;
                glGetIntegeri_v(axis.pname, 3, &outOfRange);
                EXPECT_EQ(FirstGLError(), GLenum(GL_INVALID_VALUE))
                    << axis.name << "[3]: an index past the three axes is INVALID_VALUE (GL 4.6 core 22.1)";

                if (!capsVisible) continue;
                const int* capsAxis = axis.pname == GL_MAX_COMPUTE_WORK_GROUP_COUNT ? capsCount : capsSize;
                for (int i = 0; i < 3; ++i) {
                    EXPECT_EQ(std::max(capsAxis[i], axis.minimum[i]), indexed[i])
                        << axis.name << "[" << i << "]: MGPCaps carries " << capsAxis[i]
                        << " but glGetIntegeri_v answers " << indexed[i]
                        << " - the caps block and the getter path must be one number, because P0.5 retires "
                           "the getter in favour of the caps";
                }
            }
        }

    } // namespace
} // namespace MGITest
