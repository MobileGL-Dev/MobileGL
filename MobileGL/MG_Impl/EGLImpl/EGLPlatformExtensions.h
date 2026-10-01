// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

namespace MobileGL::MG_Impl::EGLImpl {
    // Keep libglvnd routing and direct queries consistent. Device/GBM extensions
    // are absent until their entry points and image paths exist.
    inline constexpr const char* kDeviceExtensionString = "";
#if defined(__linux__) && !defined(__ANDROID__)
#define MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS " EGL_KHR_platform_wayland EGL_EXT_platform_wayland"
#else
#define MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS ""
#endif
    inline constexpr const char* kPlatformExtensionString =
        "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_base "
        "EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
    inline constexpr const char* kDisplayExtensionString =
        "EGL_KHR_create_context EGL_EXT_platform_base EGL_KHR_platform_base "
        "EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
    inline constexpr const char* kClientExtensionString =
        "EGL_EXT_client_extensions EGL_KHR_create_context EGL_EXT_platform_base "
        "EGL_KHR_platform_base EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
}
