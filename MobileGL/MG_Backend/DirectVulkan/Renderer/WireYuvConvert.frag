// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireYuvConvert.frag
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
// The image is bound through an immutable sampler that carries its VkSamplerYcbcrConversion, so
// a plain sample already returns RGB.
layout(set = 0, binding = 0) uniform sampler2D yuvImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(textureLod(yuvImage, texCoord, 0.0).rgb, 1.0);
}
