// MobileGL - MobileGL/MG_Impl/GLXImpl/X11Present.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "X11Present.h"

#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace MobileGL::MG_Impl::GLXImpl::X11Present {
    namespace {
        void CloseDescriptor(int fd) {
#if !defined(_WIN32)
            if (fd >= 0) ::close(fd);
#else
            (void)fd;
#endif
        }
    } // namespace

    const char* PathName(Path path) {
        switch (path) {
        case Path::Dri3Present:
            return "DRI3+Present shared images";
        case Path::ShmPutImage:
            return "readback + MIT-SHM PutImage";
        case Path::PutImage:
            return "readback + PutImage";
        }
        return "?";
    }

    Uint32 FourccForDepth(Uint32 depth) {
        switch (depth) {
        case 24:
            return kFourccXrgb8888;
        case 32:
            return kFourccArgb8888;
        default:
            return 0;
        }
    }

    Path SelectPath(const ConnectionCaps& caps, Uint32 depth, const char* overrideValue) {
        const Bool putImageOnly = overrideValue != nullptr && std::strcmp(overrideValue, "putimage") == 0;
        const Bool noDri3 = putImageOnly || (overrideValue != nullptr && (std::strcmp(overrideValue, "readback") == 0 ||
                                                                         std::strcmp(overrideValue, "shm") == 0));
        if (!noDri3 && caps.SharedImages && caps.LocalConnection && FourccForDepth(depth) != 0 &&
            caps.Dri3Major >= 1 && caps.PresentMajor >= 1) {
            return Path::Dri3Present;
        }
        // MIT-SHM 1.2 is the first with a client-made segment passed as a descriptor (no SysV
        // shared memory, which a container need not share with the X server).
        const Bool shmFd = caps.ShmMajor > 1 || (caps.ShmMajor == 1 && caps.ShmMinor >= 2);
        if (!putImageOnly && caps.LocalConnection && shmFd) return Path::ShmPutImage;
        return Path::PutImage;
    }

    Chain::Chain(Connection& connection, ImageSource& images, Uint32 depth, Int32 width, Int32 height,
                 std::function<Int64()> nowMs)
        : m_connection(connection), m_images(images), m_depth(depth), m_fourcc(FourccForDepth(depth)),
          m_width(width > 0 ? width : 1), m_height(height > 0 ? height : 1), m_nowMs(std::move(nowMs)) {
        if (m_fourcc == 0) Break("the window's depth has no shared-image format");
    }

    Chain::~Chain() {
        for (Buffer& buffer : m_buffers) Drop(buffer);
        m_connection.Flush();
    }

    void Chain::Break(const char* reason) {
        if (m_broken) return;
        m_broken = true;
        m_brokenReason = reason;
    }

    SizeT Chain::AllocatedBuffers() const {
        SizeT count = 0;
        for (const Buffer& buffer : m_buffers) count += buffer.Pixmap != 0 ? 1 : 0;
        return count;
    }

    SizeT Chain::BusyBuffers() const {
        SizeT count = 0;
        for (const Buffer& buffer : m_buffers) count += buffer.Pixmap != 0 && buffer.Busy ? 1 : 0;
        return count;
    }

    void Chain::Drop(Buffer& buffer) {
        // The X server keeps a pixmap a pending present still names alive past FreePixmap, and the
        // X server's own import of the image holds the image on the MobileGL server: both may go.
        if (buffer.Pixmap != 0) m_connection.FreePixmap(buffer.Pixmap);
        if (buffer.Storage.Id != 0) m_images.Release(buffer.Storage.Id);
        CloseDescriptor(buffer.Storage.Fd);
        m_damage.Invalidate(static_cast<SizeT>(&buffer - m_buffers));
        buffer = Buffer{};
    }

    Bool Chain::Allocate(Buffer& buffer) {
        Drop(buffer);
        Image image;
        if (!m_images.Allocate(static_cast<Uint32>(m_width), static_cast<Uint32>(m_height), m_fourcc, &image) ||
            image.Id == 0) {
            CloseDescriptor(image.Fd);
            Break("the server allocated no shared image for the window");
            return false;
        }
        if (image.Fd < 0 || image.Width != static_cast<Uint32>(m_width) || image.Height != static_cast<Uint32>(m_height)) {
            CloseDescriptor(image.Fd);
            m_images.Release(image.Id);
            Break("a shared image came back without a descriptor or at another size");
            return false;
        }
        const Uint32 pixmap = m_connection.ImportPixmap(image, m_depth);
        // The pixmap holds the memory now (and the X server's own import, the image).
        CloseDescriptor(image.Fd);
        image.Fd = -1;
        if (pixmap == 0) {
            m_images.Release(image.Id);
            Break("the X server refused the shared image as a DRI3 pixmap");
            return false;
        }
        buffer.Storage = image;
        buffer.Pixmap = pixmap;
        buffer.Busy = false;
        buffer.Serial = 0;
        m_damage.Invalidate(static_cast<SizeT>(&buffer - m_buffers));
        return true;
    }

    void Chain::Handle(const Event& event) {
        switch (event.Type) {
        case Event::Kind::Idle:
            for (Buffer& buffer : m_buffers) {
                if (buffer.Pixmap != 0 && buffer.Pixmap == event.Pixmap) buffer.Busy = false;
            }
            break;
        case Event::Kind::Complete:
            if (event.Msc != 0) m_lastMsc = event.Msc;
            if (m_throttle.Outstanding() && event.Serial == m_awaitedSerial) m_throttle.Done();
            break;
        case Event::Kind::Configure:
            if (event.WindowDestroyed) {
                Break("the window was destroyed");
                break;
            }
            if (event.Width > 0 && event.Height > 0) {
                m_configured = true;
                m_configuredWidth = event.Width;
                m_configuredHeight = event.Height;
            }
            break;
        case Event::Kind::None:
            break;
        }
    }

    void Chain::DrainEvents() {
        Event event;
        while (m_connection.PollEvent(&event)) Handle(event);
    }

    void Chain::WaitForPacing(Int swapInterval) {
        const Int64 budget = m_throttle.WaitBudgetMs(swapInterval, m_nowMs());
        if (budget == 0) return;
        const Int64 limit = budget == EGLImpl::Wayland::FrameThrottle::kForever ? kCompleteGiveUpMs : budget;
        const Int64 deadline = m_nowMs() + limit;
        Event event;
        while (m_throttle.Outstanding() && !m_broken) {
            const Int64 remaining = deadline - m_nowMs();
            if (remaining <= 0) {
                // Interval 0's bounded wait simply ends; a paced wait gives the completion up so a
                // server that lost it does not stall the application forever.
                if (budget == EGLImpl::Wayland::FrameThrottle::kForever) m_throttle.Done();
                return;
            }
            if (m_connection.WaitEvent(&event, remaining)) Handle(event);
        }
    }

    Chain::Buffer* Chain::NextFree() {
        const Int64 deadline = m_nowMs() + kIdleWaitMs;
        for (;;) {
            if (m_broken) return nullptr;
            // The free buffer presented longest ago, so the images take turns.
            Buffer* best = nullptr;
            for (Buffer& buffer : m_buffers) {
                if (buffer.Pixmap == 0 || buffer.Busy) continue;
                if (best == nullptr || buffer.Serial < best->Serial) best = &buffer;
            }
            if (best != nullptr) {
                const Bool sized = best->Storage.Width == static_cast<Uint32>(m_width) &&
                                   best->Storage.Height == static_cast<Uint32>(m_height);
                if (sized || Allocate(*best)) return best;
                return nullptr;
            }
            SizeT allocated = AllocatedBuffers();
            if (allocated < kBuffers) {
                for (Buffer& buffer : m_buffers) {
                    if (buffer.Pixmap == 0) return Allocate(buffer) ? &buffer : nullptr;
                }
            }
            // Every buffer is with the X server: wait for one to come back.
            const Int64 remaining = deadline - m_nowMs();
            Event event;
            if (remaining > 0 && m_connection.WaitEvent(&event, remaining)) {
                Handle(event);
                continue;
            }
            // None came back in time: one more buffer, up to the limit; past it the frame is
            // dropped (the chain is not broken: the server may just be slow).
            if (allocated < kMaxBuffers) {
                for (Buffer& buffer : m_buffers) {
                    if (buffer.Pixmap == 0) return Allocate(buffer) ? &buffer : nullptr;
                }
            }
            return nullptr;
        }
    }

    Bool Chain::Present(const MG_Util::Damage::Region& damage, Int swapInterval) {
        if (m_broken) return false;
        DrainEvents();
        if (m_connection.PresentFailed()) Break("the X server refused a PresentPixmap");
        if (m_broken) return false;
        WaitForPacing(swapInterval);
        if (m_broken) return false;

        Buffer* buffer = NextFree();
        if (buffer == nullptr) return false;
        const SizeT index = static_cast<SizeT>(buffer - m_buffers);
        const MG_Util::Damage::Region copy = m_damage.TakeForWrite(index, damage, m_width, m_height);
        if (!m_images.CopyFrame(buffer->Storage.Id, copy)) {
            Break("the frame could not be copied into a shared image");
            return false;
        }

        const Uint32 serial = ++m_serial;
        const Uint32 options = swapInterval > 0 ? 0u : kPresentOptionAsync;
        const Uint64 targetMsc = swapInterval > 0 && m_lastMsc != 0 ? m_lastMsc + static_cast<Uint64>(swapInterval) : 0;
        MG_Util::Damage::Region update = MG_Util::Damage::Region::Full();
        if (!damage.IsFull()) update = damage.FlippedY(m_height);
        if (!m_connection.PresentPixmap(buffer->Pixmap, serial, options, targetMsc, update)) {
            Break("PresentPixmap could not be sent");
            return false;
        }
        buffer->Busy = true;
        buffer->Serial = serial;
        if (m_throttle.WantsFrameRequest()) {
            m_throttle.Requested(m_nowMs());
            m_awaitedSerial = serial;
        }
        m_connection.Flush();
        ++m_framesPresented;
        return true;
    }

    void Chain::Resize(Int32 width, Int32 height) {
        if (width <= 0 || height <= 0 || (width == m_width && height == m_height)) return;
        m_width = width;
        m_height = height;
        // Old-size buffers the X server is not holding go now; the rest as they come back
        // (NextFree reallocates a free buffer of the wrong size).
        for (Buffer& buffer : m_buffers) {
            if (buffer.Pixmap != 0 && !buffer.Busy) Drop(buffer);
        }
        m_damage.InvalidateAll();
    }

    Bool Chain::TakeConfiguredSize(Int32* width, Int32* height) {
        if (!m_configured) return false;
        m_configured = false;
        *width = m_configuredWidth;
        *height = m_configuredHeight;
        return true;
    }
} // namespace MobileGL::MG_Impl::GLXImpl::X11Present
