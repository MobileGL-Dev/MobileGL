// MobileGL - MobileGL/MG_Impl/EGLImpl/WaylandWindow.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WaylandWindow.h"

#if MOBILEGL_WAYLAND_WINDOWS

#include "EGLImpl.h"
#include <MG_Backend/BackendObjects.h>
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
#include <sys/uio.h>
#include <unistd.h>

namespace MobileGL::MG_Impl::EGLImpl {
    void ReadBackFrameBGRA(EGLint width, EGLint height, Vector<Uint8>& scratch, Uint8* dst, SizeT dstStride) {
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
        const SizeT rowBytes = static_cast<SizeT>(width) * 4;
        scratch.resize(rowBytes * static_cast<SizeT>(height));
        GL::ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, scratch.data());
        GL::PixelStorei(GL_PACK_ALIGNMENT, alignment);
        GL::PixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        GL::PixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        GL::PixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        if (packBuffer != 0) GL::BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        if (readFramebuffer != 0) GL::BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));

        for (EGLint y = 0; y < height; ++y) {
            const Uint8* src = scratch.data() + static_cast<SizeT>(height - 1 - y) * rowBytes;
            Uint8* row = dst + static_cast<SizeT>(y) * dstStride;
            for (EGLint x = 0; x < width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 2];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 0];
                row[x * 4 + 3] = src[x * 4 + 3];
            }
        }
    }
} // namespace MobileGL::MG_Impl::EGLImpl

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    namespace {
        // libwayland-client's objects are opaque here: everything goes through wl_proxy_* and the
        // interface descriptions, and nothing reads a field of a proxy.
        struct wl_proxy;
        struct wl_event_queue;

        // wayland-util.h's protocol descriptors. libwayland-client exports the core protocol's;
        // linux-dmabuf's are written out below (LinuxDmabuf), as its generated code would.
        struct wl_interface;
        struct wl_message {
            const char* name;
            const char* signature;
            const wl_interface** types;
        };
        struct wl_interface {
            const char* name;
            int version;
            int method_count;
            const wl_message* methods;
            int event_count;
            const wl_message* events;
        };

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
        // linux-dmabuf (linux-dmabuf-v1.xml) request opcodes.
        constexpr uint32_t kDmabufDestroy = 0;
        constexpr uint32_t kDmabufCreateParams = 1;
        constexpr uint32_t kParamsDestroy = 0;
        constexpr uint32_t kParamsAdd = 1;
        constexpr uint32_t kParamsCreate = 2;
        // The highest linux-dmabuf version spoken here: 4 adds feedback objects this never asks for.
        constexpr uint32_t kDmabufMaxVersion = 3;
        // DRM fourccs of the server's shared images: R, G, B, A in memory - GL's RGBA8, so the
        // frame lands in the buffer with no reordering.
        constexpr uint32_t kDrmFormatAbgr8888 = 0x34324241u; // 'AB24'
        constexpr uint32_t kDrmFormatXbgr8888 = 0x34324258u; // 'XB24'

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

        // zwp_linux_dmabuf_v1 and zwp_linux_buffer_params_v1 as their generated protocol code
        // describes them (version 3: what is bound here at most). A request's `types` name the
        // interface of each new_id/object argument, null for every other argument.
        struct LinuxDmabufProtocol {
            const wl_interface* createParamsTypes[1] = {};
            const wl_interface* noObjectTypes[6] = {};
            const wl_interface* createImmedTypes[5] = {};
            const wl_interface* createdTypes[1] = {};
            wl_message dmabufRequests[2] = {};
            wl_message dmabufEvents[2] = {};
            wl_message paramsRequests[4] = {};
            wl_message paramsEvents[2] = {};
            wl_interface dmabuf = {};
            wl_interface params = {};
        };

        // Null when libwayland-client is not usable. Filled once: the wl_buffer interface it
        // refers to is libwayland-client's, known only after it is loaded.
        const LinuxDmabufProtocol* LinuxDmabuf() {
            static LinuxDmabufProtocol protocol;
            static std::once_flag once;
            static Bool ready = false;
            std::call_once(once, [] {
                const Api& api = WaylandApi();
                if (!api.loaded) return;
                LinuxDmabufProtocol& p = protocol;
                p.createParamsTypes[0] = &p.params;
                p.createImmedTypes[0] = api.bufferInterface;
                p.createdTypes[0] = api.bufferInterface;
                p.dmabufRequests[0] = {"destroy", "", p.noObjectTypes};
                p.dmabufRequests[1] = {"create_params", "n", p.createParamsTypes};
                p.dmabufEvents[0] = {"format", "u", p.noObjectTypes};
                p.dmabufEvents[1] = {"modifier", "3uuu", p.noObjectTypes};
                p.paramsRequests[0] = {"destroy", "", p.noObjectTypes};
                p.paramsRequests[1] = {"add", "huuuuu", p.noObjectTypes};
                p.paramsRequests[2] = {"create", "iiuu", p.noObjectTypes};
                p.paramsRequests[3] = {"create_immed", "2niiuu", p.createImmedTypes};
                p.paramsEvents[0] = {"created", "n", p.createdTypes};
                p.paramsEvents[1] = {"failed", "", p.noObjectTypes};
                p.dmabuf = {"zwp_linux_dmabuf_v1", static_cast<int>(kDmabufMaxVersion), 2, p.dmabufRequests, 2,
                            p.dmabufEvents};
                p.params = {"zwp_linux_buffer_params_v1", static_cast<int>(kDmabufMaxVersion), 4, p.paramsRequests,
                            2, p.paramsEvents};
                ready = true;
            });
            return ready ? &protocol : nullptr;
        }

        // The format table a compositor announces is not consulted: the server's images are one
        // of the two formats every linux-dmabuf compositor takes, and a refusal is answered by
        // `failed` below anyway.
        void OnDmabufFormat(void*, wl_proxy*, uint32_t) {}
        void OnDmabufModifier(void*, wl_proxy*, uint32_t, uint32_t, uint32_t) {}
        void (*const kDmabufListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnDmabufFormat),
                                                 reinterpret_cast<void (*)(void)>(&OnDmabufModifier)};

        // The answer to a `create`: the new wl_buffer, or `failed`.
        struct ParamsOutcome {
            wl_proxy* buffer = nullptr;
            Bool failed = false;
        };
        void OnParamsCreated(void* data, wl_proxy*, wl_proxy* buffer) { static_cast<ParamsOutcome*>(data)->buffer = buffer; }
        void OnParamsFailed(void* data, wl_proxy*) { static_cast<ParamsOutcome*>(data)->failed = true; }
        void (*const kParamsListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnParamsCreated),
                                                 reinterpret_cast<void (*)(void)>(&OnParamsFailed)};

        // One server shared image, shown to the compositor as a linux-dmabuf wl_buffer.
        struct DmabufBuffer {
            wl_proxy* buffer = nullptr;
            Uint64 imageId = 0;
            EGLint width = 0;
            EGLint height = 0;
            Bool busy = false;
        };

        void OnDmabufBufferRelease(void* data, wl_proxy*) { static_cast<DmabufBuffer*>(data)->busy = false; }
        void (*const kDmabufBufferListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnDmabufBufferRelease)};

        // MOBILEGL_WAYLAND_DMABUF=0 keeps every window on wl_shm.
        Bool DmabufPresentationAllowed() {
            const char* value = std::getenv("MOBILEGL_WAYLAND_DMABUF");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }
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

        // THE LINUX-DMABUF PATH. With a backend that has shared images, the frame never leaves
        // the GPU: each buffer is a server-allocated image exported as a dma-buf, the server
        // copies the frame into it at the swap, and the compositor (a client of the same server)
        // samples the very same image. wl_shm stays bound as the fallback.
        Bool dmabufWanted = false;
        wl_proxy* dmabuf = nullptr;
        Bool useDmabuf = false;
        uint32_t fourcc = kDrmFormatXbgr8888;
        DmabufBuffer dmabufBuffers[3];

        ~Impl() {
            const Api& api = WaylandApi();
            for (auto& buffer : buffers) Release(buffer);
            for (auto& buffer : dmabufBuffers) Release(buffer);
            if (dmabuf) api.marshalFlags(dmabuf, kDmabufDestroy, nullptr, api.getVersion(dmabuf), kMarshalFlagDestroy);
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

        // The wl_buffer goes before the image: the compositor's own import holds the image for
        // as long as it still shows it.
        void Release(DmabufBuffer& buffer) {
            const Api& api = WaylandApi();
            if (buffer.buffer) api.marshalFlags(buffer.buffer, kBufferDestroy, nullptr, api.getVersion(buffer.buffer),
                                                kMarshalFlagDestroy);
            if (buffer.imageId != 0) {
                if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
                    (void)backendObject->ReleaseSharedImage(buffer.imageId);
                }
            }
            buffer = DmabufBuffer{};
        }

        // A shared image of the window's size, as a wl_buffer. `create` and a round trip, never
        // create_immed: a compositor that cannot import the buffer answers create_immed with a
        // protocol error, fatal to the application's whole connection, where `create` answers
        // `failed` and the window simply stays on wl_shm.
        Bool Allocate(DmabufBuffer& buffer) {
            const Api& api = WaylandApi();
            const LinuxDmabufProtocol* protocol = LinuxDmabuf();
            Release(buffer);
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            if (protocol == nullptr || dmabuf == nullptr || backendObject == nullptr) return false;
            MG_Backend::SharedImageExport image;
            if (!backendObject->AllocateSharedImage(static_cast<Uint32>(width), static_cast<Uint32>(height), fourcc,
                                                    &image)) {
                MGLOG_E("Wayland: the server allocated no %dx%d shared image", width, height);
                return false;
            }
            const auto dropImage = [&] {
                if (image.Fd >= 0) close(image.Fd);
                (void)backendObject->ReleaseSharedImage(image.Id);
            };
            if (image.Fd < 0 || image.Width != static_cast<Uint32>(width) ||
                image.Height != static_cast<Uint32>(height)) {
                MGLOG_E("Wayland: a %dx%d shared image came back as %ux%u (fd %d)", width, height, image.Width,
                        image.Height, image.Fd);
                dropImage();
                return false;
            }

            const uint32_t version = api.getVersion(dmabuf);
            wl_proxy* params = api.marshalFlags(dmabuf, kDmabufCreateParams, &protocol->params, version, 0, nullptr);
            if (params == nullptr) {
                dropImage();
                return false;
            }
            ParamsOutcome outcome;
            api.addListener(params, const_cast<void (**)(void)>(kParamsListener), &outcome);
            // The descriptor is duplicated into the request, so it is closed below either way.
            api.marshalFlags(params, kParamsAdd, nullptr, version, 0, image.Fd, 0u, image.Offset, image.Stride,
                             static_cast<uint32_t>(image.Modifier >> 32),
                             static_cast<uint32_t>(image.Modifier & 0xffffffffu));
            api.marshalFlags(params, kParamsCreate, nullptr, version, 0, width, height, fourcc, 0u);
            const Bool settled = api.roundtripQueue(display, queue) >= 0;
            api.marshalFlags(params, kParamsDestroy, nullptr, version, kMarshalFlagDestroy);
            wl_proxy* wlBuffer = outcome.buffer;
            if (!settled || outcome.failed || wlBuffer == nullptr) {
                MGLOG_E("Wayland: the compositor refused a %dx%d linux-dmabuf buffer", width, height);
                if (wlBuffer)
                    api.marshalFlags(wlBuffer, kBufferDestroy, nullptr, api.getVersion(wlBuffer), kMarshalFlagDestroy);
                dropImage();
                return false;
            }
            close(image.Fd);
            buffer.buffer = wlBuffer;
            buffer.imageId = image.Id;
            buffer.width = width;
            buffer.height = height;
            buffer.busy = false;
            api.addListener(wlBuffer, const_cast<void (**)(void)>(kDmabufBufferListener), &buffer);
            return true;
        }

        // As NextFree, over the shared images.
        DmabufBuffer* NextFreeDmabuf() {
            const Api& api = WaylandApi();
            for (;;) {
                if (api.dispatchQueuePending(display, queue) < 0) return nullptr;
                for (auto& buffer : dmabufBuffers) {
                    if (buffer.buffer == nullptr || buffer.busy) continue;
                    if (buffer.width != width || buffer.height != height) {
                        if (!Allocate(buffer)) return nullptr;
                    }
                    return &buffer;
                }
                for (auto& buffer : dmabufBuffers) {
                    if (buffer.buffer == nullptr) return Allocate(buffer) ? &buffer : nullptr;
                }
                if (api.dispatchQueue(display, queue) < 0) return nullptr;
            }
        }

        // The rest of this window's life is wl_shm's.
        void FallBackToShm() {
            for (auto& buffer : dmabufBuffers) Release(buffer);
            useDmabuf = false;
        }

        // `wlBuffer` becomes the window's content: attach, damage, commit.
        void Commit(wl_proxy* wlBuffer) {
            const Api& api = WaylandApi();
            wl_proxy* surface = window->surface;
            const uint32_t surfaceVersion = api.getVersion(surface);
            api.marshalFlags(surface, kSurfaceAttach, nullptr, surfaceVersion, 0, wlBuffer, window->dx, window->dy);
            if (surfaceVersion >= 4) {
                api.marshalFlags(surface, kSurfaceDamageBuffer, nullptr, surfaceVersion, 0, 0, 0, width, height);
            } else {
                api.marshalFlags(surface, kSurfaceDamage, nullptr, surfaceVersion, 0, 0, 0, width, height);
            }
            api.marshalFlags(surface, kSurfaceCommit, nullptr, surfaceVersion, 0);
            window->attached_width = width;
            window->attached_height = height;
            window->dx = 0;
            window->dy = 0;
            api.flush(display);
        }
    };

    namespace {
        void OnRegistryGlobal(void* data, wl_proxy* registry, uint32_t name, const char* interface,
                              uint32_t version) {
            auto* impl = static_cast<WindowSurface::Impl*>(data);
            const Api& api = WaylandApi();
            if (impl->shm == nullptr && std::strcmp(interface, "wl_shm") == 0) {
                impl->shm =
                    api.marshalFlags(registry, kRegistryBind, api.shmInterface, 1, 0, name, "wl_shm", 1u, nullptr);
                return;
            }
            if (impl->dmabufWanted && impl->dmabuf == nullptr &&
                std::strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
                const LinuxDmabufProtocol* protocol = LinuxDmabuf();
                if (protocol == nullptr) return;
                const uint32_t bindVersion = std::min(version, kDmabufMaxVersion);
                impl->dmabuf = api.marshalFlags(registry, kRegistryBind, &protocol->dmabuf, bindVersion, 0, name,
                                                "zwp_linux_dmabuf_v1", bindVersion, nullptr);
                if (impl->dmabuf != nullptr) {
                    api.addListener(impl->dmabuf, const_cast<void (**)(void)>(kDmabufListener), impl);
                }
            }
        }
        void OnRegistryGlobalRemove(void*, wl_proxy*, uint32_t) {}
        void (*const kRegistryListener[])(void) = {reinterpret_cast<void (*)(void)>(&OnRegistryGlobal),
                                                   reinterpret_cast<void (*)(void)>(&OnRegistryGlobalRemove)};

        // The application destroyed its wl_egl_window first: nothing may be read from it after this.
        void OnWindowDestroyed(void* data) { static_cast<WindowSurface::Impl*>(data)->window = nullptr; }
    } // namespace

    namespace {
        // Copies `size` bytes at `address` of this process, or answers false where nothing is
        // mapped - the kernel does the reading, so a wild pointer is an error code, not a fault.
        Bool ReadOwnMemory(const void* address, void* out, SizeT size) {
            iovec local{out, size};
            iovec remote{const_cast<void*>(address), size};
            return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
        }
    } // namespace

    Bool IsWaylandDisplay(Uint64 nativeDisplay, EGLenum platform) {
        if (platform == kPlatformWayland) return true;
        if (nativeDisplay == 0 || (platform != EGL_NONE && platform != 0)) return false;
        const Api& api = WaylandApi();
        if (!api.loaded) return false;
        const void* interface = *reinterpret_cast<const void* const*>(static_cast<uintptr_t>(nativeDisplay));
        if (interface == api.displayInterface) return true;
        // AN APPLICATION WITH ITS OWN COPY OF LIBWAYLAND-CLIENT (Chromium links one in statically)
        // has its own wl_display_interface, at its own address. The interface is still the
        // protocol's: a wl_interface whose first member names it "wl_display". Whatever else the
        // first word of a native display is (a gbm_device's is a function address), the probe
        // only reads through the kernel.
        const char* name = nullptr;
        char text[sizeof("wl_display")] = {};
        return interface != nullptr && ReadOwnMemory(interface, &name, sizeof(name)) && name != nullptr &&
               ReadOwnMemory(name, text, sizeof(text)) && std::memcmp(text, "wl_display", sizeof(text)) == 0;
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
        impl->fourcc = hasAlpha ? kDrmFormatAbgr8888 : kDrmFormatXbgr8888;
        impl->dmabufWanted = DmabufPresentationAllowed() && SharedImagesAvailable();
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
        // The first shared image is made here, so a server or compositor that cannot do the
        // dma-buf path is found out before the window shows anything, and the window starts on
        // wl_shm instead.
        if (impl->dmabuf != nullptr) impl->useDmabuf = impl->Allocate(impl->dmabufBuffers[0]);
        if (!impl->useDmabuf && impl->dmabuf != nullptr) {
            api.marshalFlags(impl->dmabuf, kDmabufDestroy, nullptr, api.getVersion(impl->dmabuf), kMarshalFlagDestroy);
            impl->dmabuf = nullptr;
        }
        impl->window->driver_private = impl.get();
        impl->window->destroy_window_callback = &OnWindowDestroyed;
        if (impl->useDmabuf) {
            MGLOG_I("Wayland: window surface %dx%d presented through linux-dmabuf shared images (%s)", width, height,
                    hasAlpha ? "ABGR8888" : "XBGR8888");
        } else {
            MGLOG_I("Wayland: window surface %dx%d presented through wl_shm (%s)", width, height,
                    hasAlpha ? "ARGB8888" : "XRGB8888");
        }
        return UniquePtr<WindowSurface>(new WindowSurface(std::move(impl)));
    }

    Bool WindowSurface::TakeResize(EGLint* width, EGLint* height) {
        Impl& impl = *m_impl;
        if (impl.window == nullptr || impl.window->width <= 0 || impl.window->height <= 0) return false;
        if (impl.window->width == impl.width && impl.window->height == impl.height) return false;
        impl.width = impl.window->width;
        impl.height = impl.window->height;
        *width = impl.width;
        *height = impl.height;
        return true;
    }

    Bool WindowSurface::Present() {
        Impl& impl = *m_impl;
        if (impl.window == nullptr) return false;

        if (impl.useDmabuf) {
            // The server copies the frame into the image GPU-side, top row first - a wl_buffer's
            // first row - and answers once the copy has completed, so the compositor reads a
            // finished frame.
            DmabufBuffer* buffer = impl.NextFreeDmabuf();
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            if (buffer != nullptr && backendObject != nullptr && backendObject->PresentToSharedImage(buffer->imageId)) {
                impl.Commit(buffer->buffer);
                buffer->busy = true;
                return true;
            }
            MGLOG_E_ONCE("Wayland: a frame could not be presented through a linux-dmabuf shared image; the window "
                         "falls back to wl_shm");
            impl.FallBackToShm();
        }

        ShmBuffer* buffer = impl.NextFree();
        if (buffer == nullptr) {
            MGLOG_E_ONCE("Wayland: no wl_shm buffer could be taken for the frame; it is not shown");
            return false;
        }

        // GL's rows run bottom-up and RGBA; a wl_shm buffer's run top-down and B, G, R, A.
        ReadBackFrameBGRA(impl.width, impl.height, impl.scratch, static_cast<Uint8*>(buffer->map),
                          static_cast<SizeT>(buffer->stride));
        impl.Commit(buffer->buffer);
        buffer->busy = true;
        return true;
    }
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland

#endif
