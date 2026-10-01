// SPDX-License-Identifier: LGPL-3.0-only
#include "EmbeddedServer.h"
#include "ServerDisplay.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace {
    using namespace MobileGL::MG_Remote::Server;

    int DetachResult(ServerWindowDetach result) {
        switch (result) {
        case ServerWindowDetach::NoWindow: return MOBILEGL_SERVER_WINDOW_NO_WINDOW;
        case ServerWindowDetach::Released: return MOBILEGL_SERVER_WINDOW_RELEASED;
        case ServerWindowDetach::ReleasedBySession: return MOBILEGL_SERVER_WINDOW_RELEASED_BY_SESSION;
        case ServerWindowDetach::TimedOut: return MOBILEGL_SERVER_WINDOW_TIMED_OUT;
        }
        return MOBILEGL_SERVER_WINDOW_TIMED_OUT;
    }
}

extern "C" int mobilegl_server_display_install(const struct mobilegl_server_display_hooks* hooks) {
    if (hooks == nullptr || hooks->acquire == nullptr || hooks->release == nullptr)
        return MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT;
    auto& display = ServerDisplayInstance();
    if (display.HasDisplay() || display.Attached() || display.Leased())
        return MOBILEGL_SERVER_DISPLAY_ALREADY_INSTALLED;
    ServerDisplayHooks nativeHooks;
    nativeHooks.acquire = hooks->acquire;
    nativeHooks.release = hooks->release;
    nativeHooks.requestGeometry = hooks->request_geometry;
    nativeHooks.user = hooks->user;
    display.Install(nativeHooks);
    return MOBILEGL_SERVER_DISPLAY_OK;
}

extern "C" int mobilegl_server_display_install_android(mobilegl_server_geometry_callback request_geometry, void* user) {
#if defined(__ANDROID__)
    const auto androidHooks = AndroidNativeWindowHooks(request_geometry, user);
    const mobilegl_server_display_hooks hooks{androidHooks.acquire, androidHooks.release,
                                             androidHooks.requestGeometry, androidHooks.user};
    return mobilegl_server_display_install(&hooks);
#else
    (void)request_geometry;
    (void)user;
    return MOBILEGL_SERVER_DISPLAY_UNSUPPORTED_PLATFORM;
#endif
}

extern "C" int mobilegl_server_display_attach(void* window, uint32_t width, uint32_t height) {
    if (window == nullptr) return MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT;
    auto& display = ServerDisplayInstance();
    if (!display.HasDisplay()) return MOBILEGL_SERVER_DISPLAY_NOT_INSTALLED;
    display.Attach(window, width, height);
    return MOBILEGL_SERVER_DISPLAY_OK;
}

extern "C" int mobilegl_server_display_attach_android(struct ANativeWindow* window, uint32_t width, uint32_t height) {
#if defined(__ANDROID__)
    if (window == nullptr) return MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT;
    if (width == 0) {
        const int32_t actual = ANativeWindow_getWidth(window);
        if (actual > 0) width = static_cast<uint32_t>(actual);
    }
    if (height == 0) {
        const int32_t actual = ANativeWindow_getHeight(window);
        if (actual > 0) height = static_cast<uint32_t>(actual);
    }
    return mobilegl_server_display_attach(window, width, height);
#else
    (void)window;
    (void)width;
    (void)height;
    return MOBILEGL_SERVER_DISPLAY_UNSUPPORTED_PLATFORM;
#endif
}

extern "C" int mobilegl_server_display_detach(uint32_t timeout_ms) {
    return DetachResult(ServerDisplayInstance().Detach(timeout_ms));
}

extern "C" int mobilegl_server_display_uninstall(uint32_t timeout_ms) {
    auto& display = ServerDisplayInstance();
    const int result = DetachResult(display.Detach(timeout_ms));
    // Detach has already removed the attached window even if its lease is still live. Uninstall
    // keeps the original hooks for EndLease's deferred release, without another timeout budget.
    display.Uninstall(0);
    return result;
}
