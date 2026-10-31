// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDiscardStateProbe.frag
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#version 450
layout(location = 0) out vec4 color;

void main() {
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
