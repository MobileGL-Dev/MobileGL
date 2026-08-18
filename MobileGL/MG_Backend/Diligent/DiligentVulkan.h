// MobileGL - MobileGL/MG_Backend/Diligent/DiligentVulkan.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#pragma once

#include <Includes.h>

namespace MobileGL::MG_Backend::DiligentBackend {
    // Backend identity string used by the backend object and local smoke tests.
    inline String GetDiligentVulkanBackendName() {
        return "DiligentVulkan";
    }
} // namespace MobileGL::MG_Backend::DiligentBackend
