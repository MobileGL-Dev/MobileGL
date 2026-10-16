// MobileGL - MobileGL/MG_Backend/DirectVulkan/DirectVulkanResourceState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>

namespace MobileGL::MG_State::GLState {
    class ProgramObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend::DirectVulkan {
    GLuint GetShaderStorageBlockIndex(const MG_State::GLState::ProgramObject& program, const String& name);
    GLuint GetShaderStorageBlockBinding(const MG_State::GLState::ProgramObject& program, GLuint blockIndex);
} // namespace MobileGL::MG_Backend::DirectVulkan
