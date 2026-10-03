// MobileGL - MobileGL/MG_Impl/GLXImpl/X11PresentXcb.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "X11PresentXcb.h"

#if MOBILEGL_GLX_XCB_PRESENT

#include <MG_Util/Debug/Log.h>

#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace MobileGL::MG_Impl::GLXImpl::X11Present {
    namespace {
        // presentproto: ConfigureNotify's pixmap_flags when the window is gone (Present 1.2+).
        constexpr uint32_t kPresentWindowDestroyed = 1u << 0;

#define MGL_XCB_FN(name) decltype(&::name) name = nullptr
        struct XcbApi {
            Bool core = false, dri3 = false, present = false, xfixes = false, shm = false;
            // libxcb
            MGL_XCB_FN(xcb_generate_id);
            MGL_XCB_FN(xcb_flush);
            MGL_XCB_FN(xcb_get_extension_data);
            MGL_XCB_FN(xcb_request_check);
            MGL_XCB_FN(xcb_poll_for_reply);
            MGL_XCB_FN(xcb_discard_reply);
            MGL_XCB_FN(xcb_get_file_descriptor);
            MGL_XCB_FN(xcb_connection_has_error);
            MGL_XCB_FN(xcb_register_for_special_xge);
            MGL_XCB_FN(xcb_unregister_for_special_event);
            MGL_XCB_FN(xcb_poll_for_special_event);
            MGL_XCB_FN(xcb_free_pixmap_checked);
            MGL_XCB_FN(xcb_get_input_focus);
            MGL_XCB_FN(xcb_get_input_focus_reply);
            // libxcb-dri3
            xcb_extension_t* dri3Id = nullptr;
            MGL_XCB_FN(xcb_dri3_query_version);
            MGL_XCB_FN(xcb_dri3_query_version_reply);
            MGL_XCB_FN(xcb_dri3_pixmap_from_buffer_checked);
            MGL_XCB_FN(xcb_dri3_pixmap_from_buffers_checked);
            // libxcb-present
            xcb_extension_t* presentId = nullptr;
            MGL_XCB_FN(xcb_present_query_version);
            MGL_XCB_FN(xcb_present_query_version_reply);
            MGL_XCB_FN(xcb_present_select_input_checked);
            MGL_XCB_FN(xcb_present_pixmap_checked);
            // libxcb-xfixes
            xcb_extension_t* xfixesId = nullptr;
            MGL_XCB_FN(xcb_xfixes_query_version);
            MGL_XCB_FN(xcb_xfixes_query_version_reply);
            MGL_XCB_FN(xcb_xfixes_create_region);
            MGL_XCB_FN(xcb_xfixes_destroy_region);
            // libxcb-shm
            xcb_extension_t* shmId = nullptr;
            MGL_XCB_FN(xcb_shm_query_version);
            MGL_XCB_FN(xcb_shm_query_version_reply);
            MGL_XCB_FN(xcb_shm_attach_fd_checked);
            MGL_XCB_FN(xcb_shm_detach);
            MGL_XCB_FN(xcb_shm_put_image);
        };
#undef MGL_XCB_FN

        void* OpenLibrary(const char* name) {
            void* lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
            return lib != nullptr ? lib : dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }

        template <typename Fn>
        Bool Resolve(void* lib, const char* name, Fn& out) {
            out = lib != nullptr ? reinterpret_cast<Fn>(dlsym(lib, name)) : nullptr;
            return out != nullptr;
        }

        const XcbApi& Api() {
            static const XcbApi* api = [] {
                auto* a = new XcbApi();
                void* xcb = OpenLibrary("libxcb.so.1");
#define R(lib, fn) Resolve(lib, #fn, a->fn)
                a->core = R(xcb, xcb_generate_id) & R(xcb, xcb_flush) & R(xcb, xcb_get_extension_data) &
                          R(xcb, xcb_request_check) & R(xcb, xcb_poll_for_reply) & R(xcb, xcb_discard_reply) &
                          R(xcb, xcb_get_file_descriptor) & R(xcb, xcb_connection_has_error) &
                          R(xcb, xcb_register_for_special_xge) & R(xcb, xcb_unregister_for_special_event) &
                          R(xcb, xcb_poll_for_special_event) & R(xcb, xcb_free_pixmap_checked) &
                          R(xcb, xcb_get_input_focus) & R(xcb, xcb_get_input_focus_reply);
                if (a->core) {
                    void* dri3 = OpenLibrary("libxcb-dri3.so.0");
                    a->dri3Id = dri3 != nullptr ? static_cast<xcb_extension_t*>(dlsym(dri3, "xcb_dri3_id")) : nullptr;
                    a->dri3 = a->dri3Id != nullptr && R(dri3, xcb_dri3_query_version) &
                                                          R(dri3, xcb_dri3_query_version_reply) &
                                                          R(dri3, xcb_dri3_pixmap_from_buffer_checked);
                    // 1.2's multi-plane request is optional: an older libxcb-dri3 has only 1.0's.
                    (void)R(dri3, xcb_dri3_pixmap_from_buffers_checked);
                    void* present = OpenLibrary("libxcb-present.so.0");
                    a->presentId =
                        present != nullptr ? static_cast<xcb_extension_t*>(dlsym(present, "xcb_present_id")) : nullptr;
                    a->present = a->presentId != nullptr && R(present, xcb_present_query_version) &
                                                                R(present, xcb_present_query_version_reply) &
                                                                R(present, xcb_present_select_input_checked) &
                                                                R(present, xcb_present_pixmap_checked);
                    void* xfixes = OpenLibrary("libxcb-xfixes.so.0");
                    a->xfixesId =
                        xfixes != nullptr ? static_cast<xcb_extension_t*>(dlsym(xfixes, "xcb_xfixes_id")) : nullptr;
                    a->xfixes = a->xfixesId != nullptr && R(xfixes, xcb_xfixes_query_version) &
                                                              R(xfixes, xcb_xfixes_query_version_reply) &
                                                              R(xfixes, xcb_xfixes_create_region) &
                                                              R(xfixes, xcb_xfixes_destroy_region);
                    void* shm = OpenLibrary("libxcb-shm.so.0");
                    a->shmId = shm != nullptr ? static_cast<xcb_extension_t*>(dlsym(shm, "xcb_shm_id")) : nullptr;
                    a->shm = a->shmId != nullptr && R(shm, xcb_shm_query_version) &
                                                        R(shm, xcb_shm_query_version_reply) &
                                                        R(shm, xcb_shm_attach_fd_checked) & R(shm, xcb_shm_detach) &
                                                        R(shm, xcb_shm_put_image);
                }
#undef R
                return a;
            }();
            return *api;
        }

        xcb_connection_t* Xcb(void* connection) { return static_cast<xcb_connection_t*>(connection); }

        Bool ExtensionPresent(xcb_connection_t* c, xcb_extension_t* id) {
            const XcbApi& api = Api();
            if (id == nullptr) return false;
            const xcb_query_extension_reply_t* data = api.xcb_get_extension_data(c, id);
            return data != nullptr && data->present != 0;
        }

        struct CachedCaps {
            void* Connection = nullptr;
            ConnectionCaps Caps;
            Bool XFixes = false;
        };

        std::mutex& CapsMutex() {
            static auto* mutex = new std::mutex();
            return *mutex;
        }

        Vector<CachedCaps>& CapsCache() {
            static auto* cache = new Vector<CachedCaps>();
            return *cache;
        }

        CachedCaps Query(void* connection) {
            const std::lock_guard<std::mutex> lock(CapsMutex());
            for (const CachedCaps& cached : CapsCache()) {
                if (cached.Connection == connection) return cached;
            }
            CachedCaps result;
            result.Connection = connection;
            const XcbApi& api = Api();
            xcb_connection_t* c = Xcb(connection);
            if (!api.core || c == nullptr || api.xcb_connection_has_error(c)) return result;
            sockaddr_storage address{};
            socklen_t length = sizeof(address);
            if (::getsockname(api.xcb_get_file_descriptor(c), reinterpret_cast<sockaddr*>(&address), &length) == 0)
                result.Caps.LocalConnection = address.ss_family == AF_UNIX;
            if (api.dri3 && ExtensionPresent(c, api.dri3Id)) {
                if (auto* reply = api.xcb_dri3_query_version_reply(c, api.xcb_dri3_query_version(c, 1, 2), nullptr)) {
                    result.Caps.Dri3Major = reply->major_version;
                    result.Caps.Dri3Minor = reply->minor_version;
                    std::free(reply);
                }
            }
            if (api.present && ExtensionPresent(c, api.presentId)) {
                if (auto* reply =
                        api.xcb_present_query_version_reply(c, api.xcb_present_query_version(c, 1, 2), nullptr)) {
                    result.Caps.PresentMajor = reply->major_version;
                    result.Caps.PresentMinor = reply->minor_version;
                    std::free(reply);
                }
            }
            // XFixes demands its version be asked before any other request of it.
            if (api.xfixes && ExtensionPresent(c, api.xfixesId)) {
                if (auto* reply = api.xcb_xfixes_query_version_reply(c, api.xcb_xfixes_query_version(c, 5, 0), nullptr)) {
                    result.XFixes = reply->major_version >= 2;
                    std::free(reply);
                }
            }
            if (api.shm && ExtensionPresent(c, api.shmId)) {
                if (auto* reply = api.xcb_shm_query_version_reply(c, api.xcb_shm_query_version(c), nullptr)) {
                    result.Caps.ShmMajor = reply->major_version;
                    result.Caps.ShmMinor = reply->minor_version;
                    std::free(reply);
                }
            }
            CapsCache().push_back(result);
            return result;
        }

        Int64 NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        Bool Translate(const xcb_generic_event_t* raw, Event* out) {
            const auto* generic = reinterpret_cast<const xcb_present_generic_event_t*>(raw);
            *out = Event{};
            switch (generic->evtype) {
            case XCB_PRESENT_EVENT_IDLE_NOTIFY: {
                const auto* idle = reinterpret_cast<const xcb_present_idle_notify_event_t*>(raw);
                out->Type = Event::Kind::Idle;
                out->Serial = idle->serial;
                out->Pixmap = idle->pixmap;
                return true;
            }
            case XCB_PRESENT_EVENT_COMPLETE_NOTIFY: {
                const auto* complete = reinterpret_cast<const xcb_present_complete_notify_event_t*>(raw);
                if (complete->kind != XCB_PRESENT_COMPLETE_KIND_PIXMAP) return false;
                out->Type = Event::Kind::Complete;
                out->Serial = complete->serial;
                out->Msc = complete->msc;
                out->Mode = complete->mode;
                return true;
            }
            case XCB_PRESENT_EVENT_CONFIGURE_NOTIFY: {
                const auto* configure = reinterpret_cast<const xcb_present_configure_notify_event_t*>(raw);
                out->Type = Event::Kind::Configure;
                out->Width = configure->width;
                out->Height = configure->height;
                out->WindowDestroyed = (configure->pixmap_flags & kPresentWindowDestroyed) != 0;
                return true;
            }
            default:
                return false;
            }
        }

        class XcbConnection final : public Connection {
        public:
            XcbConnection(xcb_connection_t* c, Uint32 window, Bool buffers12, Bool xfixes)
                : m_c(c), m_window(window), m_buffers12(buffers12), m_xfixes(xfixes) {}

            Bool Init() {
                const XcbApi& api = Api();
                m_eid = api.xcb_generate_id(m_c);
                m_special = api.xcb_register_for_special_xge(m_c, Api().presentId, m_eid, &m_stamp);
                if (m_special == nullptr) return false;
                const xcb_void_cookie_t cookie = api.xcb_present_select_input_checked(
                    m_c, m_eid, m_window,
                    XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY | XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                        XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
                if (xcb_generic_error_t* error = api.xcb_request_check(m_c, cookie)) {
                    std::free(error);
                    return false;
                }
                return true;
            }

            ~XcbConnection() override {
                const XcbApi& api = Api();
                if (m_hasPendingPresent) api.xcb_discard_reply(m_c, m_pendingPresent);
                if (m_special != nullptr) {
                    const xcb_void_cookie_t cookie =
                        api.xcb_present_select_input_checked(m_c, m_eid, m_window, XCB_PRESENT_EVENT_MASK_NO_EVENT);
                    // The window may be gone already: an error here is expected and dropped.
                    api.xcb_discard_reply(m_c, cookie.sequence);
                    api.xcb_unregister_for_special_event(m_c, m_special);
                }
                api.xcb_flush(m_c);
            }

            Uint32 ImportPixmap(const Image& image, Uint32 depth) override {
                const XcbApi& api = Api();
                // libxcb closes the descriptor it sends.
                const int fd = ::fcntl(image.Fd, F_DUPFD_CLOEXEC, 0);
                if (fd < 0) return 0;
                const Uint32 pixmap = api.xcb_generate_id(m_c);
                xcb_void_cookie_t cookie{};
                if (m_buffers12 && api.xcb_dri3_pixmap_from_buffers_checked != nullptr) {
                    int32_t fds[1] = {fd};
                    cookie = api.xcb_dri3_pixmap_from_buffers_checked(
                        m_c, pixmap, m_window, 1, static_cast<uint16_t>(image.Width), static_cast<uint16_t>(image.Height),
                        image.Stride, image.Offset, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(depth), 32, image.Modifier, fds);
                } else {
                    if (image.Offset != 0 || image.Stride > 0xffffu) {
                        ::close(fd);
                        return 0;
                    }
                    cookie = api.xcb_dri3_pixmap_from_buffer_checked(
                        m_c, pixmap, m_window, image.Stride * image.Height, static_cast<uint16_t>(image.Width),
                        static_cast<uint16_t>(image.Height), static_cast<uint16_t>(image.Stride),
                        static_cast<uint8_t>(depth), 32, fd);
                }
                if (xcb_generic_error_t* error = api.xcb_request_check(m_c, cookie)) {
                    MGLOG_I("glx: the X server refused a %ux%u shared image as a pixmap (X error %u, major %u minor %u)",
                            image.Width, image.Height, error->error_code, error->major_code, error->minor_code);
                    std::free(error);
                    return 0;
                }
                return pixmap;
            }

            void FreePixmap(Uint32 pixmap) override {
                const XcbApi& api = Api();
                api.xcb_discard_reply(m_c, api.xcb_free_pixmap_checked(m_c, pixmap).sequence);
            }

            Bool PresentPixmap(Uint32 pixmap, Uint32 serial, Uint32 options, Uint64 targetMsc,
                               const MG_Util::Damage::Region& update) override {
                const XcbApi& api = Api();
                if (api.xcb_connection_has_error(m_c)) return false;
                Uint32 region = 0;
                if (!update.IsFull() && m_xfixes) {
                    Vector<xcb_rectangle_t> rects;
                    for (const MG_Util::Damage::Rect& rect : update.Rects()) {
                        rects.push_back({static_cast<int16_t>(rect.X), static_cast<int16_t>(rect.Y),
                                         static_cast<uint16_t>(rect.Width), static_cast<uint16_t>(rect.Height)});
                    }
                    if (!rects.empty()) {
                        region = api.xcb_generate_id(m_c);
                        api.xcb_xfixes_create_region(m_c, region, static_cast<uint32_t>(rects.size()), rects.data());
                    }
                }
                // An earlier present whose answer never came is given up on, not left queued.
                if (m_hasPendingPresent) api.xcb_discard_reply(m_c, m_pendingPresent);
                const xcb_void_cookie_t cookie =
                    api.xcb_present_pixmap_checked(m_c, m_window, pixmap, serial, 0, region, 0, 0, 0, 0, 0, options,
                                                   targetMsc, 0, 0, 0, nullptr);
                m_pendingPresent = cookie.sequence;
                m_hasPendingPresent = true;
                if (region != 0) api.xcb_xfixes_destroy_region(m_c, region);
                return true;
            }

            Bool PresentFailed() override {
                const XcbApi& api = Api();
                if (m_failed || !m_hasPendingPresent) return m_failed;
                void* reply = nullptr;
                xcb_generic_error_t* error = nullptr;
                // Known once anything later has come back (an event carries the sequence): no round trip.
                if (api.xcb_poll_for_reply(m_c, m_pendingPresent, &reply, &error) != 0) {
                    m_hasPendingPresent = false;
                    if (error != nullptr) {
                        MGLOG_I("glx: PresentPixmap failed (X error %u)", error->error_code);
                        m_failed = true;
                    }
                    std::free(reply);
                    std::free(error);
                }
                return m_failed;
            }

            Bool PollEvent(Event* out) override {
                const XcbApi& api = Api();
                for (;;) {
                    xcb_generic_event_t* raw = api.xcb_poll_for_special_event(m_c, m_special);
                    if (raw == nullptr) return false;
                    const Bool known = Translate(raw, out);
                    std::free(raw);
                    if (known) return true;
                }
            }

            // Short poll slices: another thread (the application's event loop) may be the one
            // reading the socket, and its read moves our events into the special queue.
            Bool WaitEvent(Event* out, Int64 timeoutMs) override {
                const XcbApi& api = Api();
                api.xcb_flush(m_c);
                const Int64 deadline = NowMs() + timeoutMs;
                for (;;) {
                    if (PollEvent(out)) return true;
                    if (api.xcb_connection_has_error(m_c)) return false;
                    const Int64 remaining = deadline - NowMs();
                    if (remaining <= 0) return false;
                    pollfd p{api.xcb_get_file_descriptor(m_c), POLLIN, 0};
                    (void)::poll(&p, 1, static_cast<int>(std::min<Int64>(remaining, 4)));
                }
            }

            void Flush() override { Api().xcb_flush(m_c); }

        private:
            xcb_connection_t* m_c = nullptr;
            Uint32 m_window = 0;
            Bool m_buffers12 = false;
            Bool m_xfixes = false;
            Uint32 m_eid = 0;
            uint32_t m_stamp = 0;
            xcb_special_event_t* m_special = nullptr;
            unsigned int m_pendingPresent = 0;
            Bool m_hasPendingPresent = false;
            Bool m_failed = false;
        };

        class XcbShmPresenter final : public ShmPresenter {
        public:
            explicit XcbShmPresenter(xcb_connection_t* c) : m_c(c) {}
            ~XcbShmPresenter() override {
                for (Segment& segment : m_segments) Release(segment);
                Api().xcb_flush(m_c);
            }

            Bool Put(Uint32 drawable, Uint32 gc, Uint32 depth, Int32 width, Int32 height,
                     const std::function<void(Uint8*, SizeT)>& fill) override {
                const XcbApi& api = Api();
                if (width <= 0 || height <= 0 || width > 0xffff || height > 0xffff) return false;
                Segment& segment = m_segments[m_next];
                m_next = (m_next + 1) % 2;
                // The X server may still be reading this segment's last frame: the round trip queued
                // behind that put coming back says it is done with it.
                if (segment.HasSync) {
                    std::free(api.xcb_get_input_focus_reply(m_c, segment.Sync, nullptr));
                    segment.HasSync = false;
                }
                const SizeT stride = static_cast<SizeT>(width) * 4;
                const SizeT size = stride * static_cast<SizeT>(height);
                if (segment.Size < size && !Make(segment, size)) return false;
                fill(static_cast<Uint8*>(segment.Map), stride);
                api.xcb_shm_put_image(m_c, drawable, gc, static_cast<uint16_t>(width), static_cast<uint16_t>(height), 0,
                                      0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), 0, 0,
                                      static_cast<uint8_t>(depth), XCB_IMAGE_FORMAT_Z_PIXMAP, 0, segment.Seg, 0);
                segment.Sync = api.xcb_get_input_focus(m_c);
                segment.HasSync = true;
                api.xcb_flush(m_c);
                return true;
            }

        private:
            struct Segment {
                Uint32 Seg = 0;
                void* Map = nullptr;
                SizeT Size = 0;
                xcb_get_input_focus_cookie_t Sync{};
                Bool HasSync = false;
            };

            void Release(Segment& segment) {
                const XcbApi& api = Api();
                if (segment.HasSync) std::free(api.xcb_get_input_focus_reply(m_c, segment.Sync, nullptr));
                if (segment.Seg != 0) api.xcb_shm_detach(m_c, segment.Seg);
                if (segment.Map != nullptr) ::munmap(segment.Map, segment.Size);
                segment = Segment{};
            }

            Bool Make(Segment& segment, SizeT size) {
                const XcbApi& api = Api();
                Release(segment);
                const int fd = ::memfd_create("mobilegl-glx-shm", MFD_CLOEXEC);
                if (fd < 0) return false;
                if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
                    ::close(fd);
                    return false;
                }
                void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (map == MAP_FAILED) {
                    ::close(fd);
                    return false;
                }
                const Uint32 seg = api.xcb_generate_id(m_c);
                // libxcb sends, then closes, the descriptor; the mapping stays.
                const xcb_void_cookie_t cookie = api.xcb_shm_attach_fd_checked(m_c, seg, fd, 1);
                if (xcb_generic_error_t* error = api.xcb_request_check(m_c, cookie)) {
                    std::free(error);
                    ::munmap(map, size);
                    return false;
                }
                segment.Seg = seg;
                segment.Map = map;
                segment.Size = size;
                return true;
            }

            xcb_connection_t* m_c = nullptr;
            Segment m_segments[2];
            SizeT m_next = 0;
        };
    } // namespace

    ConnectionCaps QueryConnectionCaps(void* xcbConnection) { return Query(xcbConnection).Caps; }

    UniquePtr<Connection> CreateXcbConnection(void* xcbConnection, Uint32 window, const ConnectionCaps& caps) {
        const XcbApi& api = Api();
        if (!api.core || !api.dri3 || !api.present || caps.Dri3Major < 1 || caps.PresentMajor < 1) return nullptr;
        const CachedCaps cached = Query(xcbConnection);
        const Bool buffers12 = caps.Dri3Major > 1 || (caps.Dri3Major == 1 && caps.Dri3Minor >= 2);
        auto connection = MakeUnique<XcbConnection>(Xcb(xcbConnection), window, buffers12, cached.XFixes);
        if (!connection->Init()) return nullptr;
        return connection;
    }

    UniquePtr<ShmPresenter> CreateShmPresenter(void* xcbConnection) {
        const XcbApi& api = Api();
        if (!api.core || !api.shm || xcbConnection == nullptr) return nullptr;
        return MakeUnique<XcbShmPresenter>(Xcb(xcbConnection));
    }
} // namespace MobileGL::MG_Impl::GLXImpl::X11Present

#else // !MOBILEGL_GLX_XCB_PRESENT

namespace MobileGL::MG_Impl::GLXImpl::X11Present {
    ConnectionCaps QueryConnectionCaps(void*) { return {}; }
    UniquePtr<Connection> CreateXcbConnection(void*, Uint32, const ConnectionCaps&) { return nullptr; }
    UniquePtr<ShmPresenter> CreateShmPresenter(void*) { return nullptr; }
} // namespace MobileGL::MG_Impl::GLXImpl::X11Present

#endif
