// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLPlatformExtensions.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

namespace MobileGL::MG_Impl::EGLImpl {
    // THE PLATFORMS THIS IMPLEMENTATION SERVES, in the words EGL spells them.
    //
    // Two callers need exactly the same list, and they disagree if it is written twice:
    // eglQueryString(EGL_EXTENSIONS) is what an application reads BEFORE it decides to call
    // eglGetPlatformDisplay at all - libepoxy refuses to use eglGetPlatformDisplayEXT unless the
    // string names EGL_EXT_platform_base - and the glvnd vendor string is what the system's
    // libEGL reads to decide which vendor library a platform request should be routed to.  A
    // vendor that does not claim the platform is never asked for it, and the caller is told there
    // is no provider: the library can be loaded, initialized, and still never called.
    //
    // GBM is on the list because a compositor asks for its display on the GBM device it owns.
    // This side treats that device the way it treats every other native display - the frames come
    // from the display host, not from the device - so the claim is exact rather than optimistic.
    inline constexpr const char* kDeviceExtensionString =
        "EGL_EXT_device_base "
        "EGL_EXT_device_enumeration "
        "EGL_EXT_device_query "
        "EGL_EXT_device_drm "
        "EGL_EXT_device_drm_render_node";

    inline constexpr const char* kPlatformExtensionString =
        "EGL_EXT_platform_base "
        "EGL_KHR_platform_base "
        "EGL_KHR_platform_gbm "
        "EGL_EXT_platform_gbm "
        "EGL_MESA_platform_gbm "
        "EGL_EXT_platform_device "
        "EGL_MESA_platform_surfaceless";

    // What a display answers for EGL_EXTENSIONS: the extensions this implementation provides that
    // are not platform ones, then the platform list above.
    inline constexpr const char* kDisplayExtensionString =
        "EGL_KHR_create_context "
        "EGL_EXT_device_base "
        "EGL_EXT_device_enumeration "
        "EGL_EXT_device_query "
        "EGL_EXT_device_drm "
        "EGL_EXT_device_drm_render_node "
        "EGL_EXT_platform_device "
        "EGL_EXT_platform_base "
        "EGL_KHR_platform_base "
        "EGL_KHR_platform_gbm "
        "EGL_EXT_platform_gbm "
        "EGL_MESA_platform_gbm "
        "EGL_MESA_platform_surfaceless";

    // ... and what EGL_NO_DISPLAY answers: the same, plus the client-extension marker that tells a
    // caller the no-display query is meaningful here.
    inline constexpr const char* kClientExtensionString =
        "EGL_EXT_client_extensions "
        "EGL_KHR_create_context "
        "EGL_EXT_device_base "
        "EGL_EXT_device_enumeration "
        "EGL_EXT_device_query "
        "EGL_EXT_device_drm "
        "EGL_EXT_device_drm_render_node "
        "EGL_EXT_platform_device "
        "EGL_EXT_platform_base "
        "EGL_KHR_platform_base "
        "EGL_KHR_platform_gbm "
        "EGL_EXT_platform_gbm "
        "EGL_MESA_platform_gbm "
        "EGL_MESA_platform_surfaceless";
}
