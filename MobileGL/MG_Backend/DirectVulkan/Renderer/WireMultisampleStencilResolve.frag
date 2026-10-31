// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleStencilResolve.frag
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
// The stencil half, and it needs an extension to exist at all: a fragment shader cannot write
// the stencil aspect without VK_EXT_shader_stencil_export (SPV_EXT_shader_stencil_export, which
// this `#extension` line is how glslang is asked for). CONTRACT-P7 §3.2 says so and says what
// happens otherwise - a stencil-only resolve DECLINES on a device without it, because there is
// no second way to put a value in the stencil aspect from a pass.
//
// usampler2DMS and not sampler2DMS: the stencil aspect of a depth/stencil image is an unsigned
// integer, and a float view of it would round the index.
//
// Sample zero, for WireMultisampleDepthResolve.frag's reason, and GL is stricter here: 18.3.1
// says integer and stencil resolves SELECT one sample rather than combining them, so sample 0
// is the answer and not merely an acceptable one.
#extension GL_ARB_shader_stencil_export : require
layout(set = 0, binding = 0) uniform usampler2DMS sourceImage;

void main() {
    gl_FragStencilRefARB = int(texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0).r);
}
