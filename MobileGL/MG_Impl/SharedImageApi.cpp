// MobileGL - MobileGL/MG_Impl/SharedImageApi.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The shared-image C ABI (SharedImageApi.h): a thin forward onto the active backend object's
// client-side shared-image calls (BackendObject_Remote under split). Linux-only, split-only: the
// CMake block that lists this file says so, and nothing in the library calls it.

#include "SharedImageApi.h"

#include <Defines.h>
#include <Init.h>
#include <MG_Backend/BackendObjects.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#include <unistd.h>

namespace {
    using MobileGL::MG_Backend::BackendObject;

    constexpr uint32_t kFormats[] = {MOBILEGL_SHARED_IMAGE_FOURCC_ABGR8888, MOBILEGL_SHARED_IMAGE_FOURCC_XBGR8888};

    bool FourccSupported(uint32_t fourcc) {
        for (uint32_t format : kFormats) {
            if (format == fourcc) return true;
        }
        return false;
    }

    // Serializes the calls against each other (not against GL calls: see the header).
    std::mutex g_apiMutex;

    // Allocate and import bring the session up, exactly as the process's first EGL entry point
    // would: the GBM device is created before any EGL display (a compositor's DRM device is what
    // its GBM display is made from), so failing here instead would only push the bring-up to a
    // caller that has no reason to know it is needed. Release never does - an image can only be
    // held through a session that already exists.
    BackendObject* ActiveBackend(bool bringUp) {
        if (bringUp) MobileGL::EnsureInitialized();
        return MobileGL::MG_Backend::pActiveBackendObject.get();
    }
} // namespace

MOBILEGL_EXPORT uint32_t mobilegl_shared_image_abi_version(void) {
    return MOBILEGL_SHARED_IMAGE_ABI_VERSION;
}

MOBILEGL_EXPORT int mobilegl_shared_image_formats(uint32_t* formats, uint32_t capacity) {
    constexpr uint32_t total = sizeof(kFormats) / sizeof(kFormats[0]);
    if (formats == nullptr && capacity != 0) return -EINVAL;
    for (uint32_t i = 0; i < total && i < capacity; ++i) formats[i] = kFormats[i];
    return static_cast<int>(total);
}

MOBILEGL_EXPORT int mobilegl_shared_image_allocate(uint32_t width, uint32_t height, uint32_t fourcc,
                                                   struct mobilegl_shared_image* out) {
    if (out == nullptr || out->struct_size < sizeof(uint32_t)) return -EINVAL;
    if (width == 0 || height == 0 || !FourccSupported(fourcc)) return -EINVAL;
    const std::lock_guard<std::mutex> lock(g_apiMutex);
    BackendObject* backend = ActiveBackend(true);
    if (backend == nullptr) return -EIO;
    MobileGL::MG_Backend::SharedImageExport image{};
    if (!backend->AllocateSharedImage(width, height, fourcc, &image) || image.Fd < 0) {
        if (image.Fd >= 0) ::close(image.Fd);
        return -EIO;
    }
    mobilegl_shared_image answer{};
    answer.struct_size = out->struct_size;
    answer.width = image.Width;
    answer.height = image.Height;
    answer.fourcc = image.Fourcc;
    answer.id = image.Id;
    answer.fd = image.Fd;
    answer.stride = image.Stride;
    answer.offset = image.Offset;
    answer.modifier = image.Modifier;
    // An older caller's struct is a prefix of this one; a newer caller's tail stays as it was.
    std::memcpy(out, &answer, std::min<size_t>(out->struct_size, sizeof(answer)));
    return 0;
}

MOBILEGL_EXPORT int mobilegl_shared_image_import(int fd, uint32_t width, uint32_t height, uint32_t fourcc,
                                                 uint64_t* out_id) {
    if (fd < 0 || out_id == nullptr || width == 0 || height == 0 || !FourccSupported(fourcc)) return -EINVAL;
    const std::lock_guard<std::mutex> lock(g_apiMutex);
    BackendObject* backend = ActiveBackend(true);
    if (backend == nullptr) return -EIO;
    uint64_t id = 0;
    if (!backend->ImportSharedImage(fd, width, height, fourcc, &id)) return -EIO;
    *out_id = id;
    return 0;
}

MOBILEGL_EXPORT int mobilegl_shared_image_release(uint64_t id) {
    const std::lock_guard<std::mutex> lock(g_apiMutex);
    BackendObject* backend = ActiveBackend(false);
    if (backend == nullptr) return -ENODEV;
    return backend->ReleaseSharedImage(id) ? 0 : -EIO;
}
