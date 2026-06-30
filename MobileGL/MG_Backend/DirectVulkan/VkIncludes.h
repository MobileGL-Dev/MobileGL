// MobileGL - MobileGL/MG_Backend/DirectVulkan/VkIncludes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "VulkanRendererConfig.h"

#define ENUM_STR_CASE(c) case c: return #c;

namespace MobileGL::MG_Backend::DirectVulkan {
    inline const char* VkResultToString(VkResult result) {
        switch (result) {
        ENUM_STR_CASE(VK_SUCCESS)
        ENUM_STR_CASE(VK_NOT_READY)
        ENUM_STR_CASE(VK_TIMEOUT)
        ENUM_STR_CASE(VK_EVENT_SET)
        ENUM_STR_CASE(VK_EVENT_RESET)
        ENUM_STR_CASE(VK_INCOMPLETE)
        ENUM_STR_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        ENUM_STR_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        ENUM_STR_CASE(VK_ERROR_INITIALIZATION_FAILED)
        ENUM_STR_CASE(VK_ERROR_DEVICE_LOST)
        ENUM_STR_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        ENUM_STR_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        ENUM_STR_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        ENUM_STR_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        ENUM_STR_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        ENUM_STR_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        ENUM_STR_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        ENUM_STR_CASE(VK_ERROR_FRAGMENTED_POOL)
        ENUM_STR_CASE(VK_ERROR_UNKNOWN)
        ENUM_STR_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        ENUM_STR_CASE(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        ENUM_STR_CASE(VK_ERROR_FRAGMENTATION)
        ENUM_STR_CASE(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
        ENUM_STR_CASE(VK_PIPELINE_COMPILE_REQUIRED)
        ENUM_STR_CASE(VK_ERROR_SURFACE_LOST_KHR)
        ENUM_STR_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        ENUM_STR_CASE(VK_SUBOPTIMAL_KHR)
        ENUM_STR_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        ENUM_STR_CASE(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR)
        ENUM_STR_CASE(VK_ERROR_VALIDATION_FAILED_EXT)
        ENUM_STR_CASE(VK_ERROR_INVALID_SHADER_NV)
        default:
            return "VK_RESULT_UNKNOWN";
        }
    }
} // namespace MobileGL::MG_Backend::DirectVulkan

#define VK_VERIFY(expr, ...)                                                                                           \
    do {                                                                                                               \
        VkResult _vk_verify_result = (expr);                                                                           \
        if (_vk_verify_result != VK_SUCCESS) {                                                                         \
            MGLOG_F("Vulkan error %s (%d) at %s:%d" __VA_OPT__(" - ") __VA_ARGS__,                                  \
                    MobileGL::MG_Backend::DirectVulkan::VkResultToString(_vk_verify_result),                          \
                    _vk_verify_result, __FILE__, __LINE__);                                                            \
        }                                                                                                              \
        MOBILEGL_ASSERT(_vk_verify_result == VK_SUCCESS, "Vulkan error %s (%d) at %s:%d" __VA_OPT__(" - ") __VA_ARGS__, MobileGL::MG_Backend::DirectVulkan::VkResultToString(_vk_verify_result), _vk_verify_result, __FILE__, __LINE__);  \
    } while (0)

#define XXHASH_VERIFY(expr, ...)                                                                                       \
    do {                                                                                                               \
        XXH_errorcode _xxh_verify_result = (expr);                                                                     \
        MOBILEGL_ASSERT(_xxh_verify_result == XXH_OK, "XXHash error %d at %s:%d" __VA_OPT__(" - ") __VA_ARGS__, _xxh_verify_result, __FILE__, __LINE__);  \
    } while (0)
