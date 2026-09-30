// MobileGL - MobileGL/MG_Gbm/GbmApi.cpp
// SPDX-License-Identifier: LGPL-3.0-only
//
// The extern "C" surface: every entry point gbm.h declares, and nothing else.
//
// This file is a translation layer, and it is kept that way on purpose. Each entry
// point does three things - check the pointers it was handed, take the device's one
// mutex for the whole call, and hand the work to GbmDevice.cpp - so the concurrency
// rule ("one mutex per device, taken once at the API boundary, never inside the
// allocation code") is visible in one place instead of being spread over the
// backends. A missing lock is a bug that only shows up in the compositor, so the
// boilerplate is deliberately identical in every function.
//
// Two conventions are observable from outside and are stated here because they are
// the ones a caller can get wrong:
//
//   * gbm_bo_get_fd and gbm_bo_get_fd_for_plane return a DUP the caller owns. The
//     caller closes what it was given; the buffer's own descriptor is closed once,
//     when the buffer is destroyed, and never by a caller's close.
//   * Nothing here throws and nothing here aborts. A failure is reported the way
//     GBM reports failures: null or -1 with errno set, so that the strerror(errno)
//     a caller prints describes the failure that actually happened.

#include "GbmInternal.h"

#include <cerrno>

using namespace MobileGL;
using namespace MobileGL::MG_Gbm;

namespace
{
    // The one mutex that guards a record, reached through the device that owns it.
    // Null means the pointer was not one this library handed out, and every entry
    // point turns that into its own failure answer rather than dereferencing it.
    //
    // A buffer or surface with no owner cannot exist while the library is used
    // correctly - the device destroys everything it still holds before it goes away
    // - so null here is a defence against a stale pointer rather than a state the
    // code produces.
    std::mutex* MutexOf(struct gbm_device* device) noexcept
    {
        return device != nullptr ? &device->Impl.Mutex : nullptr;
    }

    std::mutex* MutexOf(struct gbm_bo* bo) noexcept
    {
        return bo != nullptr ? MutexOf(bo->Impl.Owner) : nullptr;
    }

    std::mutex* MutexOf(struct gbm_surface* surface) noexcept
    {
        return surface != nullptr ? MutexOf(surface->Impl.Owner) : nullptr;
    }
} // namespace

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

extern "C" struct gbm_device* gbm_create_device(int fd)
{
    // No lock to take: the device being built is not reachable by any other thread
    // until this call returns it.
    return GbmDeviceCreate(fd);
}

extern "C" void gbm_device_destroy(struct gbm_device* gbm)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        return;
    }
    // The lock is taken and deliberately never released: the mutex lives inside the
    // record this call deletes, and unlocking a destroyed mutex is worse than
    // leaving a dead one locked. Any other thread touching this device after its
    // owner declared it dead is already outside what GBM can be asked to survive.
    mutex->lock();
    GbmDeviceDestroy(*gbm);
}

extern "C" int gbm_device_get_fd(struct gbm_device* gbm)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // The device's own dup of the fd it was created with, or -1 when it was created
    // without one and no render node could be opened. That is not an error state:
    // the self-allocated backend never talks to the node, so a device can be fully
    // usable and still have no descriptor to report.
    return gbm->Impl.DeviceFd;
}

extern "C" const char* gbm_device_get_backend_name(struct gbm_device* gbm)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // The name says which memory this device hands out ("mobilegl-dma-heap" or
    // "mobilegl-memfd"), because the callers that read it - a compositor logging its
    // backend, a peer test recording what it got - are asking exactly that. The
    // string lives in the device and outlives every call that can read it.
    return gbm->Impl.BackendName.c_str();
}

extern "C" int gbm_device_is_format_supported(struct gbm_device* gbm, uint32_t format, uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Every buffer this device produces is linear, mappable and shareable, so the
    // usage flags do not narrow the answer to any of the usual combinations -
    // including GBM_BO_USE_SCANOUT, which a linear dma-buf from the system heap
    // satisfies. The one request the backend cannot honour is protected memory.
    if ((flags & GBM_BO_USE_PROTECTED) != 0)
    {
        return 0;
    }
    return GbmFormatIsSupported(format) ? 1 : 0;
}

extern "C" int gbm_device_get_format_modifier_plane_count(struct gbm_device* gbm, uint32_t format, uint64_t modifier)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Only linear memory exists here, so any other modifier names a buffer this
    // device will never produce and the answer is zero planes. INVALID is answered
    // as well, because it means "no modifier was declared", which is the same
    // layout question asked without an answer.
    if (modifier != DrmFormatModLinear && modifier != DrmFormatModInvalid)
    {
        return 0;
    }
    GbmPlaneLayout planes[GbmMaxPlanes] = {};
    Int planeCount = 0;
    SizeT totalBytes = 0;
    if (!GbmDescribeFormat(format, 1, 1, planes, planeCount, totalBytes))
    {
        return 0;
    }
    return planeCount;
}

// ---------------------------------------------------------------------------
// Buffer objects
// ---------------------------------------------------------------------------

extern "C" struct gbm_bo* gbm_bo_create(struct gbm_device* gbm, uint32_t width, uint32_t height, uint32_t format,
                                       uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // No modifier list: the buffer is linear and gbm_bo_get_modifier says so.
    return GbmBufferCreate(*gbm, width, height, format, flags, nullptr, 0);
}

extern "C" struct gbm_bo* gbm_bo_create_with_modifiers(struct gbm_device* gbm, uint32_t width, uint32_t height,
                                                      uint32_t format, const uint64_t* modifiers,
                                                      const unsigned int count)
{
    // The modifier-less variant is the one with no flags parameter, so it is the
    // flags-0 case of the other, and saying that here keeps one implementation.
    return gbm_bo_create_with_modifiers2(gbm, width, height, format, modifiers, count, 0);
}

extern "C" struct gbm_bo* gbm_bo_create_with_modifiers2(struct gbm_device* gbm, uint32_t width, uint32_t height,
                                                       uint32_t format, const uint64_t* modifiers,
                                                       const unsigned int count, uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return GbmBufferCreate(*gbm, width, height, format, flags, modifiers, (Uint32)count);
}

extern "C" struct gbm_bo* gbm_bo_import(struct gbm_device* gbm, uint32_t type, void* buffer, uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return GbmBufferImport(*gbm, type, buffer, flags);
}

extern "C" void* gbm_bo_map(struct gbm_bo* bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                            uint32_t flags, uint32_t* stride, void** map_data)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    // The rectangle's size is not used to size the mapping: the whole buffer is
    // mapped once, at its own length, and the rectangle is applied as an offset into
    // it. That is what lets two overlapping maps share one address, and it is why a
    // buffer's mapping stays valid for a caller that only ever asks for a corner.
    (void)width;
    (void)height;
    (void)flags;
    std::lock_guard<std::mutex> guard(*mutex);
    Uint32 mapStride = 0;
    void* address = GbmBufferMap(*bo, x, y, mapStride);
    if (address == nullptr)
    {
        return nullptr;
    }
    if (stride != nullptr)
    {
        *stride = mapStride;
    }
    if (map_data != nullptr)
    {
        // The same address the function returns. Callers pass back either one to
        // gbm_bo_unmap, and both have to name this mapping.
        *map_data = address;
    }
    return address;
}

extern "C" void gbm_bo_unmap(struct gbm_bo* bo, void* map_data)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    GbmBufferUnmap(*bo, map_data);
}

extern "C" uint32_t gbm_bo_get_width(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.Width;
}

extern "C" uint32_t gbm_bo_get_height(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.Height;
}

extern "C" uint32_t gbm_bo_get_stride(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.Strides[0];
}

extern "C" uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo* bo, int plane)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // A plane this buffer does not have is answered with zero, which is the only
    // value a caller can tell apart from a real stride.
    if (plane < 0 || plane >= bo->Impl.PlaneCount)
    {
        errno = EINVAL;
        return 0;
    }
    return bo->Impl.Strides[plane];
}

extern "C" uint32_t gbm_bo_get_format(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.Format;
}

extern "C" uint32_t gbm_bo_get_bpp(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Bits per pixel: a 32 bit format answers 32, which is what a caller computing a
    // row length in bits or a row length in bytes from the same number expects.
    return bo->Impl.BytesPerPixel * 8;
}

extern "C" uint32_t gbm_bo_get_offset(struct gbm_bo* bo, int plane)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    if (plane < 0 || plane >= bo->Impl.PlaneCount)
    {
        errno = EINVAL;
        return 0;
    }
    return bo->Impl.Offsets[plane];
}

extern "C" struct gbm_device* gbm_bo_get_device(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    return bo->Impl.Owner;
}

extern "C" union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo* bo)
{
    union gbm_bo_handle handle;
    handle.ptr = nullptr;
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        handle.s32 = -1;
        return handle;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // The kernel-level name of a dma-buf in this process is its file descriptor. A
    // GEM handle would be a name that exists only relative to a DRM node, and this
    // library deliberately does not require one, so the fd is the only answer here
    // that is true for every buffer it hands out. Callers that need a name for a
    // device should ask for the fd and pass that.
    handle.s32 = bo->Impl.Fds[0];
    return handle;
}

extern "C" int gbm_bo_get_fd(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // A dup, close-on-exec, owned by the caller. Closing it cannot affect the buffer
    // or any other caller's copy.
    return GbmBufferDupFd(*bo, 0);
}

extern "C" uint64_t gbm_bo_get_modifier(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return DrmFormatModInvalid;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.Modifier;
}

extern "C" int gbm_bo_get_plane_count(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.PlaneCount;
}

extern "C" union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo* bo, int plane)
{
    union gbm_bo_handle handle;
    handle.ptr = nullptr;
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        handle.s32 = -1;
        return handle;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    if (plane < 0 || plane >= bo->Impl.PlaneCount)
    {
        errno = EINVAL;
        handle.s32 = -1;
        return handle;
    }
    // Same reasoning as gbm_bo_get_handle: the fd is the buffer's kernel name. Every
    // plane of a self-allocated buffer lives in the one dma-buf, so all of them
    // answer with that descriptor; the plane's position inside it is
    // gbm_bo_get_offset.
    const Int fd = bo->Impl.Fds[plane] >= 0 ? bo->Impl.Fds[plane] : bo->Impl.Fds[0];
    handle.s32 = fd;
    return handle;
}

extern "C" int gbm_bo_get_fd_for_plane(struct gbm_bo* bo, int plane)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return GbmBufferDupFd(*bo, plane);
}

extern "C" int gbm_bo_write(struct gbm_bo* bo, const void* buf, size_t count)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr || buf == nullptr)
    {
        errno = EINVAL;
        return -1;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    if (count == 0)
    {
        return 0;
    }
    if (count > bo->Impl.AllocationSize)
    {
        // Refused rather than truncated: a caller that writes 4 MiB into a 1 MiB
        // buffer and is told "0 bytes written, success" would read back memory it
        // never wrote and call the result correct.
        errno = EINVAL;
        return -1;
    }
    // The write goes through the same counted mapping the rest of the API uses, so a
    // caller that already holds the buffer mapped does not get a second mapping (or
    // lose its own) behind its back.
    Uint32 stride = 0;
    void* address = GbmBufferMap(*bo, 0, 0, stride);
    if (address == nullptr)
    {
        return -1;
    }
    memcpy(address, buf, count);
    GbmBufferUnmap(*bo, address);
    return 0;
}

extern "C" void gbm_bo_set_user_data(struct gbm_bo* bo, void* data,
                                     void (*destroy_user_data)(struct gbm_bo*, void*))
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Assigning over existing data does not call the old callback: the record calls
    // it exactly once, when the buffer goes away, which is the one moment a caller
    // can still recognise the pointer it registered.
    bo->Impl.UserData = data;
    bo->Impl.UserDataDestroy = destroy_user_data;
}

extern "C" void* gbm_bo_get_user_data(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return bo->Impl.UserData;
}

extern "C" void gbm_bo_destroy(struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(bo);
    if (mutex == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Drops this caller's reference. A buffer a surface still holds stays alive -
    // that is what the surface's own reference is for - so destroying a buffer that
    // is currently presented is not a use-after-free waiting to happen.
    GbmBufferRelease(*bo->Impl.Owner, *bo);
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

extern "C" struct gbm_surface* gbm_surface_create(struct gbm_device* gbm, uint32_t width, uint32_t height,
                                                  uint32_t format, uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return GbmSurfaceCreate(*gbm, width, height, format, flags, nullptr, 0);
}

extern "C" struct gbm_surface* gbm_surface_create_with_modifiers(struct gbm_device* gbm, uint32_t width,
                                                                 uint32_t height, uint32_t format,
                                                                 const uint64_t* modifiers,
                                                                 const unsigned int count)
{
    // As with the buffer pair: the modifier-less form is the flags-0 case.
    return gbm_surface_create_with_modifiers2(gbm, width, height, format, modifiers, count, 0);
}

extern "C" struct gbm_surface* gbm_surface_create_with_modifiers2(struct gbm_device* gbm, uint32_t width,
                                                                  uint32_t height, uint32_t format,
                                                                  const uint64_t* modifiers,
                                                                  const unsigned int count, uint32_t flags)
{
    std::mutex* mutex = MutexOf(gbm);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    return GbmSurfaceCreate(*gbm, width, height, format, flags, modifiers, (Uint32)count);
}

extern "C" struct gbm_bo* gbm_surface_lock_front_buffer(struct gbm_surface* surface)
{
    std::mutex* mutex = MutexOf(surface);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Null with errno = EBUSY when the compositor still holds every buffer: that is a
    // state the caller has to handle, and pretending otherwise by handing out a
    // buffer that is still being scanned out would be a torn frame rather than an
    // error.
    return GbmSurfaceLockFront(*surface);
}

extern "C" void gbm_surface_release_buffer(struct gbm_surface* surface, struct gbm_bo* bo)
{
    std::mutex* mutex = MutexOf(surface);
    if (mutex == nullptr || bo == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    GbmSurfaceReleaseBuffer(*surface, *bo);
}

extern "C" int gbm_surface_has_free_buffers(struct gbm_surface* surface)
{
    std::mutex* mutex = MutexOf(surface);
    if (mutex == nullptr)
    {
        errno = EINVAL;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    // Truthful by construction: the swapchain exists from the moment the surface
    // does, so this answers about buffers that were really allocated rather than
    // about how many could still be.
    return GbmSurfaceHasFreeBuffers(*surface) ? 1 : 0;
}

extern "C" void gbm_surface_destroy(struct gbm_surface* surface)
{
    std::mutex* mutex = MutexOf(surface);
    if (mutex == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> guard(*mutex);
    GbmSurfaceDestroy(*surface);
}

// ---------------------------------------------------------------------------
// Format names
// ---------------------------------------------------------------------------

extern "C" char* gbm_format_get_name(uint32_t gbm_format, struct gbm_format_name_desc* desc)
{
    if (desc == nullptr)
    {
        errno = EINVAL;
        return nullptr;
    }
    // The name of a fourcc is its four characters, most significant byte last, and
    // that is all there is to it: a caller compares these strings against "AR24" and
    // friends, and the big-endian bit is part of the value rather than something to
    // spell out here.
    desc->name[0] = (char)(gbm_format & 0xffu);
    desc->name[1] = (char)((gbm_format >> 8) & 0xffu);
    desc->name[2] = (char)((gbm_format >> 16) & 0xffu);
    desc->name[3] = (char)((gbm_format >> 24) & 0xffu);
    desc->name[4] = '\0';
    return desc->name;
}

// End of Source File Header
