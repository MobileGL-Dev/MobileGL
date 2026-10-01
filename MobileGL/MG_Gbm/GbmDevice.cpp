// MobileGL - MobileGL/MG_Gbm/GbmDevice.cpp
// SPDX-License-Identifier: LGPL-3.0-only
//
// The records, and the backend that allocates without a display server behind it.
//
// There are two ways a buffer can come into existence here, and which one answers is
// decided per allocation: a frame the display host offered is taken (GbmHostChannel.cpp
// owns that conversation), and everything else is allocated locally by this file. A
// device therefore does not have "a" backend so much as a backend per buffer, which is
// why the buffer record names the one it was built on.
//
// Allocation happens in the kernel's dma-heap when a heap node is readable by this
// user, and in a memfd when it is not. The order is not a preference: a dma-heap
// buffer is a real dma-buf, so the Anland display producer can import the fd and
// hand it to the phone, while a memfd is ordinary anonymous memory that no DRM
// device can import. The fallback exists so that a compositor still starts and
// renders on a machine where /dev/dma_heap is root-only or absent; it is announced
// in the log and in gbm_device_get_backend_name, because a frame that reaches the
// phone through the wrong path must never look like the right one.
//
// Every buffer this file produces is linear: one allocation, one stride per plane,
// no tiling and no compression, which is what gbm_bo_get_modifier answers with
// DRM_FORMAT_MOD_LINEAR. That is a statement about the memory that actually exists,
// not a default that was picked because nothing else was available - the dma-heap
// hands out contiguous system memory and this library never asks a GPU driver to
// lay anything out.

#include "GbmInternal.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>

// memfd_create is declared by glibc under _GNU_SOURCE (which g++ defines for C++),
// but MFD_CLOEXEC lives in <linux/memfd.h>, which is not pulled in by <sys/mman.h>
// and does not exist in every sysroot this file may be compiled against.
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace MobileGL::MG_Gbm
{
    namespace
    {
        // Three buffers per swapchain: one on screen, one being rendered into, and
        // one being handed back. That is the depth every compositor in this tree
        // already assumes, and a fixed depth is what lets
        // gbm_surface_has_free_buffers tell the truth instead of describing an
        // unbounded pool that does not exist.
        constexpr Uint32 GbmSurfaceBufferCount = 3;

        // The page size is not a compile-time constant on every target this library
        // is built for (aarch64 kernels run 4K, 16K and 64K pages), and dma-heap
        // rejects a length that is not a whole number of pages, so it is asked for.
        SizeT GbmPageSize() noexcept
        {
            const long size = sysconf(_SC_PAGESIZE);
            return size > 0 ? (SizeT)size : (SizeT)4096;
        }

        SizeT GbmAlignUp(SizeT value, SizeT alignment) noexcept
        {
            if (alignment == 0)
            {
                return value;
            }
            return ((value + alignment - 1) / alignment) * alignment;
        }

        // close() is allowed to overwrite errno, and every caller here is in the
        // middle of reporting a failure with strerror(errno). Saving errno around
        // the close is what keeps the reported failure the one that happened.
        void GbmCloseQuietly(Int fd) noexcept
        {
            if (fd < 0)
            {
                return;
            }
            const Int savedErrno = errno;
            close(fd);
            errno = savedErrno;
        }

        // The descriptor a caller receives must be its own - that is the whole
        // difference between gbm_bo_get_fd and gbm_bo_get_handle - and a descriptor
        // that survives exec into a child that has no idea what it refers to is
        // worse than one that does not, so every dup here is close-on-exec.
        Int GbmDupCloexec(Int fd) noexcept
        {
            if (fd < 0)
            {
                errno = EBADF;
                return -1;
            }
            return fcntl(fd, F_DUPFD_CLOEXEC, 0);
        }

        // The kernel's dma-heap allocator ABI: four fields and one ioctl number.
        // They are spelled out here rather than included from <linux/dma-heap.h>
        // because that header is absent from bionic and from plenty of desktop
        // kernel-header packages, while the ABI itself has not changed since the
        // allocator was merged.
        struct DmaHeapAllocationData
        {
            Uint64 Len;
            Uint32 Fd;
            Uint32 FdFlags;
            Uint64 HeapFlags;
        };

        constexpr Uint32 DmaHeapIoctlAlloc = (Uint32)_IOWR('H', 0x0, DmaHeapAllocationData);

        // One allocation, from the heap node when there is one and from a memfd when
        // there is not. Returns the descriptor with the access mode the callers need
        // (O_RDWR, so that mmap with PROT_WRITE works on either backend) and reports
        // which backend answered in backendOut.
        Int GbmAllocateBackingFd(GbmDevice& device, SizeT size, GbmBackend& backendOut) noexcept
        {
            const SizeT alignedSize = GbmAlignUp(size, GbmPageSize());

            if (device.HeapFd >= 0)
            {
                DmaHeapAllocationData request = {};
                request.Len = (Uint64)alignedSize;
                request.Fd = 0; // the kernel writes the new descriptor here
                request.FdFlags = (Uint32)(O_RDWR | O_CLOEXEC);
                request.HeapFlags = 0;
                if (ioctl(device.HeapFd, (unsigned long)DmaHeapIoctlAlloc, &request) == 0)
                {
                    backendOut = GbmBackend::DmaHeap;
                    return (Int)request.Fd;
                }
                const Int heapErrno = errno;
                MGLOG_W_ONCE("gbm: %s refused a %zu byte allocation (%d: %s); this buffer falls back to a "
                             "memfd, which is not a dma-buf any DRM device or display host can import",
                             device.HeapPath.c_str(), alignedSize, heapErrno, strerror(heapErrno));
            }

#if defined(__ANDROID__) && __ANDROID_API__ < 30
            // BIONIC ONLY DECLARES memfd_create FROM API 30, and this library builds against 26.
            // The kernel call is far older than the declaration - it has been there since 3.17 - so
            // asking for it by number is not a workaround for a missing feature, it is the same call
            // with the name the headers of this API level do not carry yet.
            const Int fd = static_cast<Int>(::syscall(__NR_memfd_create, "mobilegl-gbm", MFD_CLOEXEC));
#else
            const Int fd = memfd_create("mobilegl-gbm", MFD_CLOEXEC);
#endif
            if (fd < 0)
            {
                MGLOG_E_ONCE("gbm: memfd_create failed (%d: %s) and no dma-heap node is usable, so no "
                             "buffer can be allocated at all",
                             errno, strerror(errno));
                return -1;
            }
            if (ftruncate(fd, (off_t)alignedSize) != 0)
            {
                const Int truncateErrno = errno;
                GbmCloseQuietly(fd);
                errno = truncateErrno;
                MGLOG_E_ONCE("gbm: ftruncate(%zu) on the fallback memfd failed (%d: %s)", alignedSize, errno,
                             strerror(errno));
                return -1;
            }
            backendOut = GbmBackend::MemFd;
            return fd;
        }

        // A modifier list that does not contain DRM_FORMAT_MOD_LINEAR asks for
        // something this library cannot produce: it only ever allocates linear
        // memory. Refusing outright would be worse than answering, because the
        // caller re-reads gbm_bo_get_modifier and can still make its own decision
        // about a linear buffer - so the request is served with the layout that
        // exists and the disagreement is logged once, at the call site that caused it.
        void GbmWarnIfLinearNotOffered(const Uint64* modifiers, Uint32 modifierCount, const char* what) noexcept
        {
            if (modifiers == nullptr || modifierCount == 0)
            {
                return;
            }
            for (Uint32 i = 0; i < modifierCount; ++i)
            {
                if (modifiers[i] == DrmFormatModLinear)
                {
                    return;
                }
            }
            MGLOG_W_ONCE("gbm: %s was offered %u modifier(s) that do not include DRM_FORMAT_MOD_LINEAR; "
                         "this library only allocates linear dma-bufs and reports "
                         "DRM_FORMAT_MOD_LINEAR, so the caller must accept the buffer it gets or refuse it",
                         what, modifierCount);
        }

        // How many bytes an imported buffer's descriptors actually back. The caller
        // described the memory with strides and offsets and the format says how many
        // rows each plane has; where the format is one this library cannot lay out,
        // the whole height is assumed for every plane. Over-stating is the safe
        // direction: a mapping shorter than the caller's own stride arithmetic would
        // fault on the last row.
        SizeT GbmComputeImportSize(Uint32 format, Uint32 width, Uint32 height, const Uint32 strides[],
                                   const Uint32 offsets[], Int planeCount) noexcept
        {
            GbmPlaneLayout layout[GbmMaxPlanes] = {};
            Int layoutPlanes = 0;
            SizeT laidOutBytes = 0;
            const Bool layoutKnown = GbmDescribeFormat(format, width, height, layout, layoutPlanes, laidOutBytes);

            SizeT end = 0;
            for (Int i = 0; i < planeCount; ++i)
            {
                Uint32 rows = height;
                if (layoutKnown && i < layoutPlanes && layout[i].Stride != 0)
                {
                    rows = (Uint32)(layout[i].Size / layout[i].Stride);
                }
                const SizeT planeEnd = (SizeT)offsets[i] + (SizeT)strides[i] * (SizeT)rows;
                if (planeEnd > end)
                {
                    end = planeEnd;
                }
            }
            return end;
        }

        // Builds the record for a frame the display host offered. The geometry is the
        // offer's rather than the caller's, which is the whole point of the backend: the
        // buffer is one the phone's display path already knows how to show, and its
        // width, height, stride and format are facts about that memory instead of
        // requests this library could have answered differently.
        ::gbm_bo* GbmHostBufferCreate(::gbm_device& device, GbmHostFrame& frame) noexcept
        {
            ::gbm_bo* bo = nullptr;
            try
            {
                bo = new gbm_bo();
                GbmBufferObject& impl = bo->Impl;
                impl.Width = frame.Width;
                impl.Height = frame.Height;
                impl.Format = frame.Format;
                impl.Stride = frame.StrideBytes;
                impl.BytesPerPixel = frame.BytesPerPixel;
                // The host's own statement about what the frame was allocated for is the
                // only true one here: the caller's usage flags describe a buffer the
                // caller asked for, and the caller did not get one.
                impl.Usage = frame.Usage;
                impl.PlaneCount = 1;
                // Measured from the descriptor rather than computed from the geometry,
                // and the only length that is safe to mmap.
                impl.AllocationSize = frame.Size;
                impl.Backend = GbmBackend::HostFrame;
                impl.Owner = &device;
                impl.RefCount = 1;
                impl.Fds[0] = frame.Fd;
                // The record owns the descriptor from here on, so the frame that carried
                // it must not close it when it goes out of scope - and the throwing path
                // below is covered by the record's own destructor instead.
                frame.Fd = -1;
                // A stride is a complete description of a linear image and the offer
                // carries nothing else about the layout, so linear is what this records.
                impl.Modifier = DrmFormatModLinear;
                for (Int i = 0; i < (Int)GbmMaxPlanes; ++i)
                {
                    impl.Strides[i] = i == 0 ? frame.StrideBytes : 0;
                    impl.Offsets[i] = 0;
                    impl.Modifiers[i] = DrmFormatModLinear;
                }
                impl.HostOwned = true;
                impl.HostSeq = frame.Seq;
                impl.HostIndex = frame.Index;
                impl.HostGeneration = frame.Generation;

                device.Impl.Buffers[bo] = &impl;

                MGLOG_I("gbm_bo_create: %ux%u format=0x%08x stride=%u planes=1 size=%zu fd=%d "
                        "backend=host-frame seq=%u index=%u",
                        impl.Width, impl.Height, impl.Format, impl.Stride, impl.AllocationSize, impl.Fds[0],
                        frame.Seq, frame.Index);
                return bo;
            }
            catch (...)
            {
                delete bo;
                errno = ENOMEM;
                return nullptr;
            }
        }
    } // namespace

    // ------------------------------------------------------------------------
    // Formats
    // ------------------------------------------------------------------------

    Bool GbmDescribeFormat(Uint32 format, Uint32 width, Uint32 height, GbmPlaneLayout planes[GbmMaxPlanes],
                           Int& planeCount, SizeT& totalBytes) noexcept
    {
        if (width == 0 || height == 0)
        {
            return false;
        }

        struct PlaneShape
        {
            Uint32 BytesPerPixel;
            Uint32 Width;
            Uint32 Height;
        };

        PlaneShape shape[GbmMaxPlanes] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        Int count = 1;

        switch (format)
        {
        // ---- one plane, an integer number of bytes per pixel ----
        case GBM_FORMAT_C8:
        case GBM_FORMAT_R8:
        case GBM_FORMAT_RGB332:
        case GBM_FORMAT_BGR233:
            shape[0] = {1, width, height};
            break;

        case GBM_FORMAT_R16:
        case GBM_FORMAT_GR88:
        case GBM_FORMAT_XRGB4444:
        case GBM_FORMAT_XBGR4444:
        case GBM_FORMAT_RGBX4444:
        case GBM_FORMAT_BGRX4444:
        case GBM_FORMAT_ARGB4444:
        case GBM_FORMAT_ABGR4444:
        case GBM_FORMAT_RGBA4444:
        case GBM_FORMAT_BGRA4444:
        case GBM_FORMAT_XRGB1555:
        case GBM_FORMAT_XBGR1555:
        case GBM_FORMAT_RGBX5551:
        case GBM_FORMAT_BGRX5551:
        case GBM_FORMAT_ARGB1555:
        case GBM_FORMAT_ABGR1555:
        case GBM_FORMAT_RGBA5551:
        case GBM_FORMAT_BGRA5551:
        case GBM_FORMAT_RGB565:
        case GBM_FORMAT_BGR565:
            shape[0] = {2, width, height};
            break;

        case GBM_FORMAT_RGB888:
        case GBM_FORMAT_BGR888:
            shape[0] = {3, width, height};
            break;

        case GBM_FORMAT_RG1616:
        case GBM_FORMAT_GR1616:
        case GBM_FORMAT_XRGB8888:
        case GBM_FORMAT_XBGR8888:
        case GBM_FORMAT_RGBX8888:
        case GBM_FORMAT_BGRX8888:
        case GBM_FORMAT_ARGB8888:
        case GBM_FORMAT_ABGR8888:
        case GBM_FORMAT_RGBA8888:
        case GBM_FORMAT_BGRA8888:
        case GBM_FORMAT_XRGB2101010:
        case GBM_FORMAT_XBGR2101010:
        case GBM_FORMAT_RGBX1010102:
        case GBM_FORMAT_BGRX1010102:
        case GBM_FORMAT_ARGB2101010:
        case GBM_FORMAT_ABGR2101010:
        case GBM_FORMAT_RGBA1010102:
        case GBM_FORMAT_BGRA1010102:
        case GBM_FORMAT_AYUV:
            shape[0] = {4, width, height};
            break;

        case GBM_FORMAT_XBGR16161616:
        case GBM_FORMAT_ABGR16161616:
        case GBM_FORMAT_XBGR16161616F:
        case GBM_FORMAT_ABGR16161616F:
            shape[0] = {8, width, height};
            break;

        // Packed 4:2:2: one 32 bit word holds two pixels, so a row is two bytes per
        // pixel even though no single pixel is two bytes wide.
        case GBM_FORMAT_YUYV:
        case GBM_FORMAT_YVYU:
        case GBM_FORMAT_UYVY:
        case GBM_FORMAT_VYUY:
            shape[0] = {2, width, height};
            break;

        // ---- two planes ----
        // NV12/NV21 interleave Cb and Cr, so one chroma row is one row of pairs and
        // is as wide as the luma row in bytes; only its row count is halved. NV16
        // and NV61 subsample horizontally only, so their chroma rows match luma.
        case GBM_FORMAT_NV12:
        case GBM_FORMAT_NV21:
            shape[0] = {1, width, height};
            shape[1] = {1, width, (height + 1) / 2};
            count = 2;
            break;

        case GBM_FORMAT_NV16:
        case GBM_FORMAT_NV61:
            shape[0] = {1, width, height};
            shape[1] = {1, width, height};
            count = 2;
            break;

        // ---- three planes ----
        // The chroma planes are rounded up rather than truncated: for an odd width or
        // height the last chroma sample still has to fit, and a plane one row short
        // is a plane the GPU writes past the end of.
        case GBM_FORMAT_YUV420:
        case GBM_FORMAT_YVU420:
            shape[0] = {1, width, height};
            shape[1] = {1, (width + 1) / 2, (height + 1) / 2};
            shape[2] = shape[1];
            count = 3;
            break;

        case GBM_FORMAT_YUV410:
        case GBM_FORMAT_YVU410:
            shape[0] = {1, width, height};
            shape[1] = {1, (width + 3) / 4, (height + 3) / 4};
            shape[2] = shape[1];
            count = 3;
            break;

        case GBM_FORMAT_YUV411:
        case GBM_FORMAT_YVU411:
            shape[0] = {1, width, height};
            shape[1] = {1, (width + 3) / 4, height};
            shape[2] = shape[1];
            count = 3;
            break;

        case GBM_FORMAT_YUV422:
        case GBM_FORMAT_YVU422:
            shape[0] = {1, width, height};
            shape[1] = {1, (width + 1) / 2, height};
            shape[2] = shape[1];
            count = 3;
            break;

        case GBM_FORMAT_YUV444:
        case GBM_FORMAT_YVU444:
            shape[0] = {1, width, height};
            shape[1] = {1, width, height};
            shape[2] = shape[1];
            count = 3;
            break;

        // A format this library cannot lay out is refused rather than guessed at:
        // every plane offset that follows from a wrong table is memory some other
        // component will write through.
        default:
            return false;
        }

        // Planes are packed tightly, each one starting where the previous ended. That
        // is the layout a single-stride-per-plane reader expects, and every plane of
        // the buffer stays addressable from its offset inside the one allocation.
        SizeT offset = 0;
        for (Int i = 0; i < count; ++i)
        {
            const SizeT stride = (SizeT)shape[i].Width * shape[i].BytesPerPixel;
            const SizeT size = stride * shape[i].Height;
            // A plane offset is a 32 bit field in every DRM descriptor that follows,
            // so a buffer whose planes would not fit in one is not one this library
            // can describe.
            if (stride > 0xffffffffu || offset + size > 0xffffffffu)
            {
                return false;
            }
            planes[i].Stride = (Uint32)stride;
            planes[i].Offset = (Uint32)offset;
            planes[i].Size = size;
            offset += size;
        }
        for (Int i = count; i < (Int)GbmMaxPlanes; ++i)
        {
            planes[i] = GbmPlaneLayout{};
        }

        planeCount = count;
        totalBytes = offset;
        return true;
    }

    Uint32 GbmFormatBytesPerPixel(Uint32 format) noexcept
    {
        GbmPlaneLayout planes[GbmMaxPlanes] = {};
        Int planeCount = 0;
        SizeT totalBytes = 0;
        // A 1x1 image of the format has exactly one pixel per row, so its plane 0
        // stride is the format's bytes per pixel. Asking the layout function keeps
        // one table in this file instead of two that can drift apart.
        if (!GbmDescribeFormat(format, 1, 1, planes, planeCount, totalBytes))
        {
            return 0;
        }
        return planes[0].Stride;
    }

    Bool GbmFormatIsSupported(Uint32 format) noexcept
    {
        return GbmFormatBytesPerPixel(format) != 0;
    }

    // ------------------------------------------------------------------------
    // Record lifetime
    // ------------------------------------------------------------------------

    GbmBufferObject::~GbmBufferObject()
    {
        // A buffer that is still mapped when it is destroyed is unmapped here rather
        // than leaked: gbm_bo_destroy promises to release the buffer, and a
        // compositor that forgot an unmap must not keep the pages of a dead dma-buf
        // alive. This is also why the deliberate "leave the write mapping open"
        // habit some callers have is safe with this library.
        if (MapAddress != nullptr)
        {
            munmap(MapAddress, MapSize);
            MapAddress = nullptr;
            MapSize = 0;
            MapRefCount = 0;
        }
        for (Uint32 i = 0; i < GbmMaxPlanes; ++i)
        {
            if (Fds[i] >= 0)
            {
                close(Fds[i]);
                Fds[i] = -1;
            }
        }
        if (HostOwned && Owner != nullptr)
        {
            // The frame goes back to the phone only now, with this process's descriptor
            // already closed: a release that left first would let the host recycle a
            // slot that this process could still read. This is also the one place a
            // frame can be released from, which is what keeps the release to exactly one
            // per frame - gbm_bo_destroy of a buffer a surface still presents only drops
            // the caller's reference and must not free the frame underneath it.
            GbmHostChannelReleaseFrame(*Owner, HostSeq, HostGeneration);
            HostOwned = false;
        }
        if (UserDataDestroy != nullptr)
        {
            // The public name of this record is the gbm_bo that contains it, which is
            // the pointer the caller registered the data against.
            UserDataDestroy(reinterpret_cast<::gbm_bo*>(this), UserData);
            UserDataDestroy = nullptr;
            UserData = nullptr;
        }
    }

    GbmSurface::~GbmSurface()
    {
        if (Owner == nullptr)
        {
            return;
        }
        // Dropping the surface's own references here is what removes a second
        // failure path from surface creation: a swapchain that could only allocate
        // two of its three buffers returns those two to the device by unwinding,
        // without a cleanup routine of its own. A buffer the compositor still holds
        // locked survives, because the reference the lock handed out is separate.
        for (::gbm_bo* buffer : Buffers)
        {
            if (buffer != nullptr)
            {
                GbmBufferRelease(*Owner, *buffer);
            }
        }
        Buffers.clear();
        Free.clear();
        LockedIndex = -1;
        Owner = nullptr;
    }

    GbmDevice::~GbmDevice()
    {
        if (HeapFd >= 0)
        {
            close(HeapFd);
            HeapFd = -1;
        }
        if (DeviceFd >= 0)
        {
            close(DeviceFd);
            DeviceFd = -1;
        }
    }

    // ------------------------------------------------------------------------
    // Device
    // ------------------------------------------------------------------------

    ::gbm_device* GbmDeviceCreate(Int fd) noexcept
    {
        ::gbm_device* device = nullptr;
        try
        {
            device = new gbm_device();
            GbmDevice& impl = device->Impl;

            // The caller's descriptor is duped rather than adopted. gbm_create_device
            // is called with an fd the caller usually keeps using - kwin holds its
            // DRM node open for mode setting for the life of the process - and a
            // device that closed it in gbm_device_destroy would be closing somebody
            // else's descriptor. The dup costs one number and removes the question.
            if (fd >= 0)
            {
                impl.DeviceFd = GbmDupCloexec(fd);
                if (impl.DeviceFd < 0)
                {
                    MGLOG_W("gbm_create_device: could not dup fd %d (%d: %s); the device is still usable "
                            "because the self-allocated backend needs no DRM node",
                            fd, errno, strerror(errno));
                }
            }
            else
            {
                // gbm_create_device(-1) asks for a device with no node behind it. A
                // render node is opened when there is one, so that gbm_device_get_fd
                // answers with something a caller can use, but failing to open it is
                // not fatal: nothing in the self-allocated backend talks to DRM.
                impl.DeviceFd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
                if (impl.DeviceFd < 0)
                {
                    MGLOG_I("gbm_create_device(-1): no render node to open (%d: %s); allocating from the "
                            "dma-heap without one", errno, strerror(errno));
                }
            }

            // The heap nodes, in the order the container is expected to provide
            // them. The uncached node is the fallback for a machine that exposes only
            // it; a cached node is what a compositor wants, because it renders into
            // these buffers with the GPU.
            static const char* const heapNodes[] = {"/dev/dma_heap/system", "/dev/dma_heap/system-uncached"};
            for (const char* node : heapNodes)
            {
                const Int heapFd = open(node, O_RDWR | O_CLOEXEC);
                if (heapFd >= 0)
                {
                    impl.HeapFd = heapFd;
                    impl.HeapPath = node;
                    break;
                }
                MGLOG_I("gbm_create_device: %s is not usable (%d: %s)", node, errno, strerror(errno));
            }

            impl.DefaultBackend = impl.HeapFd >= 0 ? GbmBackend::DmaHeap : GbmBackend::MemFd;
            // The backend name is not decoration: a compositor that logs it, and the
            // T4 probe that records it, are asking which kind of memory the frames
            // they are about to show are made of.
            impl.BackendName = impl.HeapFd >= 0 ? "mobilegl-dma-heap" : "mobilegl-memfd";

            if (impl.HeapFd >= 0)
            {
                MGLOG_I("gbm_create_device: %s backend=%s devFd=%d heapFd=%d (buffers are dma-bufs, "
                        "modifier=DRM_FORMAT_MOD_LINEAR)",
                        impl.HeapPath.c_str(), impl.BackendName.c_str(), impl.DeviceFd, impl.HeapFd);
            }
            else
            {
                MGLOG_W("gbm_create_device: no dma-heap node could be opened, so every buffer comes from a "
                        "memfd; backend=%s devFd=%d. Rendering still works, but the fds cannot be imported "
                        "by a DRM device or shipped to the phone as dma-bufs until %s is readable",
                        impl.BackendName.c_str(), impl.DeviceFd, heapNodes[0]);
            }

            // The host-frame backend, and the one connection attempt it makes at
            // creation time. A machine that is not the container half of a split run has
            // nothing at that path, and a device with no display host is a device that
            // allocates locally rather than one that failed - the channel keeps retrying
            // on later allocations.
            impl.SelfAllocatedBackendName = impl.BackendName;
            GbmHostChannelCreate(*device);

            return device;
        }
        catch (...)
        {
            // A GBM device that throws cannot be caught by any of its C callers, so
            // the failure is reported the way GBM reports every other one. The channel
            // is a no-op to destroy when it was never built, and the one thing here that
            // owns a descriptor which the device's own destructor does not close.
            if (device != nullptr)
            {
                GbmHostChannelDestroy(*device);
            }
            delete device;
            errno = ENOMEM;
            return nullptr;
        }
    }

    void GbmDeviceDestroy(::gbm_device& device) noexcept
    {
        GbmDevice& impl = device.Impl;

        // Surfaces first. A surface holds a reference to every buffer in its
        // swapchain, so destroying a buffer before its surface would leave the
        // surface's table pointing at memory that is already gone.
        while (!impl.Surfaces.empty())
        {
            GbmSurfaceDestroy(*impl.Surfaces.begin()->first);
        }

        if (!impl.Buffers.empty())
        {
            // A process that exits without destroying its buffers is the normal case
            // (the compositor relies on process exit for the last few), so this is a
            // statement about what is being released here, not a call to action the
            // caller can still answer.
            MGLOG_W("gbm_device_destroy: %zu buffer(s) were still allocated; releasing them here rather "
                    "than letting the dma-bufs outlive the device that owns them",
                    impl.Buffers.size());
        }
        while (!impl.Buffers.empty())
        {
            GbmBufferForceDestroy(device, *impl.Buffers.begin()->first);
        }

        // The channel outlives the buffers so that the frames they were made of can
        // still be released to the phone - a release sent after the socket closed would
        // be a slot the phone never gets back - and dies here rather than in the
        // destructor, which is below the public struct and could only reach it through
        // a cast back to it.
        GbmHostChannelDestroy(device);

        // The destructor closes the heap node and the private device fd.
        delete &device;
    }

    // ------------------------------------------------------------------------
    // Buffers
    // ------------------------------------------------------------------------

    ::gbm_bo* GbmBufferCreate(::gbm_device& device, Uint32 width, Uint32 height, Uint32 format, Uint64 usage,
                              const Uint64* modifiers, Uint32 modifierCount) noexcept
    {
        if (width == 0 || height == 0)
        {
            errno = EINVAL;
            return nullptr;
        }
        if ((usage & GBM_BO_USE_PROTECTED) != 0)
        {
            // A protected buffer is encrypted memory that only a trusted
            // display/GPU path may touch. A dma-heap allocation is plain system
            // memory that any process holding the fd can map, so answering this
            // request with one would hand out the opposite of what was asked for.
            MGLOG_E_ONCE("gbm_bo_create: GBM_BO_USE_PROTECTED is not something a dma-heap allocation can "
                         "provide; refusing rather than handing out unprotected memory");
            errno = EINVAL;
            return nullptr;
        }

        // A frame the display host offered is taken before the caller's request is
        // examined any further, because the host decides the geometry: what the
        // compositor gets is the buffer the phone's display path already knows, and
        // reporting the size the caller asked for instead would describe memory that is
        // not there. The checks above still apply - they are about what a caller may be
        // handed at all rather than about how big it is.
        //
        // The errno of a failed attempt is put back before the local path runs: a buffer
        // that was allocated locally is not a failure, and the strerror(errno) a caller
        // prints has to describe a failure that actually happened.
        const Int callerErrno = errno;
        GbmHostFrame hostFrame;
        if (GbmHostChannelTakeFrame(device, hostFrame))
        {
            return GbmHostBufferCreate(device, hostFrame);
        }
        errno = callerErrno;

        // The modifier list is not consulted for a host frame either: its layout is the
        // display's, and the stride the offer carries is a description of linear memory.
        GbmPlaneLayout layout[GbmMaxPlanes] = {};
        Int planeCount = 0;
        SizeT totalBytes = 0;
        if (!GbmDescribeFormat(format, width, height, layout, planeCount, totalBytes))
        {
            MGLOG_E_ONCE("gbm_bo_create: format 0x%08x is not a format this library can lay out; refusing "
                         "rather than allocating memory whose plane offsets would be a guess",
                         format);
            errno = EINVAL;
            return nullptr;
        }
        GbmWarnIfLinearNotOffered(modifiers, modifierCount, "gbm_bo_create_with_modifiers");

        ::gbm_bo* bo = nullptr;
        try
        {
            GbmBackend backend = GbmBackend::None;
            const Int fd = GbmAllocateBackingFd(device.Impl, totalBytes, backend);
            if (fd < 0)
            {
                errno = ENOMEM;
                return nullptr;
            }

            // From here on the record owns the descriptor, so even the throwing
            // paths below cannot leak it: the holder's destructor closes it.
            bo = new gbm_bo();
            GbmBufferObject& impl = bo->Impl;
            impl.Width = width;
            impl.Height = height;
            impl.Format = format;
            impl.Stride = layout[0].Stride;
            impl.BytesPerPixel = layout[0].Stride / width;
            impl.Usage = usage;
            impl.PlaneCount = planeCount;
            impl.AllocationSize = GbmAlignUp(totalBytes, GbmPageSize());
            impl.Backend = backend;
            impl.Owner = &device;
            impl.RefCount = 1;
            impl.Fds[0] = fd;
            impl.Modifier = DrmFormatModLinear;
            for (Int i = 0; i < (Int)GbmMaxPlanes; ++i)
            {
                const Bool used = i < planeCount;
                impl.Strides[i] = used ? layout[i].Stride : 0;
                impl.Offsets[i] = used ? layout[i].Offset : 0;
                impl.Modifiers[i] = DrmFormatModLinear;
            }

            // The device's table is the record of what is alive, so registering
            // before the function returns is what makes gbm_device_destroy able to
            // release a buffer whose owner forgot it.
            device.Impl.Buffers[bo] = &impl;

            MGLOG_I("gbm_bo_create: %ux%u format=0x%08x stride=%u planes=%d size=%zu fd=%d backend=%s "
                    "modifier=DRM_FORMAT_MOD_LINEAR(0)",
                    width, height, format, impl.Stride, planeCount, impl.AllocationSize, fd,
                    backend == GbmBackend::DmaHeap ? "dma-heap" : "memfd");
            return bo;
        }
        catch (...)
        {
            delete bo;
            errno = ENOMEM;
            return nullptr;
        }
    }

    ::gbm_bo* GbmBufferImport(::gbm_device& device, Uint32 type, const void* buffer, Uint64 usage) noexcept
    {
        if (buffer == nullptr)
        {
            errno = EINVAL;
            return nullptr;
        }

        Uint32 width = 0;
        Uint32 height = 0;
        Uint32 format = 0;
        Int planeCount = 1;
        Uint64 modifier = DrmFormatModLinear;
        Uint32 strides[GbmMaxPlanes] = {0, 0, 0, 0};
        Uint32 offsets[GbmMaxPlanes] = {0, 0, 0, 0};
        Int sourceFds[GbmMaxPlanes] = {-1, -1, -1, -1};

        switch (type)
        {
        case GBM_BO_IMPORT_FD:
        {
            const auto* data = static_cast<const gbm_import_fd_data*>(buffer);
            width = data->width;
            height = data->height;
            format = data->format;
            strides[0] = data->stride;
            sourceFds[0] = data->fd;
            planeCount = 1;
            // A plain FD import carries a stride and nothing else, and a single
            // stride is only a complete description of a linear image - so linear is
            // what this records. A caller importing a tiled or compressed buffer has
            // to say so, and the import type that can say it is
            // GBM_BO_IMPORT_FD_MODIFIER.
            modifier = DrmFormatModLinear;
            break;
        }
        case GBM_BO_IMPORT_FD_MODIFIER:
        {
            const auto* data = static_cast<const gbm_import_fd_modifier_data*>(buffer);
            if (data->num_fds == 0 || data->num_fds > GBM_MAX_PLANES)
            {
                MGLOG_E_ONCE("gbm_bo_import: GBM_BO_IMPORT_FD_MODIFIER with %u descriptors; this library "
                             "handles 1..%d", data->num_fds, (int)GBM_MAX_PLANES);
                errno = EINVAL;
                return nullptr;
            }
            width = data->width;
            height = data->height;
            format = data->format;
            planeCount = (Int)data->num_fds;
            // The modifier is the caller's own statement about memory it allocated,
            // so it is carried through unchanged rather than replaced with the one
            // this library's own allocations use.
            modifier = data->modifier;
            for (Int i = 0; i < planeCount; ++i)
            {
                sourceFds[i] = data->fds[i];
                strides[i] = (Uint32)data->strides[i];
                offsets[i] = (Uint32)data->offsets[i];
            }
            break;
        }
        default:
            // A wl_buffer or an EGLImage is a handle this library cannot resolve
            // without the client library that minted it, and guessing would produce
            // a buffer pointing at memory nobody verified.
            MGLOG_E_ONCE("gbm_bo_import: type 0x%08x is not implemented; only GBM_BO_IMPORT_FD and "
                         "GBM_BO_IMPORT_FD_MODIFIER are, because any other type names a handle this "
                         "library has no way to resolve", type);
            errno = ENOSYS;
            return nullptr;
        }

        if (width == 0 || height == 0)
        {
            errno = EINVAL;
            return nullptr;
        }

        // Every descriptor is duped before the record exists, so a failure half way
        // leaves none of the caller's descriptors closed and none of ours open. The
        // import owns its copies: the peer that mints a dma-buf and closes its own
        // fd right after the import is doing exactly what this contract allows.
        Int ownedFds[GbmMaxPlanes] = {-1, -1, -1, -1};
        for (Int i = 0; i < planeCount; ++i)
        {
            ownedFds[i] = GbmDupCloexec(sourceFds[i]);
            if (ownedFds[i] < 0)
            {
                const Int dupErrno = errno;
                for (Int j = 0; j < planeCount; ++j)
                {
                    GbmCloseQuietly(ownedFds[j]);
                }
                MGLOG_E_ONCE("gbm_bo_import: cannot dup plane %d's descriptor (%d: %s)", i, dupErrno,
                             strerror(dupErrno));
                errno = dupErrno;
                return nullptr;
            }
        }

        const SizeT totalBytes = GbmComputeImportSize(format, width, height, strides, offsets, planeCount);

        ::gbm_bo* bo = nullptr;
        try
        {
            bo = new gbm_bo();
            GbmBufferObject& impl = bo->Impl;
            impl.Width = width;
            impl.Height = height;
            impl.Format = format;
            impl.Stride = strides[0];
            impl.BytesPerPixel = GbmFormatBytesPerPixel(format);
            impl.Usage = usage;
            impl.PlaneCount = planeCount;
            impl.AllocationSize = GbmAlignUp(totalBytes, GbmPageSize());
            impl.Backend = GbmBackend::None;
            impl.Owner = &device;
            impl.RefCount = 1;
            impl.Modifier = modifier;
            for (Int i = 0; i < (Int)GbmMaxPlanes; ++i)
            {
                const Bool used = i < planeCount;
                impl.Fds[i] = ownedFds[i];
                impl.Strides[i] = used ? strides[i] : 0;
                impl.Offsets[i] = used ? offsets[i] : 0;
                impl.Modifiers[i] = used ? modifier : 0;
            }

            device.Impl.Buffers[bo] = &impl;

            MGLOG_I("gbm_bo_import: type=0x%08x %ux%u format=0x%08x stride=%u planes=%d modifier=0x%016llx "
                    "size=%zu",
                    type, width, height, format, impl.Stride, planeCount, (unsigned long long)modifier,
                    impl.AllocationSize);
            return bo;
        }
        catch (...)
        {
            for (Int i = 0; i < planeCount; ++i)
            {
                GbmCloseQuietly(ownedFds[i]);
            }
            delete bo;
            errno = ENOMEM;
            return nullptr;
        }
    }

    void GbmBufferRelease(::gbm_device& device, ::gbm_bo& bo) noexcept
    {
        GbmBufferObject& impl = bo.Impl;
        if (impl.RefCount > 1)
        {
            impl.RefCount--;
            return;
        }
        // The last reference is gone, so the record goes with it - and the record's
        // destructor is what closes the descriptors and releases any live mapping.
        device.Impl.Buffers.erase(&bo);
        delete &bo;
    }

    void GbmBufferForceDestroy(::gbm_device& device, ::gbm_bo& bo) noexcept
    {
        device.Impl.Buffers.erase(&bo);
        delete &bo;
    }

    Int GbmBufferDupFd(::gbm_bo& bo, Int plane) noexcept
    {
        GbmBufferObject& impl = bo.Impl;
        if (plane < 0)
        {
            plane = 0;
        }
        if (plane >= impl.PlaneCount)
        {
            errno = EINVAL;
            return -1;
        }
        // A self-allocated buffer keeps every plane in one dma-buf and separates them
        // by offset, so the same descriptor answers for each of its planes; an import
        // that carried one descriptor per plane has its own.
        const Int source = impl.Fds[plane] >= 0 ? impl.Fds[plane] : impl.Fds[0];
        return GbmDupCloexec(source);
    }

    void* GbmBufferMap(::gbm_bo& bo, Uint32 x, Uint32 y, Uint32& strideOut) noexcept
    {
        GbmBufferObject& impl = bo.Impl;
        if (impl.Fds[0] < 0 || impl.AllocationSize == 0)
        {
            MGLOG_E_ONCE("gbm_bo_map: the buffer carries no descriptor to map");
            errno = EBADF;
            return nullptr;
        }
        if (x >= impl.Width || y >= impl.Height)
        {
            // A rectangle outside the buffer would produce an address outside the
            // mapping, and the fault would land in the caller's code rather than in
            // this one.
            errno = EINVAL;
            return nullptr;
        }

        if (impl.MapAddress == nullptr)
        {
            // One mmap for the whole buffer, MAP_SHARED because the point of the
            // exercise is that the CPU and the GPU look at the same memory: a private
            // mapping would give the caller a copy that no scanout ever sees. The
            // protection is read/write regardless of the transfer flags, because the
            // mapping is shared between calls and a later write must not need a
            // second one.
            void* address = mmap(nullptr, impl.AllocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, impl.Fds[0], 0);
            if (address == MAP_FAILED)
            {
                MGLOG_E_ONCE("gbm_bo_map: mmap(%zu, PROT_READ|PROT_WRITE, MAP_SHARED) on fd %d failed "
                             "(%d: %s)",
                             impl.AllocationSize, impl.Fds[0], errno, strerror(errno));
                return nullptr;
            }
            impl.MapAddress = address;
            impl.MapSize = impl.AllocationSize;
            impl.MapRefCount = 0;
        }

        // Counted rather than duplicated: a second map of the same buffer returns the
        // address the first one did, and the mapping only goes away when the last
        // unmap has been seen.
        impl.MapRefCount++;
        strideOut = impl.Strides[0];
        return static_cast<Uint8*>(impl.MapAddress) + (SizeT)y * impl.Strides[0] +
               (SizeT)x * impl.BytesPerPixel;
    }

    void GbmBufferUnmap(::gbm_bo& bo, void* mapData) noexcept
    {
        GbmBufferObject& impl = bo.Impl;
        if (impl.MapAddress == nullptr || impl.MapRefCount <= 0)
        {
            MGLOG_W_ONCE("gbm_bo_unmap: the buffer is not mapped; the call is ignored");
            return;
        }
        // map_data is accepted rather than required to match the base address:
        // callers hand back either the pointer gbm_bo_map returned or the one it
        // wrote into *map_data, and for a sub-rectangle those differ. Both name the
        // same mapping, and the count is what decides when it is released.
        (void)mapData;
        impl.MapRefCount--;
        if (impl.MapRefCount > 0)
        {
            return;
        }
        munmap(impl.MapAddress, impl.MapSize);
        impl.MapAddress = nullptr;
        impl.MapSize = 0;
    }

    // ------------------------------------------------------------------------
    // Surfaces
    // ------------------------------------------------------------------------

    ::gbm_surface* GbmSurfaceCreate(::gbm_device& device, Uint32 width, Uint32 height, Uint32 format, Uint64 usage,
                                    const Uint64* modifiers, Uint32 modifierCount) noexcept
    {
        if (width == 0 || height == 0 || !GbmFormatIsSupported(format))
        {
            // The format is checked here rather than left to the first
            // lock_front_buffer, because a surface that exists but cannot produce a
            // buffer fails inside the compositor's frame loop, where there is no good
            // answer left.
            MGLOG_E("gbm_surface_create: %ux%u format 0x%08x is not something this library can allocate",
                    width, height, format);
            errno = EINVAL;
            return nullptr;
        }
        GbmWarnIfLinearNotOffered(modifiers, modifierCount, "gbm_surface_create");

        ::gbm_surface* surface = nullptr;
        try
        {
            surface = new gbm_surface();
            GbmSurface& impl = surface->Impl;
            impl.Width = width;
            impl.Height = height;
            impl.Format = format;
            impl.Flags = (Uint32)usage;
            impl.Owner = &device;
            impl.LockedIndex = -1;
            impl.Buffers.reserve(GbmSurfaceBufferCount);
            impl.Free.reserve(GbmSurfaceBufferCount);

            for (Uint32 i = 0; i < GbmSurfaceBufferCount; ++i)
            {
                ::gbm_bo* buffer = GbmBufferCreate(device, width, height, format, usage, modifiers, modifierCount);
                if (buffer == nullptr)
                {
                    // ~GbmSurface returns the buffers that did get allocated, so this
                    // path needs no cleanup of its own.
                    delete surface;
                    errno = ENOMEM;
                    return nullptr;
                }
                impl.Buffers.push_back(buffer);
                impl.Free.push_back(true);
            }

            device.Impl.Surfaces[surface] = &impl;
            MGLOG_I("gbm_surface_create: %ux%u format=0x%08x flags=0x%x swapchain=%u buffers (all free)",
                    width, height, format, (unsigned)usage, GbmSurfaceBufferCount);
            return surface;
        }
        catch (...)
        {
            delete surface;
            errno = ENOMEM;
            return nullptr;
        }
    }

    void GbmSurfaceDestroy(::gbm_surface& surface) noexcept
    {
        GbmSurface& impl = surface.Impl;
        if (impl.Owner != nullptr)
        {
            impl.Owner->Impl.Surfaces.erase(&surface);
        }
        // ~GbmSurface returns the swapchain's references. A buffer the compositor
        // still holds stays alive until it releases or destroys it, which is exactly
        // what the reference counts are for.
        delete &surface;
    }

    ::gbm_bo* GbmSurfaceLockFront(::gbm_surface& surface) noexcept
    {
        GbmSurface& impl = surface.Impl;
        for (SizeT i = 0; i < impl.Buffers.size(); ++i)
        {
            if (!impl.Free[i])
            {
                continue;
            }
            ::gbm_bo* buffer = impl.Buffers[i];
            impl.Free[i] = false;
            impl.LockedIndex = (Int)i;
            // The caller gets a reference of its own on top of the surface's. That is
            // what makes gbm_bo_destroy on a locked buffer safe: it drops the caller's
            // reference and leaves the surface's table intact.
            buffer->Impl.RefCount++;
            return buffer;
        }
        MGLOG_W_ONCE("gbm_surface_lock_front_buffer: every swapchain buffer is still locked; the compositor "
                     "has to release one before it can render into another");
        errno = EBUSY;
        return nullptr;
    }

    void GbmSurfaceReleaseBuffer(::gbm_surface& surface, ::gbm_bo& bo) noexcept
    {
        GbmSurface& impl = surface.Impl;
        for (SizeT i = 0; i < impl.Buffers.size(); ++i)
        {
            if (impl.Buffers[i] != &bo)
            {
                continue;
            }
            if (impl.Free[i])
            {
                // Releasing twice would hand the same buffer to two frames at once,
                // so the second release is ignored and said out loud.
                MGLOG_W_ONCE("gbm_surface_release_buffer: the buffer was already released; the second "
                             "release is ignored so that two frames cannot be handed the same buffer");
                return;
            }
            impl.Free[i] = true;
            if (impl.LockedIndex == (Int)i)
            {
                impl.LockedIndex = -1;
            }
            // Drops the reference the lock handed out. The surface's own reference
            // keeps the record - and this table's pointer to it - valid.
            GbmBufferRelease(*impl.Owner, bo);
            return;
        }
        // The pointer is compared, never followed, so a stale or foreign buffer
        // reaches this line safely instead of being released.
        MGLOG_W_ONCE("gbm_surface_release_buffer: the buffer was not handed out by this surface; ignored");
    }

    Bool GbmSurfaceHasFreeBuffers(::gbm_surface& surface) noexcept
    {
        for (const Bool free : surface.Impl.Free)
        {
            if (free)
            {
                return true;
            }
        }
        return false;
    }
} // namespace MobileGL::MG_Gbm

// End of Source File Header
