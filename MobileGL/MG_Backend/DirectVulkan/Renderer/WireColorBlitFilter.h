// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlitFilter.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
#pragma once

#include <Includes.h>
#include "../VkIncludes.h"

// P7 g3-blit (g3det, gate 3): WHICH SAMPLER THE WIRE ARM'S SHADER COLOUR BLIT READS ITS SOURCE
// THROUGH.
//
// GL applies a blit's `filter` only where the image is STRETCHED (GL 4.6 core 18.3.1). A blit whose
// source and destination rectangles are the same size is a copy, whatever `filter` says, and so is
// a mirrored one. The wire arm's shader blit (WireColorBlit.inc, BlitWireColorImage) draws a quad
// and samples `textureLod(sourceImage, texCoord, 0)` at interpolated coordinates, and it used the
// LINEAR sampler for every GL_LINEAR blit. At 1:1 every fragment centre maps to a texel centre, so
// in exact arithmetic linear filtering reads the same single texel. Adreno 830 quantises the
// coordinate to sub-texel precision: a fragment that lands a fraction off the centre blends in a
// small weight of a neighbour. minecraft-1.21.1-neoforge-create-instancing-in-world ends with a
// 1:1 GL_LINEAR glBlitFramebuffer into FB 0, and on the wire arm 712 high-contrast pixels moved by
// 1 LSB toward a neighbour. The monolith's vkCmdBlitImage copied them exactly
// (~/w7/logs/g3det/VERDICT.md section 3). NEAREST within half a texel of the centre is exact, so an
// unscaled blit takes NEAREST. Lavapipe samples texel centres exactly, so no host picture can see
// the difference; this pure half is what the unit test pins
// (MG_Test/Pipeline/PipelineQuirkTest.cpp, WireColorBlitSamples*).
//
// THE SURFACE TRANSFORM DOES NOT TURN A COPY INTO A STRETCH. BlitWireColorImage applies the
// quarter and half turns in NDC, over a viewport of the native image, and for a quarter turn it
// takes the logical extent as the native extent swapped. A native pixel then has exactly the pitch
// of a logical one, and the rotation maps pixel centres onto pixel centres. So the scale test is on
// GL's own (logical) rectangles for identity and all three rotations. BlitWireColorImage declines
// any other transform before it chooses a sampler; for one, this answers with the filter's own
// mapping, as before this package.
#if MOBILEGL_BUILD_DISAGGREGATED
namespace MobileGL::MG_Backend::DirectVulkan {
    // A glBlitFramebuffer rectangle as GL names it: two corners, either order.
    struct WireBlitRect {
        GLint x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    // Same extent on both axes, orientation aside. The extents are taken in 64 bits: GL accepts any
    // GLint corner, and INT_MAX - INT_MIN does not fit in one.
    inline Bool WireBlitIsUnscaled(const WireBlitRect& source, const WireBlitRect& destination) {
        const auto extent = [](GLint a, GLint b) {
            return std::abs(static_cast<Int64>(b) - static_cast<Int64>(a));
        };
        return extent(source.x0, source.x1) == extent(destination.x0, destination.x1) &&
               extent(source.y0, source.y1) == extent(destination.y0, destination.y1);
    }

    inline VkFilter WireColorBlitFilterFor(const WireBlitRect& source, const WireBlitRect& destination,
                                           GLenum filter, VkSurfaceTransformFlagBitsKHR transform) {
        if (filter != GL_LINEAR) return VK_FILTER_NEAREST;
        const Bool expressed = transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
                               transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                               transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ||
                               transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        if (expressed && WireBlitIsUnscaled(source, destination)) return VK_FILTER_NEAREST;
        return VK_FILTER_LINEAR;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
#endif
