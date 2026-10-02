// MobileGL - MobileGL/MG_Gbm/MobileGLGbm.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// mobilegl_gbm.so: a GBM backend (the loader's backend ABI, gbm_backend_abi.h) whose buffer
// objects are MobileGL shared images - colour buffers the MobileGL server allocates and exports
// as dma-bufs (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md). Selected with
// GBM_BACKEND=mobilegl; the loader then finds <dir>/mobilegl_gbm.so on its backend path.
//
// THE DEVICE DESCRIPTOR IS OPAQUE. The fd a gbm_device is created on is only the identity the
// caller chose (a render node, or any character device); no ioctl is ever issued on it, because
// the buffers do not come from that device's driver - they come from the server, over the
// process's MobileGL client session. Whatever node the caller holds is therefore equally good,
// and none is vendor-specific.
//
// THE LAYOUT IS THE ALLOCATOR'S. Buffers report DRM_FORMAT_MOD_INVALID and a single plane; a
// request that needs a known layout (LINEAR, or a modifier list without INVALID) is refused
// rather than answered with a buffer whose layout is not what was asked for.
//
// Buffer operations reach the server through libMobileGL.so's shared-image C ABI
// (MG_Impl/SharedImageApi.h), resolved with dlsym on first use - preferably from the instance the
// process has already loaded (its EGL vendor), so the buffers belong to the same session.

#include "SharedImageApi.h"

#include <gbm.h>
#include <gbm_backend_abi.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#define MOBILEGL_GBM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
    constexpr uint64_t kModifierLinear = 0;
    constexpr uint64_t kModifierInvalid = 0x00ffffffffffffffull;

    // Usages no shared image can honour: a CPU-written cursor (bo_write), a guaranteed-linear
    // layout, protected content. Scanout/rendering/texturing are accepted - every caller of
    // gbm_bo_create passes them, and a shared image serves all three.
    constexpr uint32_t kRefusedUsage = GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE | GBM_BO_USE_LINEAR | GBM_BO_USE_PROTECTED;

    const gbm_core* g_core = nullptr;

    struct Device {
        gbm_device base; // first: the loader's gbm_device* is this object
    };

    struct Bo {
        gbm_bo base; // first: the loader's gbm_bo* is this object
        uint64_t imageId;
        int fd; // owned; every get_fd hands out a duplicate
        uint32_t offset;
        uint64_t modifier;
    };

    // ---- libMobileGL's shared-image ABI -----------------------------------------------------

    struct SharedImageApi {
        PFN_mobilegl_shared_image_allocate allocate = nullptr;
        PFN_mobilegl_shared_image_import import = nullptr;
        PFN_mobilegl_shared_image_release release = nullptr;
    };

    // The library the process already has (by its soname: glvnd loads it by path, which records
    // the soname too), else MOBILEGL_GBM_LIBRARY, else the loader's search path. Never closed:
    // libMobileGL leaks its globals at exit by design and must not be unloaded under them.
    const SharedImageApi* Api() {
        static std::once_flag once;
        static SharedImageApi api;
        static bool resolved = false;
        std::call_once(once, [] {
            void* lib = dlopen("libMobileGL.so", RTLD_NOW | RTLD_NOLOAD);
            if (lib == nullptr) {
                const char* path = std::getenv("MOBILEGL_GBM_LIBRARY");
                lib = dlopen(path != nullptr && path[0] != '\0' ? path : "libMobileGL.so", RTLD_NOW | RTLD_LOCAL);
            }
            if (lib == nullptr) {
                std::fprintf(stderr, "mobilegl_gbm: libMobileGL.so could not be loaded: %s\n", dlerror());
                return;
            }
            const auto version = reinterpret_cast<PFN_mobilegl_shared_image_abi_version>(
                dlsym(lib, "mobilegl_shared_image_abi_version"));
            api.allocate = reinterpret_cast<PFN_mobilegl_shared_image_allocate>(dlsym(lib, "mobilegl_shared_image_allocate"));
            api.import = reinterpret_cast<PFN_mobilegl_shared_image_import>(dlsym(lib, "mobilegl_shared_image_import"));
            api.release = reinterpret_cast<PFN_mobilegl_shared_image_release>(dlsym(lib, "mobilegl_shared_image_release"));
            if (version == nullptr || version() < 1 || api.allocate == nullptr || api.import == nullptr ||
                api.release == nullptr) {
                std::fprintf(stderr, "mobilegl_gbm: this libMobileGL.so has no shared-image ABI\n");
                return;
            }
            resolved = true;
        });
        return resolved ? &api : nullptr;
    }

    // ---- formats ----------------------------------------------------------------------------

    uint32_t Canonical(uint32_t format) {
        if (g_core != nullptr && g_core->v0.format_canonicalize != nullptr) return g_core->v0.format_canonicalize(format);
        // The two legacy GBM_BO_FORMAT_* enumerants are the only non-fourcc formats.
        if (format == GBM_BO_FORMAT_XRGB8888) return GBM_FORMAT_XRGB8888;
        if (format == GBM_BO_FORMAT_ARGB8888) return GBM_FORMAT_ARGB8888;
        return format;
    }

    // What a shared image can be. The ARGB/XRGB orders are stored like the ABGR/XBGR ones: the
    // layout is the server's (modifier INVALID, no map is offered), so no reader can tell, and
    // every GL that touches the image addresses its channels logically.
    bool FormatSupported(uint32_t format) {
        return format == MOBILEGL_SHARED_IMAGE_FOURCC_ABGR8888 || format == MOBILEGL_SHARED_IMAGE_FOURCC_XBGR8888 ||
               format == MOBILEGL_SHARED_IMAGE_FOURCC_ARGB8888 || format == MOBILEGL_SHARED_IMAGE_FOURCC_XRGB8888;
    }

    bool ModifierAcceptable(uint64_t modifier) { return modifier == kModifierInvalid || modifier == kModifierLinear; }

    // There is no GEM handle behind a shared image; callers that only use the handle as a key
    // (a buffer cache) still need it to be unique.
    uint32_t NextHandle() {
        static std::atomic<uint32_t> next{1};
        uint32_t handle = next.fetch_add(1, std::memory_order_relaxed);
        if (handle == 0) handle = next.fetch_add(1, std::memory_order_relaxed);
        return handle;
    }

    gbm_bo* NewBo(gbm_device* gbm, uint64_t imageId, int fd, uint32_t width, uint32_t height, uint32_t format,
                  uint32_t stride, uint32_t offset, uint64_t modifier) {
        auto* bo = new (std::nothrow) Bo{};
        if (bo == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
        bo->base.gbm = gbm;
        bo->base.v0.width = width;
        bo->base.v0.height = height;
        bo->base.v0.stride = stride;
        bo->base.v0.format = format;
        bo->base.v0.handle.u32 = NextHandle();
        bo->imageId = imageId;
        bo->fd = fd;
        bo->offset = offset;
        bo->modifier = modifier;
        return &bo->base;
    }

    Bo* AsBo(gbm_bo* bo) { return reinterpret_cast<Bo*>(bo); }

    // ---- device -----------------------------------------------------------------------------

    void DeviceDestroy(gbm_device* gbm) { delete reinterpret_cast<Device*>(gbm); }

    int IsFormatSupported(gbm_device*, uint32_t format, uint32_t usage) {
        return FormatSupported(Canonical(format)) && (usage & kRefusedUsage) == 0 ? 1 : 0;
    }

    int GetFormatModifierPlaneCount(gbm_device*, uint32_t format, uint64_t modifier) {
        if (!FormatSupported(Canonical(format)) || !ModifierAcceptable(modifier)) return -1;
        return 1;
    }

    gbm_bo* BoCreate(gbm_device* gbm, uint32_t width, uint32_t height, uint32_t format, uint32_t usage,
                     const uint64_t* modifiers, const unsigned int count) {
        format = Canonical(format);
        if (!FormatSupported(format) || (usage & kRefusedUsage) != 0) {
            errno = EINVAL;
            return nullptr;
        }
        if (count > 0) {
            // The layout is the allocator's, which only a list that admits INVALID accepts.
            bool admitsImplicit = false;
            for (unsigned int i = 0; i < count; ++i) admitsImplicit |= modifiers[i] == kModifierInvalid;
            if (!admitsImplicit) {
                errno = EINVAL;
                return nullptr;
            }
        }
        const SharedImageApi* api = Api();
        if (api == nullptr) {
            errno = ENOSYS;
            return nullptr;
        }
        mobilegl_shared_image image{};
        image.struct_size = sizeof(image);
        image.fd = -1;
        const int rc = api->allocate(width, height, format, &image);
        if (rc != 0 || image.fd < 0) {
            if (image.fd >= 0) ::close(image.fd);
            errno = rc < 0 ? -rc : EIO;
            return nullptr;
        }
        gbm_bo* bo = NewBo(gbm, image.id, image.fd, image.width, image.height, image.fourcc, image.stride,
                           image.offset, image.modifier);
        if (bo == nullptr) {
            api->release(image.id);
            ::close(image.fd);
        }
        return bo;
    }

    gbm_bo* BoImport(gbm_device* gbm, uint32_t type, void* buffer, uint32_t usage) {
        (void)usage;
        if (buffer == nullptr) {
            errno = EINVAL;
            return nullptr;
        }
        int fd = -1;
        uint32_t width = 0, height = 0, format = 0, stride = 0, offset = 0;
        uint64_t modifier = kModifierInvalid;
        switch (type) {
        case GBM_BO_IMPORT_FD: {
            const auto* data = static_cast<const gbm_import_fd_data*>(buffer);
            fd = data->fd;
            width = data->width;
            height = data->height;
            format = data->format;
            stride = data->stride;
            break;
        }
        case GBM_BO_IMPORT_FD_MODIFIER: {
            const auto* data = static_cast<const gbm_import_fd_modifier_data*>(buffer);
            if (data->num_fds != 1 || !ModifierAcceptable(data->modifier)) {
                errno = EINVAL;
                return nullptr;
            }
            fd = data->fds[0];
            width = data->width;
            height = data->height;
            format = data->format;
            stride = static_cast<uint32_t>(data->strides[0]);
            offset = static_cast<uint32_t>(data->offsets[0]);
            modifier = data->modifier;
            break;
        }
        default:
            // A wl_buffer or an EGLImage would have to be resolved through a display this backend
            // does not own; their owners can export a dma-buf and import that instead.
            errno = ENOSYS;
            return nullptr;
        }
        format = Canonical(format);
        if (fd < 0 || width == 0 || height == 0 || !FormatSupported(format)) {
            errno = EINVAL;
            return nullptr;
        }
        const SharedImageApi* api = Api();
        if (api == nullptr) {
            errno = ENOSYS;
            return nullptr;
        }
        // The caller keeps its descriptor; the bo holds its own.
        const int owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (owned < 0) return nullptr;
        uint64_t id = 0;
        const int rc = api->import(owned, width, height, format, &id);
        if (rc != 0) {
            ::close(owned);
            errno = -rc;
            return nullptr;
        }
        gbm_bo* bo = NewBo(gbm, id, owned, width, height, format, stride, offset, modifier);
        if (bo == nullptr) {
            api->release(id);
            ::close(owned);
        }
        return bo;
    }

    // CPU access is not offered: the layout is opaque, so a mapping would need the server to
    // copy through a linear staging buffer, which does not exist yet.
    void* BoMap(gbm_bo*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void**) {
        errno = ENOSYS;
        return nullptr;
    }

    void BoUnmap(gbm_bo*, void*) {}

    int BoWrite(gbm_bo*, const void*, size_t) {
        errno = ENOSYS;
        return -1;
    }

    int BoGetFd(gbm_bo* bo) { return ::fcntl(AsBo(bo)->fd, F_DUPFD_CLOEXEC, 0); }

    int BoGetPlanes(gbm_bo*) { return 1; }

    gbm_bo_handle BoGetHandle(gbm_bo* bo, int plane) {
        gbm_bo_handle handle{};
        if (plane != 0) {
            errno = EINVAL;
            handle.s32 = -1;
            return handle;
        }
        return bo->v0.handle;
    }

    int BoGetPlaneFd(gbm_bo* bo, int plane) {
        if (plane != 0) {
            errno = EINVAL;
            return -1;
        }
        return BoGetFd(bo);
    }

    uint32_t BoGetStride(gbm_bo* bo, int plane) { return plane == 0 ? bo->v0.stride : 0; }

    uint32_t BoGetOffset(gbm_bo* bo, int plane) { return plane == 0 ? AsBo(bo)->offset : 0; }

    uint64_t BoGetModifier(gbm_bo* bo) { return AsBo(bo)->modifier; }

    void BoDestroy(gbm_bo* gbm) {
        Bo* bo = AsBo(gbm);
        if (const SharedImageApi* api = Api()) api->release(bo->imageId);
        if (bo->fd >= 0) ::close(bo->fd);
        delete bo;
    }

    // Window surfaces belong to the EGL platform, not to this allocator.
    gbm_surface* SurfaceCreate(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t, const uint64_t*, const unsigned) {
        errno = ENOSYS;
        return nullptr;
    }

    gbm_bo* SurfaceLockFrontBuffer(gbm_surface*) { return nullptr; }

    void SurfaceReleaseBuffer(gbm_surface*, gbm_bo*) {}

    int SurfaceHasFreeBuffers(gbm_surface*) { return 0; }

    void SurfaceDestroy(gbm_surface*) {}

    gbm_device* CreateDevice(int fd, uint32_t backendVersion) {
        auto* device = new (std::nothrow) Device{};
        if (device == nullptr) return nullptr;
        gbm_device_v0& v0 = device->base.v0;
        v0.backend_version = backendVersion; // the loader checks this is what it asked for
        v0.fd = fd;
        v0.name = "mobilegl";
        v0.destroy = DeviceDestroy;
        v0.is_format_supported = IsFormatSupported;
        v0.get_format_modifier_plane_count = GetFormatModifierPlaneCount;
        v0.bo_create = BoCreate;
        v0.bo_import = BoImport;
        v0.bo_map = BoMap;
        v0.bo_unmap = BoUnmap;
        v0.bo_write = BoWrite;
        v0.bo_get_fd = BoGetFd;
        v0.bo_get_planes = BoGetPlanes;
        v0.bo_get_handle = BoGetHandle;
        v0.bo_get_plane_fd = BoGetPlaneFd;
        v0.bo_get_stride = BoGetStride;
        v0.bo_get_offset = BoGetOffset;
        v0.bo_get_modifier = BoGetModifier;
        v0.bo_destroy = BoDestroy;
        v0.surface_create = SurfaceCreate;
        v0.surface_lock_front_buffer = SurfaceLockFrontBuffer;
        v0.surface_release_buffer = SurfaceReleaseBuffer;
        v0.surface_has_free_buffers = SurfaceHasFreeBuffers;
        v0.surface_destroy = SurfaceDestroy;
        return &device->base;
    }

    // ABI 1 differs from 0 only in the loader populating usage/flags alongside modifiers, which
    // this backend reads as they come; every structure here is the v0 layout.
    const gbm_backend kBackend = {
        .v0 =
            {
                .backend_version = GBM_BACKEND_ABI_VERSION,
                .backend_name = "mobilegl",
                .create_device = CreateDevice,
            },
    };
} // namespace

MOBILEGL_GBM_EXPORT const gbm_backend* GBM_GET_BACKEND_PROC(const gbm_core* core) {
    g_core = core;
    return &kBackend;
}
