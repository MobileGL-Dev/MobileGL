// MobileGL - MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.tese
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Tessellation evaluation stage of the PATCHES variant of the
// primitives-generated-without-transform-feedback probe. Triangles domain: with the
// control stage's all-1 levels the tessellator emits exactly one triangle per
// patch. Like the vertex stage, it deliberately carries no Xfb execution mode.
#version 450

layout(triangles, equal_spacing, cw) in;

void main() {
    gl_Position = vec4(gl_TessCoord.xy * 2.0 - 1.0, 0.0, 1.0);
}
