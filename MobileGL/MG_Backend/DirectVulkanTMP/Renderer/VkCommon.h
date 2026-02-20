// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/VkCommon.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    inline void VkCheck(VkResult result, const char* msg) {
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return;
        MGLOG_E("Vulkan error %d at %s", static_cast<Int>(result), msg ? msg : "(unknown)");
        throw RuntimeError(msg ? msg : "Vulkan error");
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager

#ifndef VK_VERIFY
#define VK_VERIFY(res, msg) ::MobileGL::MG_Backend::DirectVulkanTMP::VkManager::VkCheck((res), (msg))
#endif
