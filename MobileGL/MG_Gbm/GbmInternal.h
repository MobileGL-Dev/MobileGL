// MobileGL - MobileGL/MG_Gbm/GbmInternal.h
// SPDX-License-Identifier: LGPL-3.0-only
//
// This header carries the internal model shared by the GBM front-end files: the
// device, buffer-object and surface records, the two allocation backends, and the
// wire protocol used to talk to a phone that owns the real display hardware.
//
// The reason a GBM implementation has to exist in this tree at all is that the
// container half of MobileGL runs a Wayland compositor (kwin). That compositor
// needs libgbm to allocate the buffers it renders into, and the fd of such a
// buffer is handed to the Anland display producer, which ships the frame to the
// phone. If Mesa's libgbm is loaded, the compositor renders into Mesa buffers and
// the phone ends up receiving a copy through a path that was never designed to
// carry them. When this library is loaded instead, the buffers are either frames
// the phone itself offered to us, or buffers allocated from the kernel's dma-heap
// so that they are dma-bufs the display host can consume directly.
//
// The ownership rules below are the whole contract, because every one of them is
// observable from outside this library:
//
//   * A buffer record owns its descriptors. gbm_bo_get_fd and
//     gbm_bo_get_fd_for_plane hand out a dup, so a caller that closes what it was
//     given cannot invalidate the buffer, and the matching close happens exactly
//     once, in ~GbmBufferObject.
//   * A mapping is one mmap of the buffer's fd, counted rather than duplicated, so
//     two overlapping gbm_bo_map calls share one address and the last matching
//     gbm_bo_unmap releases it.
//   * A surface owns one reference per swapchain buffer, and every
//     gbm_surface_lock_front_buffer hands the caller a reference of its own. That
//     is what makes gbm_bo_destroy on a locked buffer harmless: it drops the
//     caller's reference and leaves the surface's bookkeeping intact.
//   * One mutex per device. GBM calls arrive from the compositor's render thread
//     and from its main thread, and the state they touch (the buffer table, the
//     swapchains, the backends) is small enough that one lock is cheaper than
//     several - and, more importantly, one lock cannot be taken in the wrong order.
//
// The helpers declared at the bottom of this file are the entire internal interface
// between GbmDevice.cpp (the records and the self-allocated backend) and GbmApi.cpp
// (the extern "C" surface). Every one of them assumes the device mutex is already
// held: the lock is taken once, in the API layer, so that a compositor call can
// never interleave with another half way through a swapchain update.

#pragma once

#include <Includes.h>

#include <gbm.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MobileGL::MG_Gbm
{
    // DRM_FORMAT_MOD_LINEAR and DRM_FORMAT_MOD_INVALID. drm_fourcc.h is not part of
    // libgbm's interface, and these two values are frozen kernel UAPI, so they are
    // spelled out here rather than made a build dependency on libdrm's headers.
    static constexpr Uint64 DrmFormatModLinear = 0ULL;
    static constexpr Uint64 DrmFormatModInvalid = 0x00ffffffffffffffULL;

    // Every buffer this library hands out is described by one of these. HostFrame
    // buffers are owned by the phone and only borrowed by us, so they must be
    // released back over the channel instead of being freed locally.
    enum class GbmBackend
    {
        None = 0,
        HostFrame,
        DmaHeap,
        MemFd,
    };

    // The phone offers frames with this packed struct immediately followed by an
    // AHardwareBuffer native handle in the payload of one message, with the handle's
    // descriptors in the SCM_RIGHTS control data of that same message. The layout is
    // fixed by the phone side, so the packing attribute is load bearing and the field
    // order may not be rearranged.
    //
    // The transport below the two structs is SOCK_SEQPACKET, and that is part of the
    // protocol rather than an implementation detail: a frame and its descriptors only
    // mean anything together, and the message boundary is what lets the reader take
    // the two as one thing. A stream socket would let a reader receive half an offer,
    // or a descriptor whose position in the byte stream it had to reconstruct, and the
    // failure mode of getting that arithmetic wrong is a frame silently made of
    // another frame's memory.
    //
    // Format is a DRM fourcc and not an AHardwareBuffer format, because it is what
    // gbm_bo_get_format has to answer with and a compositor compares it against
    // DRM_FORMAT_* values. StrideBytes is the byte distance between two rows of plane
    // 0, which is the same single-stride description GBM_BO_IMPORT_FD carries - and
    // the reason a frame read from this struct is reported as linear.
    struct GbmFrameOffer
    {
        uint32_t Magic; // 'M','G','G','F' little endian on both ends of the wire
        uint32_t Version;
        uint32_t Seq;
        uint32_t Index;
        uint32_t Width;
        uint32_t Height;
        uint32_t StrideBytes;
        uint32_t Format;
        uint32_t UsageLo;
        uint32_t UsageHi;
    } __attribute__((packed));

    // The container replies with this once it no longer reads the frame, which is
    // what lets the phone recycle the slot. Without it the display host would run
    // out of buffers after a handful of frames.
    struct GbmFrameRelease
    {
        uint32_t Magic; // 'M','G','G','R'
        uint32_t Version;
        uint32_t Seq;
    } __attribute__((packed));

    static constexpr uint32_t GbmFrameOfferMagic = 0x4647474Du;   // "MGGF"
    static constexpr uint32_t GbmFrameReleaseMagic = 0x5247474Du; // "MGGR"
    static constexpr uint32_t GbmHostProtocolVersion = 1;

    // An AHardwareBuffer native handle as it appears on the wire: three counts
    // followed by numFds file descriptors and numInts integers. The counts are
    // 32 bit even on 64 bit Android, which is why they are spelled out here
    // rather than using the platform's native_handle_t.
    struct AHBNativeHandleHeader
    {
        int32_t Version;
        int32_t NumFds;
        int32_t NumInts;
    } __attribute__((packed));

    static constexpr uint32_t GbmMaxPlanes = 4;

    // How one plane of a buffer is laid out inside the single allocation that backs
    // it. Multi-plane formats keep every plane in one dma-buf and describe the
    // separation with Offsets, which is the layout a modifier of
    // DRM_FORMAT_MOD_LINEAR can express and the only one this library produces.
    struct GbmPlaneLayout
    {
        Uint32 Stride = 0;
        Uint32 Offset = 0;
        SizeT Size = 0;
    };

    // One frame the phone offered, as this process holds it: the offer's own
    // description of the memory, plus the descriptor this library duped out of the
    // handle the phone sent. It owns that descriptor for as long as it lives, so a
    // caller that takes a frame and then fails still closes it, and the descriptor the
    // phone sent is closed with the rest of the message's descriptors rather than kept.
    struct GbmHostFrame
    {
        Uint32 Seq = 0;
        Uint32 Index = 0;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 StrideBytes = 0;
        Uint32 Format = 0;
        // Bytes per pixel of plane 0. The offer carries a fourcc, so it comes from the
        // format table when there is an entry for it and from the stride otherwise.
        Uint32 BytesPerPixel = 0;
        Uint64 Usage = 0;
        // Which connection offered the frame. A release names a frame, and a frame only
        // means anything to the connection that offered it.
        Uint64 Generation = 0;
        // The duped dma-buf descriptor, and how many bytes it actually backs as asked
        // of the descriptor itself rather than computed from the offered geometry.
        Int Fd = -1;
        SizeT Size = 0;

        // Move-only, because two records naming one descriptor would close it twice.
        GbmHostFrame() = default;
        GbmHostFrame(const GbmHostFrame&) = delete;
        GbmHostFrame& operator=(const GbmHostFrame&) = delete;
        GbmHostFrame(GbmHostFrame&& other) noexcept;
        GbmHostFrame& operator=(GbmHostFrame&& other) noexcept;
        ~GbmHostFrame();
    };

    // One imported or locally allocated buffer. The struct is private to this
    // library; gbm_bo is the public opaque name and is defined in terms of it.
    struct GbmBufferObject
    {
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Format = 0;
        Uint32 Stride = 0;
        Uint64 Modifier = DrmFormatModLinear;
        Uint64 Usage = 0;
        Int PlaneCount = 1;
        // Bytes per pixel of plane 0. Kept beside the layout because gbm_bo_map
        // needs it to place a sub-rectangle, and because a format this library can
        // allocate but cannot describe in bytes would be a format it must refuse.
        Uint32 BytesPerPixel = 0;

        // Fds owned by this record. get_fd and get_fd_for_plane hand out dups so
        // that a caller closing its fd can never invalidate the buffer. Index 0 is
        // the only one a self-allocated buffer has: every plane of a linear image
        // lives in that one dma-buf, and Fds[1..] stay -1 for it. An import with
        // several descriptors fills several entries, and the destructor closes
        // every entry that is not -1.
        Int Fds[GbmMaxPlanes] = {-1, -1, -1, -1};
        Uint32 Strides[GbmMaxPlanes] = {0, 0, 0, 0};
        Uint32 Offsets[GbmMaxPlanes] = {0, 0, 0, 0};
        Uint64 Modifiers[GbmMaxPlanes] = {0, 0, 0, 0};

        // The number of bytes the descriptors actually back, rounded up to the page
        // size because that is what both backends allocate. It is the only length
        // that is safe to mmap: a mapping longer than the object faults on the last
        // page, and a shorter one would hide the caller's own stride arithmetic.
        SizeT AllocationSize = 0;

        GbmBackend Backend = GbmBackend::None;

        // Set only for host frames: the slot index and sequence number that the
        // release message has to mention so the phone can recycle the right slot, and
        // the connection the frame arrived on. A frame left over from a connection
        // that has since died is never released into the one that replaced it, because
        // that would free a slot belonging to another session.
        Uint32 HostIndex = 0;
        Uint32 HostSeq = 0;
        Uint64 HostGeneration = 0;
        Bool HostOwned = false;

        // How many maps are outstanding. The mapping itself is kept here so that
        // unmap can find and tear down the right one.
        Int MapRefCount = 0;
        void* MapAddress = nullptr;
        SizeT MapSize = 0;

        // Surfaces keep their buffers alive by count rather than by ownership, so
        // the device knows when a bo is truly unreferenced.
        Int RefCount = 1;

        // gbm_bo_set_user_data's pair. The destroy callback is invoked with the
        // gbm_bo that is going away, once, from the destructor below.
        void* UserData = nullptr;
        void (*UserDataDestroy)(::gbm_bo*, void*) = nullptr;

        ::gbm_device* Owner = nullptr;

        // The record releases its own descriptors and its own mapping, so that no
        // failure path anywhere in this library can leak either one - an allocation
        // that fails half way through construction still cleans up when the record
        // it was building is destroyed.
        ~GbmBufferObject();
    };

    // A real swapchain. The compositor asks for a buffer to render into, presents
    // it, and later returns it; the three states below are exactly the states the
    // GBM API can express.
    struct GbmSurface
    {
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Format = 0;
        Uint32 Flags = 0;

        std::vector<::gbm_bo*> Buffers;
        // Index of the buffer currently handed to the compositor as the front
        // buffer, or -1 when nothing has been locked.
        Int LockedIndex = -1;
        // Indices of buffers the compositor has released and that may be handed
        // out again, plus the one that is locked.
        std::vector<Bool> Free;

        ::gbm_device* Owner = nullptr;

        // Dropping the surface's references here is what keeps the creation path
        // free of a second failure path: if the third swapchain buffer cannot be
        // allocated, unwinding the half-built surface returns the two that exist.
        ~GbmSurface();
    };

    // The device ties one backend choice, one host channel and one mutex together.
    // The single mutex is deliberate: GBM calls arrive from the compositor's
    // render thread and from its main thread, and the state they touch (the bo
    // table, the backends) is small enough that one lock is cheaper than several.
    struct GbmDevice
    {
        // The fd this device was created with. It is a private dup taken at
        // creation, never the caller's descriptor, so gbm_device_destroy can close
        // it without ever closing an fd the caller still has open. -1 when the
        // caller passed none and no render node could be opened: the self-allocated
        // backend does not need a DRM node, so that is a working device and not an
        // error.
        Int DeviceFd = -1;
        // The dma-heap node this device allocates from, or -1 when none could be
        // opened, in which case every allocation falls back to a memfd.
        Int HeapFd = -1;
        String HeapPath;

        // Where the display host is reached, as configured when the device was created.
        String SocketPath;
        GbmBackend DefaultBackend = GbmBackend::DmaHeap;
        // True while the channel below is connected and frames are arriving over it.
        // False means every allocation this device answers is answered locally.
        Bool HostAvailable = false;
        // The name gbm_device_get_backend_name reports. It names what this device
        // actually does, because a compositor that logs it is asking exactly that - so
        // it changes when the host channel connects and when that connection dies, and
        // SelfAllocatedBackendName is what it falls back to when it does.
        String BackendName = "mobilegl";
        String SelfAllocatedBackendName = "mobilegl";

        std::mutex Mutex;
        std::unordered_map<::gbm_bo*, GbmBufferObject*> Buffers;
        std::unordered_map<::gbm_surface*, GbmSurface*> Surfaces;

        // The host channel is a separate translation unit so that the socket protocol
        // never leaks into the allocation paths. It lives for as long as the device
        // does and is reached only under this device's mutex, which is what lets the
        // allocation path ask it for a frame without a second lock to order against.
        class GbmHostChannel* Host = nullptr;

        // Closes the heap node and the private device dup, so that a device which
        // fails to finish constructing still releases the descriptors it opened.
        ~GbmDevice();
    };

    // ---- format description ------------------------------------------------

    // Fills planes[]/planeCount/totalBytes with the linear layout this library
    // would allocate for format at width x height, and returns false for a format
    // it cannot lay out. gbm_bo_create refuses a format this refuses, because a
    // buffer whose plane layout was guessed is worse than a buffer that was not
    // handed out at all.
    Bool GbmDescribeFormat(Uint32 format, Uint32 width, Uint32 height, GbmPlaneLayout planes[GbmMaxPlanes],
                           Int& planeCount, SizeT& totalBytes) noexcept;

    // Whether this library can allocate the format at all.
    Bool GbmFormatIsSupported(Uint32 format) noexcept;

    // Bytes per pixel of plane 0: what gbm_bo_map needs to place a sub-rectangle,
    // and half of what gbm_bo_get_bpp reports.
    Uint32 GbmFormatBytesPerPixel(Uint32 format) noexcept;

    // ---- records and the self-allocated backend ----------------------------

    // Builds a device. The only helper here that is called without the mutex,
    // because no other thread can see the device it is building.
    ::gbm_device* GbmDeviceCreate(Int fd) noexcept;

    // Tears the device down: every remaining surface, every remaining buffer, the
    // heap node, the private device fd, and the record itself.
    void GbmDeviceDestroy(::gbm_device& device) noexcept;

    // Allocates a buffer from the device's heap node, or from a memfd when there
    // is none, and records it in the device's table. Returns null with errno set.
    ::gbm_bo* GbmBufferCreate(::gbm_device& device, Uint32 width, Uint32 height, Uint32 format, Uint64 usage,
                              const Uint64* modifiers, Uint32 modifierCount) noexcept;

    // Wraps descriptors the caller already owns. The descriptors are duped, so the
    // caller keeps ownership of its own; type is GBM_BO_IMPORT_FD or
    // GBM_BO_IMPORT_FD_MODIFIER.
    ::gbm_bo* GbmBufferImport(::gbm_device& device, Uint32 type, const void* buffer, Uint64 usage) noexcept;

    // Drops one reference, destroying the buffer when it was the last one.
    void GbmBufferRelease(::gbm_device& device, ::gbm_bo& bo) noexcept;

    // Destroys a buffer outright, whatever its reference count says. Only the
    // device's own teardown uses this: by then no caller's reference is meaningful.
    void GbmBufferForceDestroy(::gbm_device& device, ::gbm_bo& bo) noexcept;

    // Hands out a dup of the descriptor backing a plane, which the caller then owns.
    // A plane this buffer does not have is refused with errno = EINVAL.
    Int GbmBufferDupFd(::gbm_bo& bo, Int plane) noexcept;

    // Maps the whole buffer once and counts the call; strideOut receives the
    // stride of plane 0. The rect (x, y) is applied to the returned address.
    void* GbmBufferMap(::gbm_bo& bo, Uint32 x, Uint32 y, Uint32& strideOut) noexcept;

    // Drops one mapping; the last one unmaps.
    void GbmBufferUnmap(::gbm_bo& bo, void* mapData) noexcept;

    // ---- host frames -------------------------------------------------------

    // Builds the channel to the display host and makes one attempt to connect it, then
    // stores it in the device. A machine that is not the container half of a split run
    // has nothing at that path, and that is a device which allocates locally from then
    // on - not a device that failed to be created.
    void GbmHostChannelCreate(::gbm_device& device) noexcept;

    // Closes the channel and releases what it holds. Called from the device's
    // destructor, after the buffers that still name offered frames are gone, so that
    // their releases can still be sent.
    void GbmHostChannelDestroy(::gbm_device& device) noexcept;

    // Takes the next frame the host offered, or returns false when none is waiting or
    // the channel is down. Never blocks: the socket is non-blocking, so a caller that
    // finds nothing allocates locally in the same call, which is what makes this usable
    // from a compositor's frame loop.
    Bool GbmHostChannelTakeFrame(::gbm_device& device, GbmHostFrame& frameOut) noexcept;

    // Tells the host that a frame may be recycled. Called from the buffer record's
    // destructor, so a frame goes back exactly when nothing in this process can still
    // read it, and only into the connection that offered it.
    void GbmHostChannelReleaseFrame(::gbm_device& device, Uint32 seq, Uint64 generation) noexcept;

    // ---- swapchains --------------------------------------------------------

    // Builds a surface and its swapchain up front, so that
    // gbm_surface_has_free_buffers can answer truthfully from the first call.
    ::gbm_surface* GbmSurfaceCreate(::gbm_device& device, Uint32 width, Uint32 height, Uint32 format, Uint64 usage,
                                    const Uint64* modifiers, Uint32 modifierCount) noexcept;

    // Unregisters the surface and destroys it, returning the swapchain buffers it
    // still owns to the device.
    void GbmSurfaceDestroy(::gbm_surface& surface) noexcept;

    // Hands out the next free swapchain buffer with a reference of its own, or
    // null with errno = EBUSY when the compositor still holds every one of them.
    ::gbm_bo* GbmSurfaceLockFront(::gbm_surface& surface) noexcept;

    // Takes a locked buffer back, dropping the reference the lock handed out.
    void GbmSurfaceReleaseBuffer(::gbm_surface& surface, ::gbm_bo& bo) noexcept;

    Bool GbmSurfaceHasFreeBuffers(::gbm_surface& surface) noexcept;
} // namespace MobileGL::MG_Gbm

// The public opaque types are the internal records. Callers only ever see the
// pointers, so this costs nothing and removes a layer of casting.
struct gbm_device
{
    MobileGL::MG_Gbm::GbmDevice Impl;
};

struct gbm_bo
{
    MobileGL::MG_Gbm::GbmBufferObject Impl;
};

struct gbm_surface
{
    MobileGL::MG_Gbm::GbmSurface Impl;
};

// End of Source File Header
