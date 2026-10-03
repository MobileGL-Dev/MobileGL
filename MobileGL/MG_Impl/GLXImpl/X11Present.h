// MobileGL - MobileGL/MG_Impl/GLXImpl/X11Present.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// GLX WINDOWS PRESENTED THROUGH DRI3 + PRESENT (docs/Disaggregated/notes/anland/plan-x11-gpu.md).
//
// A GLX window's frames are drawn by the server into a pbuffer. Instead of reading each frame back
// and putting it on the window, the frame is copied GPU-side into one of a few server-allocated
// shared images, each imported into the X server once as a pixmap (DRI3 PixmapFromBuffer(s), the
// image's dma-buf), and presented with Present PresentPixmap. An X server drawing with MobileGL
// (Xwayland's glamor) then shows the very same image - attached to its window as a linux-dmabuf
// buffer, or copied on its GPU - and no frame crosses the CPU.
//
// The logic is here, behind two narrow interfaces: Connection (the X side of one window: the DRI3
// import, PresentPixmap, the Present events of a special-event queue) and ImageSource (the server's
// shared images and the GPU copy). X11PresentXcb.cpp implements Connection over libxcb; the host
// tests drive Chain with fakes of both.
//
//   Buffers. Up to kBuffers images of the window's size (kMaxBuffers when the X server has held
//   every one past kIdleWaitMs); a buffer is busy from its PresentPixmap to its IdleNotify. A
//   resize drops each old-size buffer as it comes back idle.
//   Pacing. FrameThrottle's rules (EGLImpl/FrameThrottle.h) on CompleteNotify: interval >= 1 waits
//   for the previous frame's completion and targets last_msc + interval; interval 0 presents
//   PresentOptionAsync, unpaced while completions come, slowed to about one frame a second once
//   one is overdue (the X server shows nothing of the window).
//   Errors. A refused import, a failed PresentPixmap, a failed copy or the window's destruction
//   break the chain for good; its owner falls back to the readback path.

#include <Includes.h>
#include <MG_Util/Damage/Damage.h>

#include "../EGLImpl/FrameThrottle.h"

#include <functional>

namespace MobileGL::MG_Impl::GLXImpl::X11Present {
    inline constexpr Uint64 kModifierInvalid = 0x00ffffffffffffffull;
    // DRM fourccs: what an X server drawing through GBM imports a depth-24/32 pixmap as.
    inline constexpr Uint32 kFourccXrgb8888 = 0x34325258u; // 'XR24'
    inline constexpr Uint32 kFourccArgb8888 = 0x34325241u; // 'AR24'
    // Present option bits (presentproto).
    inline constexpr Uint32 kPresentOptionAsync = 1u;

    // ---- which path a window's frames take -------------------------------------------------------

    enum class Path { Dri3Present, ShmPutImage, PutImage };
    const char* PathName(Path path);

    struct ConnectionCaps {
        Bool SharedImages = false;  // the backend allocates shared images (a split client)
        Bool LocalConnection = false; // a unix socket: descriptors can be passed
        Uint32 Dri3Major = 0, Dri3Minor = 0;
        Uint32 PresentMajor = 0, PresentMinor = 0;
        Uint32 ShmMajor = 0, ShmMinor = 0;
    };

    // The fourcc a window of `depth` is presented in; 0 for a depth this cannot present.
    Uint32 FourccForDepth(Uint32 depth);

    // DRI3+Present when the connection has both, descriptors can travel, the backend has shared
    // images and the depth is 24/32; otherwise MIT-SHM >= 1.2 (fd attach) when local; otherwise
    // PutImage. `overrideValue` (MOBILEGL_GLX_PRESENT): "readback" never takes DRI3, "putimage"
    // takes neither DRI3 nor MIT-SHM; anything else is automatic.
    Path SelectPath(const ConnectionCaps& caps, Uint32 depth, const char* overrideValue);

    // ---- the two sides -------------------------------------------------------------------------

    struct Image {
        Uint64 Id = 0;
        int Fd = -1;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Stride = 0;
        Uint32 Offset = 0;
        Uint32 Fourcc = 0;
        Uint64 Modifier = kModifierInvalid;
    };

    class ImageSource {
    public:
        virtual ~ImageSource() = default;
        // A new image; `out->Fd` is the caller's.
        virtual Bool Allocate(Uint32 width, Uint32 height, Uint32 fourcc, Image* out) = 0;
        virtual void Release(Uint64 id) = 0;
        // The current frame into the image, top row first; `region` in GL window coordinates (Full
        // = all). True once the copy is submitted and its fence published.
        virtual Bool CopyFrame(Uint64 id, const MG_Util::Damage::Region& region) = 0;
    };

    struct Event {
        enum class Kind { None, Idle, Complete, Configure };
        Kind Type = Kind::None;
        Uint32 Serial = 0;
        Uint32 Pixmap = 0;
        Uint64 Msc = 0;
        Uint8 Mode = 0;
        Int32 Width = 0;
        Int32 Height = 0;
        Bool WindowDestroyed = false;
    };

    class Connection {
    public:
        virtual ~Connection() = default;
        // A pixmap of `depth` whose storage is the image (its descriptor is duplicated for the
        // request; the caller keeps its own). 0 = the X server refused it.
        virtual Uint32 ImportPixmap(const Image& image, Uint32 depth) = 0;
        virtual void FreePixmap(Uint32 pixmap) = 0;
        // Queues a PresentPixmap. `update` is in X coordinates (top-left origin); Full = all of it.
        // False when it could not even be sent.
        virtual Bool PresentPixmap(Uint32 pixmap, Uint32 serial, Uint32 options, Uint64 targetMsc,
                                   const MG_Util::Damage::Region& update) = 0;
        // Whether a PresentPixmap sent earlier is now known to have failed (an X error).
        virtual Bool PresentFailed() = 0;
        virtual Bool PollEvent(Event* out) = 0;
        // Waits up to `timeoutMs` for an event. False: none came (or the connection broke).
        virtual Bool WaitEvent(Event* out, Int64 timeoutMs) = 0;
        virtual void Flush() = 0;
    };

    // ---- one window's swap chain ----------------------------------------------------------------

    class Chain {
    public:
        static constexpr SizeT kBuffers = 3;
        static constexpr SizeT kMaxBuffers = 4;
        static constexpr Int64 kIdleWaitMs = 1000;
        // Longest a swap waits for a completion before it stops pacing on it (a server that lost it).
        static constexpr Int64 kCompleteGiveUpMs = 5000;

        // `nowMs`: a monotonic clock in milliseconds (injected for the tests).
        Chain(Connection& connection, ImageSource& images, Uint32 depth, Int32 width, Int32 height,
              std::function<Int64()> nowMs);
        ~Chain();
        Chain(const Chain&) = delete;
        Chain& operator=(const Chain&) = delete;

        // Presents the current frame. `damage` is what it changed (GL window coordinates; Full = all).
        // False: the frame was not shown this way - Broken() says whether that is for good.
        Bool Present(const MG_Util::Damage::Region& damage, Int swapInterval);

        // The window has a new size: the next frame goes into images of that size.
        void Resize(Int32 width, Int32 height);
        // A size the X server reported (ConfigureNotify) that the owner has not taken yet.
        Bool TakeConfiguredSize(Int32* width, Int32* height);

        Bool Broken() const { return m_broken; }
        const char* BrokenReason() const { return m_brokenReason; }

        // For the tests and the log.
        SizeT AllocatedBuffers() const;
        SizeT BusyBuffers() const;
        Uint64 FramesPresented() const { return m_framesPresented; }
        Int32 Width() const { return m_width; }
        Int32 Height() const { return m_height; }

    private:
        struct Buffer {
            Image Storage;
            Uint32 Pixmap = 0;
            Bool Busy = false;
            Uint32 Serial = 0;
        };

        void Break(const char* reason);
        void Handle(const Event& event);
        void DrainEvents();
        void WaitForPacing(Int swapInterval);
        Buffer* NextFree();
        Bool Allocate(Buffer& buffer);
        void Drop(Buffer& buffer);

        Connection& m_connection;
        ImageSource& m_images;
        Uint32 m_depth = 24;
        Uint32 m_fourcc = kFourccXrgb8888;
        Int32 m_width = 0;
        Int32 m_height = 0;
        std::function<Int64()> m_nowMs;
        Buffer m_buffers[kMaxBuffers];
        MG_Util::Damage::BufferDamageTracker m_damage{kMaxBuffers};
        EGLImpl::Wayland::FrameThrottle m_throttle;
        Uint32 m_awaitedSerial = 0;
        Uint32 m_serial = 0;
        Uint64 m_lastMsc = 0;
        Uint64 m_framesPresented = 0;
        Bool m_configured = false;
        Int32 m_configuredWidth = 0;
        Int32 m_configuredHeight = 0;
        Bool m_broken = false;
        const char* m_brokenReason = "";
    };
} // namespace MobileGL::MG_Impl::GLXImpl::X11Present
