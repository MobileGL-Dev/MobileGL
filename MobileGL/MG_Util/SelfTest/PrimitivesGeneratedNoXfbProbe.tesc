// MobileGL - MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.tesc
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Tessellation control stage of the PATCHES variant of the
// primitives-generated-without-transform-feedback probe. Every level is 1, so with
// the evaluation stage's triangles domain the tessellator emits exactly one
// triangle per patch - the expected count the probe checks the queries against.
#version 450

layout(vertices = 1) out;

void main() {
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelOuter[3] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_TessLevelInner[1] = 1.0;
}
