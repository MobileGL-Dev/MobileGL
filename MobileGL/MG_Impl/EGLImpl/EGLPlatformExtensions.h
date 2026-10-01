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

    // EGL_EXT_client_extensions IS ON THE PLATFORM LIST FOR A MEASURED REASON. libglvnd only folds
    // a vendor client-extension string (the answer to eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS))
    // into its own no-display answer when the vendor PLATFORM string - the one __egl_Main publishes
    // for the platform-extensions query - names EGL_EXT_client_extensions. Without it the client
    // string never reached libepoxy, so libepoxy concluded EGL_EXT_device_query was absent and left
    // epoxy_eglQueryDisplayAttribEXT as a NULL pointer, while kwin (which reads the display string,
    // where the extension IS present) called it: measured as signal 11, fault_addr=(nil), pc=0x0,
    // from KWin::EglDisplay::determineRenderNode() through the GOT slot whose relocation is
    // R_AARCH64_GLOB_DAT epoxy_eglQueryDisplayAttribEXT.
    inline constexpr const char* kPlatformExtensionString =
        "EGL_EXT_client_extensions "
        "EGL_EXT_platform_base "
        "EGL_KHR_platform_base "
        "EGL_KHR_platform_gbm "
        "EGL_EXT_platform_gbm "
        "EGL_MESA_platform_gbm "
        "EGL_EXT_platform_device "
        "EGL_MESA_platform_surfaceless";

    // What a display answers for EGL_EXTENSIONS: the extensions this implementation provides that
    // are not platform ones, then the platform list above.
    // EGL_KHR_no_config_context and EGL_KHR_surfaceless_context are on the display and client lists
    // because both are answered by this implementation rather than announced by a driver: a context
    // is created with EGL_NO_CONFIG_KHR and made current with EGL_NO_SURFACE, which is the pair a
    // compositor asks for when it renders into framebuffers of its own.  Measured on kwin_wayland
    // against this vendor before they were here: createContext() refused with
    // "EGL_KHR_no_config_context extension is unsupported" and the session ended at
    // "no usable DRM render device; cannot bring up OpenGL compositing".
    //
    // EGL_KHR_image_base AND EGL_EXT_image_dma_buf_import ARE ONE FEATURE HERE, and they are on the
    // list for a measured reason rather than a plausible one.  In the Fedora container that runs
    // this library as the glvnd EGL vendor, eglGetProcAddress("eglCreateImageKHR") and
    // eglGetProcAddress("eglDestroyImageKHR") both returned NULL, and kwin_wayland 6.7.3's anland
    // backend imports the display daemon's dma-bufs through exactly those two
    // (AnlandEglLayer::importBuffers -> EglBackend::importDmaBufAsTexture ->
    // EglDisplay::importBufferAsImage -> EglDisplay::createImage) before it can draw a pixel into a
    // frame the daemon produced - so with a NULL the whole composited scene has no image to be
    // drawn into.  Both names are backed by entry points that exist: EGL_KHR_image_base by
    // eglCreateImage/eglDestroyImage and their KHR spellings (Exporting/Definitions.cpp, and the
    // same names in the glvnd vendor table), EGL_EXT_image_dma_buf_import by EGL_LINUX_DMA_BUF_EXT
    // images, which carry the caller's EGL_WIDTH, EGL_HEIGHT and EGL_LINUX_DRM_FOURCC_EXT and own a
    // dup() of every plane's fd, offset and pitch (EGLState/Core.cpp, CreateImage).
    //
    // EGL_EXT_image_dma_buf_import_modifiers IS DELIBERATELY NOT NAMED.  Its own two entry points -
    // eglQueryDmaBufFormatsEXT and eglQueryDmaBufModifiersEXT - do not exist in this library, and a
    // name in this string means the entry points beside it are there.  The modifier attributes
    // (EGL_DMA_BUF_PLANEn_MODIFIER_LO/HI_EXT) are nevertheless ACCEPTED and carried on the image:
    // a caller does not need the name to send them, and kwin's does not look at any extension
    // string before it does - EglDisplay::importDmaBufAsImage appends the pair whenever the
    // buffer's modifier is not DRM_FORMAT_MOD_INVALID.
    inline constexpr const char* kDisplayExtensionString =
        "EGL_KHR_create_context "
        // WHAT THE COMPOSITOR CHECKS BETWEEN ITS CONTEXT AND ITS FIRST SURFACE.  EglBackend::init
        // asks for these three by name right after initRenderingContext() and before createSurfaces();
        // the entry points behind them are published beside this string (Exporting/Definitions.cpp),
        // so the announcement and the answers are one thing rather than two.
        "EGL_KHR_fence_sync "
        "EGL_KHR_wait_sync "
        "EGL_KHR_swap_buffers_with_damage "
        "EGL_EXT_swap_buffers_with_damage "
        "EGL_EXT_buffer_age "
        "EGL_KHR_image_base "
        "EGL_EXT_image_dma_buf_import "
        "EGL_KHR_no_config_context "
        "EGL_KHR_surfaceless_context "
        // ANNOUNCED BECAUSE IT IS NOW ANSWERED: the four names below are published as real entry
        // points and eglGetProcAddress answers them (see Exporting/Definitions.cpp and
        // EGLImpl::GetProcAddress).  Both earlier states were measured failures: announcing the
        // extension while the names resolved to NULL made kwin jump to that NULL (signal 11,
        // pc=0x0, from KWin::EglDisplay::determineRenderNode()), and withdrawing the announcement
        // left kwin without a render device so its compositing never became active and nothing was
        // ever painted.
        "EGL_EXT_device_base "
        "EGL_EXT_device_enumeration "
        "EGL_EXT_device_query "
        "EGL_EXT_device_drm "
        "EGL_EXT_device_drm_render_node "
        // KWin::EglDisplay::determineRenderNode() at libkwin.so.6+0x308b9c.  A compositor that is
        // never told the extension exists cannot take that path; announcing it again is the right
        // move once these four names are really published and resolvable.
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
        "EGL_KHR_image_base "
        "EGL_EXT_image_dma_buf_import "
        "EGL_KHR_no_config_context "
        "EGL_KHR_surfaceless_context "
        // ANNOUNCED BECAUSE IT IS NOW ANSWERED: the four names below are published as real entry
        // points and eglGetProcAddress answers them (see Exporting/Definitions.cpp and
        // EGLImpl::GetProcAddress).  Both earlier states were measured failures: announcing the
        // extension while the names resolved to NULL made kwin jump to that NULL (signal 11,
        // pc=0x0, from KWin::EglDisplay::determineRenderNode()), and withdrawing the announcement
        // left kwin without a render device so its compositing never became active and nothing was
        // ever painted.
        "EGL_EXT_device_base "
        "EGL_EXT_device_enumeration "
        "EGL_EXT_device_query "
        "EGL_EXT_device_drm "
        "EGL_EXT_device_drm_render_node "
        // KWin::EglDisplay::determineRenderNode() at libkwin.so.6+0x308b9c.  A compositor that is
        // never told the extension exists cannot take that path; announcing it again is the right
        // move once these four names are really published and resolvable.
        "EGL_EXT_platform_device "
        "EGL_EXT_platform_base "
        "EGL_KHR_platform_base "
        "EGL_KHR_platform_gbm "
        "EGL_EXT_platform_gbm "
        "EGL_MESA_platform_gbm "
        "EGL_MESA_platform_surfaceless";
}
