// SPDX-License-Identifier: LGPL-3.0-only
// Public C interface for an application that owns a MobileGL split server.
#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define MOBILEGL_SERVER_EXPORT
#else
#define MOBILEGL_SERVER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;

typedef void (*mobilegl_server_geometry_callback)(void* user, uint32_t width, uint32_t height);

// Opaque-window hooks also let non-Android hosts embed the display without depending on
// MobileGL's C++ headers. Both reference callbacks are required. Geometry may be null.
struct mobilegl_server_display_hooks {
    void (*acquire)(void* user, void* window);
    void (*release)(void* user, void* window);
    mobilegl_server_geometry_callback request_geometry;
    void* user;
};

enum mobilegl_server_display_result {
    MOBILEGL_SERVER_DISPLAY_OK = 0,
    MOBILEGL_SERVER_DISPLAY_INVALID_ARGUMENT = -1,
    MOBILEGL_SERVER_DISPLAY_ALREADY_INSTALLED = -2,
    MOBILEGL_SERVER_DISPLAY_NOT_INSTALLED = -3,
    MOBILEGL_SERVER_DISPLAY_UNSUPPORTED_PLATFORM = -4
};

enum mobilegl_server_window_detach {
    MOBILEGL_SERVER_WINDOW_NO_WINDOW = 0,
    MOBILEGL_SERVER_WINDOW_RELEASED = 1,
    MOBILEGL_SERVER_WINDOW_RELEASED_BY_SESSION = 2,
    MOBILEGL_SERVER_WINDOW_TIMED_OUT = 3
};

// Install once before serving; owner lifecycle calls must be serialized. A live display or
// an old lease awaiting release refuses a replacement, so its window always uses its original
// reference hooks. The geometry callback runs on the apply thread and must return promptly.
// A positive size requests fixed buffer dimensions; 0/0 requests the host's layout dimensions.
// Report the resulting dimensions with attach, for example from SurfaceHolder.surfaceChanged.
// Callback user data must live until uninstall has returned and any timed-out lease has ended.
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_install(const struct mobilegl_server_display_hooks* hooks);
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_install_android(mobilegl_server_geometry_callback request_geometry,
                                                                 void* user);

// Attach retains its own reference. Attaching the same window updates its dimensions without
// another retain. A different window detaches the previous one first (up to 3 seconds).
// Android attach uses ANativeWindow_getWidth/Height for zero dimensions; the caller may release
// its ANativeWindow_fromSurface reference immediately after this call.
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_attach(void* window, uint32_t width, uint32_t height);
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_attach_android(struct ANativeWindow* window, uint32_t width,
                                                                uint32_t height);

// Returns mobilegl_server_window_detach. A timeout never drops a reference while the backend
// is rendering: release happens when the apply thread ends its lease. Surface loss currently
// ends the live split session; a fresh client session can use a newly attached window.
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_detach(uint32_t timeout_ms);
MOBILEGL_SERVER_EXPORT int mobilegl_server_display_uninstall(uint32_t timeout_ms);

// Blocking serve on the caller's thread; stop is non-blocking and safe on the UI thread.
// Endpoint: filesystem path, @abstract AF_UNIX name (SCM_RIGHTS + shared-memory rings), or
// tcp://host:port. One client session per process; independent workers need distinct endpoints.
// Before loading libMobileGL set MOBILEGL_IPC_ROLE=server and MOBILEGL_IPC_DIAL=no, and unset
// MOBILEGL_TRANSPORT, MOBILEGL_IPC_SERVER_PATH, MOBILEGL_IPC_RING_MB, MOBILEGL_IPC_STAGE_MB.
MOBILEGL_SERVER_EXPORT int mobilegl_server_serve_inprocess(const char* endpoint);
MOBILEGL_SERVER_EXPORT void mobilegl_server_stop_inprocess(void);

#ifdef __cplusplus
}
#endif
