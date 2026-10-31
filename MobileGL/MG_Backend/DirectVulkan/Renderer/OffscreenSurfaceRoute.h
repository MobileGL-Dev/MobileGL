// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/OffscreenSurfaceRoute.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// How an offscreen (pbuffer) surface target gets its VkSurfaceKHR on Android, decided from what the
// INSTANCE has, not from what the loader could offer. A renderer created for a window enabled
// VK_KHR_android_surface and nothing headless, so its later offscreen targets (a second client
// surface, or the placeholder a suspended server window draws into) must take the AImageReader
// route; asking that instance for vkCreateHeadlessSurfaceEXT answers null. Pure, so a host case pins
// it without a driver.

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {

    enum class OffscreenSurfaceRoute : Uint8 {
        Headless,           // vkCreateHeadlessSurfaceEXT
        AndroidImageReader, // vkCreateAndroidSurfaceKHR on an AImageReader's window nobody displays
        Unavailable,        // neither: the target cannot be built (the caller refuses, by name)
    };

    // `headlessEnabled`: the instance was created with VK_EXT_headless_surface.
    // `headlessEntryPoint`: vkGetInstanceProcAddr answered vkCreateHeadlessSurfaceEXT.
    // `androidSurfaceEnabled`: the instance was created with VK_KHR_android_surface.
    constexpr OffscreenSurfaceRoute ChooseOffscreenSurfaceRoute(Bool headlessEnabled, Bool headlessEntryPoint,
                                                                Bool androidSurfaceEnabled) {
        if (headlessEnabled && headlessEntryPoint) return OffscreenSurfaceRoute::Headless;
        if (androidSurfaceEnabled) return OffscreenSurfaceRoute::AndroidImageReader;
        return OffscreenSurfaceRoute::Unavailable;
    }

} // namespace MobileGL::MG_Backend::DirectVulkan
