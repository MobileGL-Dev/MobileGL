// MobileGL - MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.vert
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Vertex stage of the primitives-generated-without-transform-feedback probe
// (PrimitivesGeneratedNoXfbProbe.cpp), used by both triangle shapes (with and
// without rasterizer discard) and as the tessellation shapes' vertex stage.
// Deliberately carries NO Xfb execution mode: the probe's whole subject is what
// the transform-feedback stream query answers for a pipeline that captures
// nothing. Positions are distinct (a full-viewport triangle per three vertices)
// so no driver can excuse the primitive as degenerate before it reaches
// primitive assembly.
#version 450

void main() {
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex % 3], 0.0, 1.0);
}
