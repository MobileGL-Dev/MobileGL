// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlit.frag
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // Source framebuffer clipping preserves the original scale, rather than
    // extending the edge texels into pixels outside the source framebuffer.
    if (any(lessThan(texCoord, vec2(0.0))) || any(greaterThan(texCoord, vec2(1.0)))) discard;
    // Explicit LOD is required on Adreno 650: implicit LOD through a rotated
    // single-mip UBWC render target can read outside its allocation.
    outColor = textureLod(sourceImage, texCoord, 0.0);
}
