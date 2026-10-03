// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

namespace MobileGL::MG_Impl::EGLImpl {
    // Keep libglvnd routing and direct queries consistent. Device/GBM extensions
    // are absent until their entry points and image paths exist.
    inline constexpr const char* kDeviceExtensionString = "";
#if defined(__linux__) && !defined(__ANDROID__)
// GBM is claimed as a display platform only: a display on a gbm_device is an offscreen display
// like a surfaceless one (the frames come from the backend, not from the device), and window
// surfaces on it are refused. Chromium's Wayland GPU process hands its GL implementation nothing
// but a GBM display, so without the claim it never reaches this library at all.
#define MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS \
    " EGL_KHR_platform_wayland EGL_EXT_platform_wayland EGL_KHR_platform_gbm EGL_MESA_platform_gbm"
#else
#define MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS ""
#endif
    inline constexpr const char* kPlatformExtensionString =
        "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_base "
        "EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
    // EGL_EXT_create_context_robustness: a context may ask for EGL_LOSE_CONTEXT_ON_RESET, and a
    // split client's contexts are lost exactly when their server session is (a device loss on the
    // server ends that session). An application learns of it through glGetGraphicsResetStatus only
    // if it could ask for it here: ANGLE's GL-on-EGL backend mirrors this extension onto its own
    // display, and Chrome checks that display for it before it ever queries the reset status.
    inline constexpr const char* kDisplayExtensionString =
        "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_KHR_no_config_context EGL_EXT_platform_base "
        "EGL_EXT_create_context_robustness EGL_KHR_platform_base "
        "EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
    // The display list again, with the dma-buf import a backend with shared images has (a split
    // client: the server allocates the images and names the descriptors that come back). A
    // monolith has no allocator to recognise a dma-buf with, so it never advertises these. The
    // same server answers the buffer age of its surfaces and takes swap damage to its presents.
    inline constexpr const char* kDisplayExtensionStringWithSharedImages =
        "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_KHR_no_config_context EGL_EXT_platform_base "
        "EGL_EXT_create_context_robustness EGL_KHR_platform_base "
        "EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS
        " EGL_KHR_image_base EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers"
        " EGL_EXT_buffer_age EGL_KHR_partial_update EGL_KHR_swap_buffers_with_damage"
        " EGL_EXT_swap_buffers_with_damage";
    inline constexpr const char* kClientExtensionString =
        "EGL_EXT_client_extensions EGL_KHR_create_context EGL_EXT_platform_base "
        "EGL_KHR_platform_base EGL_MESA_platform_surfaceless" MOBILEGL_PLATFORM_WAYLAND_EXTENSIONS;
}
