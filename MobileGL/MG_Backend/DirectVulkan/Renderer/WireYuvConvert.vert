// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireYuvConvert.vert
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
// A YUV shared image converted into an RGBA texture (WireYuvImage.inc): one triangle over the
// whole target, its first row at the image's first row.
layout(location = 0) out vec2 texCoord;

void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = corners[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);
    texCoord = (p + 1.0) * 0.5;
}
