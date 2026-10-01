// MobileGL - MobileGL/MG_Impl/EGLImpl/WaylandWindow.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WaylandWindow.h"

#if MOBILEGL_WAYLAND_WINDOWS

#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Util/Debug/Log.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    namespace {
        // libwayland-client's types are opaque here: everything goes through wl_proxy_* and the
        // exported interface descriptions, and nothing reads a field of either.
        struct wl_proxy;
        struct wl_interface;
        struct wl_event_queue;

        // wayland-egl-backend.h, WL_EGL_WINDOW_VERSION 3: the struct libwayland-egl allocates for
        // wl_egl_window_create and the driver reads.  The ABI is versioned by its first word.
        struct EglWindow {
            const intptr_t version;
            int width;
            int height;
            int dx;
            int dy;
            int attached_width;
            int attached_height;
            void* driver_private;
            void (*resize_callback)(EglWindow*, void*);
            void (*destroy_window_callback)(void*);
            wl_proxy* surface;
        };
        constexpr intptr_t kEglWindowVersion = 3;

        // Request opcodes (wayland.xml, the order the requests are declared in).
        constexpr uint32_t kDisplayGetRegistry = 1;
        constexpr uint32_t kRegistryBind = 0;
        constexpr uint32_t kShmCreatePool = 0;
        constexpr uint32_t kShmPoolCreateBuffer = 0;
        constexpr uint32_t kShmPoolDestroy = 1;
        constexpr uint32_t kBufferDestroy = 0;
        constexpr uint32_t kSurfaceAttach = 1;
        constexpr uint32_t kSurfaceDamage = 2;
        constexpr uint32_t kSurfaceCommit = 6;
        constexpr uint32_t kSurfaceDamageBuffer = 9; // since wl_surface version 4
        constexpr uint32_t kMarshalFlagDestroy = 1u << 0;
        // wl_shm formats.  The two every compositor must support; byte order in memory is
        // B, G, R, A for both (little-endian 0xAARRGGBB).
        constexpr uint32_t kShmFormatArgb8888 = 0;
        constexpr uint32_t kShmFormatXrgb8888 = 1;

        struct Api {
            Bool loaded = false;
            const wl_interface* displayInterface = nullptr;
            const wl_interface* registryInterface = nullptr;
            const wl_interface* shmInterface = nullptr;
            const wl_interface* shmPoolInterface = nullptr;
            const wl_interface* bufferInterface = nullptr;
            wl_proxy* (*marshalFlags)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, uint32_t, ...) = nullptr;
            int (*addListener)(wl_proxy*, void (**)(void), void*) = nullptr;
            void (*destroy)(wl_proxy*) = nullptr;
            uint32_t (*getVersion)(wl_proxy*) = nullptr;
            void* (*createWrapper)(void*) = nullptr;
            void (*wrapperDestroy)(void*) = nullptr;
            void (*setQueue)(wl_proxy*, wl_event_queue*) = nullptr;
            wl_event_queue* (*createQueue)(void*) = nullptr;
            void (*queueDestroy)(wl_event_queue*) = nullptr;
            int (*roundtripQueue)(void*, wl_event_queue*) = nullptr;
            int (*dispatchQueue)(void*, wl_event_queue*) = nullptr;
            int (*dispatchQueuePending)(void*, wl_event_queue*) = nullptr;
            int (*flush)(void*) = nullptr;
        };

        // Loaded once.  RTLD_NOLOAD first: a Wayland client has the library already, and the copy
        // it is using is the one whose wl_display this must talk to.
        const Api& WaylandApi() {
            static Api api;
            static std::once_flag once;
            std::call_once(once, [] {
                void* lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
                if (lib == nullptr) lib = dlopen("libwayland-client.so.0", RTLD_NOW);
                if (lib == nullptr) return;
                const auto sym = [lib](const char* name) { return dlsym(lib, name); };
                api.displayInterface = static_cast<const wl_interface*>(sym("wl_display_interface"));
                api.registryInterface = static_cast<const wl_interface*>(sym("wl_registry_interface"));
                api.shmInterface = static_cast<const wl_interface*>(sym("wl_shm_interface"));
                api.shmPoolInterface = static_cast<const wl_interface*>(sym("wl_shm_pool_interface"));
                api.bufferInterface = static_cast<const wl_interface*>(sym("wl_buffer_interface"));
                api.marshalFlags = reinterpret_cast<decltype(api.marshalFlags)>(sym("wl_proxy_marshal_flags"));
                api.addListener = reinterpret_cast<decltype(api.addListener)>(sym("wl_proxy_add_listener"));
                api.destroy = reinterpret_cast<decltype(api.destroy)>(sym("wl_proxy_destroy"));
                api.getVersion = reinterpret_cast<decltype(api.getVersion)>(sym("wl_proxy_get_version"));
                api.createWrapper = reinterpret_cast<decltype(api.createWrapper)>(sym("wl_proxy_create_wrapper"));
                api.wrapperDestroy = reinterpret_cast<decltype(api.wrapperDestroy)>(sym("wl_proxy_wrapper_destroy"));
                api.setQueue = reinterpret_cast<decltype(api.setQueue)>(sym("wl_proxy_set_queue"));
                api.createQueue = reinterpret_cast<decltype(api.createQueue)>(sym("wl_display_create_queue"));
                api.queueDestroy = reinterpret_cast<decltype(api.queueDestroy)>(sym("wl_event_queue_destroy"));
                api.roundtripQueue = reinterpret_cast<decltype(api.roundtripQueue)>(sym("wl_display_roundtrip_queue"));
                api.dispatchQueue = reinterpret_cast<decltype(api.dispatchQueue)>(sym("wl_display_dispatch_queue"));
                api.dispatchQueuePending =
                    reinterpret_cast<decltype(api.dispatchQueuePending)>(sym("wl_display_dispatch_queue_pending"));
                api.flush = reinterpret_cast<decltype(api.flush)>(sym("wl_display_flush"));
                api.loaded = api.displayInterface && api.registryInterface && api.shmInterface &&
                             api.shmPoolInterface && api.bufferInterface && api.marshalFlags && api.addListener && api.destroy &&
                             api.getVersion && api.createWrapper && api.wrapperDestroy && api.setQueue &&
                             api.createQueue && api.queueDestroy && api.roundtripQueue && api.dispatchQueue &&
                             api.dispatchQueuePending && api.flush;
                if (!api.loaded) MGLOG_E("Wayland: libwayland-client.so.0 lacks an entry point this library uses");
            });
            return api;
        }

        struct ShmBuffer {
            wl_proxy* buffer = nullptr;
            void* map = nullptr;
            SizeT size = 0;
            EGLint width = 0;
            EGLint height = 0;
            EGLint stride = 0;
            Bool busy = false;
        };

        // The compositor is done with a buffer: it may be drawn into again.
        void OnBufferRelease(void* data, wl_proxy*) { static_cast<ShmBuffer*>(data)->busy = false; }
        void (*const kBufferListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnBufferRelease)};
    } // namespace

    struct WindowSurface::Impl {
        void* display = nullptr;
        EglWindow* window = nullptr;
        wl_event_queue* queue = nullptr;
        void* displayWrapper = nullptr;
        wl_proxy* registry = nullptr;
        wl_proxy* shm = nullptr;
        uint32_t format = kShmFormatXrgb8888;
        EGLint width = 0;
        EGLint height = 0;
        // Three: one on the glass, one the compositor may still be reading, one to draw into.
        ShmBuffer buffers[3];
        Vector<Uint8> scratch;

        ~Impl() {
            const Api& api = WaylandApi();
            for (auto& buffer : buffers) Release(buffer);
            // Neither wl_shm (version 1) nor wl_registry has a destroy request: the proxies go
            // on this side only.
            if (shm) api.destroy(shm);
            if (registry) api.destroy(registry);
            if (displayWrapper) api.wrapperDestroy(displayWrapper);
            if (queue) api.queueDestroy(queue);
            if (window && window->driver_private == this) {
                window->driver_private = nullptr;
                window->destroy_window_callback = nullptr;
            }
        }

        void Release(ShmBuffer& buffer) {
            const Api& api = WaylandApi();
            if (buffer.buffer) api.marshalFlags(buffer.buffer, kBufferDestroy, nullptr, api.getVersion(buffer.buffer),
                                                kMarshalFlagDestroy);
            if (buffer.map) munmap(buffer.map, buffer.size);
            buffer = ShmBuffer{};
        }

        Bool Allocate(ShmBuffer& buffer) {
            const Api& api = WaylandApi();
            Release(buffer);
            const EGLint stride = width * 4;
            const SizeT size = static_cast<SizeT>(stride) * static_cast<SizeT>(height);
            const int fd = memfd_create("mobilegl-wayland", MFD_CLOEXEC);
            if (fd < 0) {
                MGLOG_E("Wayland: memfd_create for a %dx%d buffer failed: %s", width, height, std::strerror(errno));
                return false;
            }
            if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
                MGLOG_E("Wayland: sizing a %dx%d buffer failed: %s", width, height, std::strerror(errno));
                close(fd);
                return false;
            }
            void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                MGLOG_E("Wayland: mapping a %dx%d buffer failed: %s", width, height, std::strerror(errno));
                close(fd);
                return false;
            }
            // One pool per buffer, destroyed at once: the buffer keeps the memory alive.
            wl_proxy* pool = api.marshalFlags(shm, kShmCreatePool, api.shmPoolInterface, api.getVersion(shm), 0, nullptr,
                                              fd, static_cast<int32_t>(size));
            close(fd);
            if (pool == nullptr) {
                munmap(map, size);
                return false;
            }
            wl_proxy* wlBuffer = api.marshalFlags(pool, kShmPoolCreateBuffer, api.bufferInterface, api.getVersion(pool),
                                                  0, nullptr, 0, width, height, stride, format);
            api.marshalFlags(pool, kShmPoolDestroy, nullptr, api.getVersion(pool), kMarshalFlagDestroy);
            if (wlBuffer == nullptr) {
                munmap(map, size);
                return false;
            }
            buffer.buffer = wlBuffer;
            buffer.map = map;
            buffer.size = size;
            buffer.width = width;
            buffer.height = height;
            buffer.stride = stride;
            buffer.busy = false;
            api.addListener(wlBuffer, const_cast<void (**)(void)>(kBufferListener), &buffer);
            return true;
        }

        // A buffer the compositor is not reading: the events already received first, then (all
        // three still held) a blocking wait for the next release.
        ShmBuffer* NextFree() {
            const Api& api = WaylandApi();
            for (;;) {
                if (api.dispatchQueuePending(display, queue) < 0) return nullptr;
                for (auto& buffer : buffers) {
                    if (buffer.buffer == nullptr || buffer.busy) continue;
                    if (buffer.width != width || buffer.height != height) {
                        if (!Allocate(buffer)) return nullptr;
                    }
                    return &buffer;
                }
                for (auto& buffer : buffers) {
                    if (buffer.buffer == nullptr) return Allocate(buffer) ? &buffer : nullptr;
                }
                if (api.dispatchQueue(display, queue) < 0) return nullptr;
            }
        }
    };

    namespace {
        void OnRegistryGlobal(void* data, wl_proxy* registry, uint32_t name, const char* interface, uint32_t) {
            auto* impl = static_cast<WindowSurface::Impl*>(data);
            if (impl->shm != nullptr || std::strcmp(interface, "wl_shm") != 0) return;
            const Api& api = WaylandApi();
            impl->shm = api.marshalFlags(registry, kRegistryBind, api.shmInterface, 1, 0, name, "wl_shm", 1u, nullptr);
        }
        void OnRegistryGlobalRemove(void*, wl_proxy*, uint32_t) {}
        void (*const kRegistryListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnRegistryGlobal),
                                                   reinterpret_cast<void (*)(void)>(&OnRegistryGlobalRemove)};

        // The application destroyed its wl_egl_window first: nothing may be read from it after this.
        void OnWindowDestroyed(void* data) { static_cast<WindowSurface::Impl*>(data)->window = nullptr; }
    } // namespace

    Bool IsWaylandDisplay(Uint64 nativeDisplay, EGLenum platform) {
        if (platform == kPlatformWayland) return true;
        if (nativeDisplay == 0 || (platform != EGL_NONE && platform != 0)) return false;
        const Api& api = WaylandApi();
        if (!api.loaded) return false;
        return *reinterpret_cast<const void* const*>(static_cast<uintptr_t>(nativeDisplay)) == api.displayInterface;
    }

    Bool IsWaylandWindow(const void* window) {
        if (window == nullptr) return false;
        const auto* eglWindow = static_cast<const EglWindow*>(window);
        return eglWindow->version >= kEglWindowVersion && eglWindow->surface != nullptr;
    }

    Bool WindowSize(const void* window, EGLint* width, EGLint* height) {
        if (!IsWaylandWindow(window)) return false;
        const auto* eglWindow = static_cast<const EglWindow*>(window);
        *width = eglWindow->width;
        *height = eglWindow->height;
        return eglWindow->width > 0 && eglWindow->height > 0;
    }

    WindowSurface::WindowSurface(UniquePtr<Impl> impl) : m_impl(std::move(impl)) {}
    WindowSurface::~WindowSurface() = default;

    UniquePtr<WindowSurface> WindowSurface::Create(void* wlDisplay, void* wlEglWindow, EGLint width, EGLint height,
                                                   Bool hasAlpha) {
        const Api& api = WaylandApi();
        if (!api.loaded) {
            MGLOG_E("Wayland: a window surface needs libwayland-client.so.0, and it could not be used");
            return nullptr;
        }
        if (wlDisplay == nullptr || !IsWaylandWindow(wlEglWindow) || width <= 0 || height <= 0) return nullptr;
        auto impl = MakeUnique<Impl>();
        impl->display = wlDisplay;
        impl->window = static_cast<EglWindow*>(wlEglWindow);
        impl->width = width;
        impl->height = height;
        impl->format = hasAlpha ? kShmFormatArgb8888 : kShmFormatXrgb8888;
        // A queue of this surface's own, so the application's dispatching never sees these events
        // and this never dispatches the application's.
        impl->queue = api.createQueue(wlDisplay);
        impl->displayWrapper = api.createWrapper(wlDisplay);
        if (impl->queue == nullptr || impl->displayWrapper == nullptr) return nullptr;
        api.setQueue(static_cast<wl_proxy*>(impl->displayWrapper), impl->queue);
        auto* wrapper = static_cast<wl_proxy*>(impl->displayWrapper);
        impl->registry = api.marshalFlags(wrapper, kDisplayGetRegistry, api.registryInterface, api.getVersion(wrapper),
                                          0, nullptr);
        if (impl->registry == nullptr) return nullptr;
        api.addListener(impl->registry, const_cast<void (**)(void)>(kRegistryListener), impl.get());
        if (api.roundtripQueue(wlDisplay, impl->queue) < 0 || impl->shm == nullptr) {
            MGLOG_E("Wayland: the compositor offers no wl_shm; a window surface cannot be presented");
            return nullptr;
        }
        impl->window->driver_private = impl.get();
        impl->window->destroy_window_callback = &OnWindowDestroyed;
        MGLOG_I("Wayland: window surface %dx%d presented through wl_shm (%s)", width, height,
                hasAlpha ? "ARGB8888" : "XRGB8888");
        return UniquePtr<WindowSurface>(new WindowSurface(std::move(impl)));
    }

    Bool WindowSurface::Present() {
        Impl& impl = *m_impl;
        if (impl.window == nullptr) return false;
        const Api& api = WaylandApi();
        ShmBuffer* buffer = impl.NextFree();
        if (buffer == nullptr) {
            MGLOG_E_ONCE("Wayland: no wl_shm buffer could be taken for the frame; it is not shown");
            return false;
        }

        // The frame, out of the default framebuffer, through this library's own entry points: the
        // application's read framebuffer, pack buffer and pack state are saved and put back, so
        // the readback is invisible to it.
        namespace GL = MG_Impl::GLImpl;
        GLint readFramebuffer = 0, packBuffer = 0, alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
        GL::GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        GL::GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        GL::GetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        GL::GetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
        GL::GetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        GL::GetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        if (readFramebuffer != 0) GL::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (packBuffer != 0) GL::BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        GL::PixelStorei(GL_PACK_ALIGNMENT, 4);
        GL::PixelStorei(GL_PACK_ROW_LENGTH, 0);
        GL::PixelStorei(GL_PACK_SKIP_ROWS, 0);
        GL::PixelStorei(GL_PACK_SKIP_PIXELS, 0);
        const SizeT rowBytes = static_cast<SizeT>(impl.width) * 4;
        impl.scratch.resize(rowBytes * static_cast<SizeT>(impl.height));
        GL::ReadPixels(0, 0, impl.width, impl.height, GL_RGBA, GL_UNSIGNED_BYTE, impl.scratch.data());
        GL::PixelStorei(GL_PACK_ALIGNMENT, alignment);
        GL::PixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        GL::PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        GL::PixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        if (packBuffer != 0) GL::BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        if (readFramebuffer != 0) GL::BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));

        // GL's rows run bottom-up and RGBA; a wl_shm buffer's run top-down and B, G, R, A.
        auto* dst = static_cast<Uint8*>(buffer->map);
        for (EGLint y = 0; y < impl.height; ++y) {
            const Uint8* src = impl.scratch.data() + static_cast<SizeT>(impl.height - 1 - y) * rowBytes;
            Uint8* row = dst + static_cast<SizeT>(y) * static_cast<SizeT>(buffer->stride);
            for (EGLint x = 0; x < impl.width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 2];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 0];
                row[x * 4 + 3] = src[x * 4 + 3];
            }
        }

        wl_proxy* surface = impl.window->surface;
        const uint32_t surfaceVersion = api.getVersion(surface);
        api.marshalFlags(surface, kSurfaceAttach, nullptr, surfaceVersion, 0, buffer->buffer, impl.window->dx,
                         impl.window->dy);
        if (surfaceVersion >= 4) {
            api.marshalFlags(surface, kSurfaceDamageBuffer, nullptr, surfaceVersion, 0, 0, 0, impl.width,
                             impl.height);
        } else {
            api.marshalFlags(surface, kSurfaceDamage, nullptr, surfaceVersion, 0, 0, 0, impl.width, impl.height);
        }
        api.marshalFlags(surface, kSurfaceCommit, nullptr, surfaceVersion, 0);
        buffer->busy = true;
        impl.window->attached_width = impl.width;
        impl.window->attached_height = impl.height;
        impl.window->dx = 0;
        impl.window->dy = 0;
        api.flush(impl.display);
        return true;
    }
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland

#endif
