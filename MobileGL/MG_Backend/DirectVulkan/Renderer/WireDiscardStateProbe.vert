// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDiscardStateProbe.vert
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
// The discard state probe's draws (WireDiscardStateProbe.h): vertices 0-3 are a full-viewport strip,
// vertices 4-7 the same strip far outside the clip volume (the control's first draw).
void main() {
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    const vec2 offset = gl_VertexIndex >= 4 ? vec2(8.0) : vec2(0.0);
    gl_Position = vec4(corners[gl_VertexIndex & 3] + offset, 0.0, 1.0);
}
