// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ClampMultisampleFetchPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // GL 4.6 core table 23.53 requires GL_MAX_SAMPLES >= 4, so MobileGL floors every
            // multisample ceiling it advertises to 4 (GL_Getter's kFrontendMaxSamples) no matter
            // what the ES driver reports. The realised allocation cannot be floored the same way -
            // the driver would simply reject it - so DirectGLES clamps the count it passes to
            // glTexStorage*Multisample down to what the format really supports
            // (ClampSamplesToBackendSupport). On Adreno and on Mali's Immortalis-G925 that is ONE
            // sample for every integer format, while the frontend keeps telling the application
            // GL_MAX_INTEGER_SAMPLES is 4.
            //
            // A shader written against the advertised ceiling therefore fetches a sample the
            // backing storage does not have. KHR-GL33/40/41.texture_swizzle.functional_* and
            // KHR-GLxx.texture_size_promotion.functional bake `texelFetch(usampler2DMS, coord, 3)`
            // in as a literal, and the fetch comes back as 0/garbage ("Found pixel with wrong
            // value", "read value = 0") on a texture the backend quietly allocated with one
            // sample.
            //
            // This pass closes that gap from the shader side: for every fetch of a multisampled
            // image it clamps the Sample image-operand to the backend's REAL maximum for that
            // image's category, so the lookup lands inside the allocation the backend made.
            //   - K >= advertisedMaxSamples: the category is not squeezed, nothing is rewritten.
            //   - K <= 1: the Sample operand becomes a constant 0 of its own type - the only
            //     sample that exists.
            //   - 1 < K < advertisedMaxSamples: the operand is wrapped in min(operand, K-1),
            //     which leaves an in-range index exactly as it was.
            // Only the UPPER bound is clamped. A negative index is out of range in GL before this
            // pass and after it alike, and MobileGL is not the component that should be inventing
            // a value for it.
            //
            // Category comes from the OpTypeImage: an OpTypeInt sampled type is the integer
            // class (GL_MAX_INTEGER_SAMPLES), a float one is depth when the image's Depth operand
            // is exactly 1 and colour otherwise. That last clause is deliberate: glslang writes
            // Depth 0 for a plain sampler and 2 ("unknown") wherever it cannot tell, and GLSL has
            // no multisampled shadow sampler at all, so only an explicit 1 is treated as a depth
            // image and everything else falls to the colour limit - which is the one a
            // mis-classified image would want anyway.
            //
            // DirectGLES transpile path only. DirectVulkan allocates the sample count it was
            // asked for and must see the module unchanged.
            class ClampMultisampleFetchPass : public spvtools::opt::Pass {
            public:
                // The three backend-REAL per-category ceilings, plus the count the GL frontend
                // advertises (GL_Getter's GetAdvertisedMaxSamples). A category whose real ceiling
                // already reaches the advertised one is left completely alone.
                ClampMultisampleFetchPass(Int32 maxColorSamples, Int32 maxIntegerSamples,
                                          Int32 maxDepthSamples, Int32 advertisedMaxSamples)
                    : m_maxColorSamples(maxColorSamples),
                      m_maxIntegerSamples(maxIntegerSamples),
                      m_maxDepthSamples(maxDepthSamples),
                      m_advertisedMaxSamples(advertisedMaxSamples) {}

                const char* name() const override { return "clamp-multisample-fetch"; }
                Status Process() override;

                // Whether the module declares any multisampled image type, i.e. whether running
                // this pass could change anything. Answered from a single parse so the caller can
                // skip the optimizer round trip entirely - which is every shader but the handful
                // that read a multisample texture directly.
                static bool DeclaresMultisampledImage(const Vector<Uint32>& binary);

                static spvtools::Optimizer::PassToken CreateClampMultisampleFetchPass(
                    Int32 maxColorSamples, Int32 maxIntegerSamples, Int32 maxDepthSamples,
                    Int32 advertisedMaxSamples);

            private:
                Int32 m_maxColorSamples;
                Int32 m_maxIntegerSamples;
                Int32 m_maxDepthSamples;
                Int32 m_advertisedMaxSamples;
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
