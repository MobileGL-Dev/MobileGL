// MobileGL - MobileGL/MG_Test/Gbm/FakeSharedImageLibrary.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A stand-in for libMobileGL.so's shared-image C ABI (MG_Impl/SharedImageApi.h), loaded by
// mobilegl_gbm.so through MOBILEGL_GBM_LIBRARY in GbmGlamorTest: images are memfds, identified on
// import by inode as the server identifies its dma-bufs. fake_shared_image_* let the test look in.

#include "SharedImageApi.h"

#include <cerrno>
#include <map>
#include <mutex>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))

namespace {
    struct FakeImage {
        int fd = -1;
        ino_t inode = 0;
        uint32_t width = 0, height = 0, fourcc = 0;
        int references = 0;
    };
    std::mutex g_mutex;
    std::map<uint64_t, FakeImage> g_images;
    uint64_t g_nextId = 1;
    int g_imports = 0;

    bool Supported(uint32_t fourcc) {
        return fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_ABGR8888 || fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_XBGR8888 ||
               fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_ARGB8888 || fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_XRGB8888 ||
               fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_NV12;
    }
} // namespace

FAKE_EXPORT uint32_t mobilegl_shared_image_abi_version(void) { return MOBILEGL_SHARED_IMAGE_ABI_VERSION; }

FAKE_EXPORT int mobilegl_server_available(void) { return 1; }

FAKE_EXPORT int mobilegl_shared_image_formats(uint32_t* formats, uint32_t capacity) {
    const uint32_t all[] = {MOBILEGL_SHARED_IMAGE_FOURCC_ABGR8888, MOBILEGL_SHARED_IMAGE_FOURCC_XBGR8888,
                            MOBILEGL_SHARED_IMAGE_FOURCC_ARGB8888, MOBILEGL_SHARED_IMAGE_FOURCC_XRGB8888,
                            MOBILEGL_SHARED_IMAGE_FOURCC_NV12};
    for (uint32_t i = 0; i < 5 && i < capacity; ++i) formats[i] = all[i];
    return 5;
}

FAKE_EXPORT int mobilegl_shared_image_allocate(uint32_t width, uint32_t height, uint32_t fourcc,
                                               struct mobilegl_shared_image* out) {
    if (width == 0 || height == 0 || !Supported(fourcc) || out == nullptr) return -EINVAL;
    const int fd = ::memfd_create("fake-shared-image", MFD_CLOEXEC);
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(width) * height * 4) != 0) return -EIO;
    struct stat st {};
    ::fstat(fd, &st);
    const std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t id = g_nextId++;
    g_images[id] = FakeImage{fd, st.st_ino, width, height, fourcc, 1};
    out->width = width;
    out->height = height;
    out->fourcc = fourcc;
    out->id = id;
    out->fd = ::dup(fd);
    out->stride = width * 4;
    out->offset = 0;
    out->modifier = 0x00ffffffffffffffull;
    if (fourcc == MOBILEGL_SHARED_IMAGE_FOURCC_NV12) {
        out->stride = width;
        out->plane1_stride = width;
        out->plane1_offset = width * height;
    }
    return 0;
}

FAKE_EXPORT int mobilegl_shared_image_import(int fd, uint32_t width, uint32_t height, uint32_t fourcc,
                                             uint64_t* out_id) {
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !Supported(fourcc)) return -EINVAL;
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [id, image] : g_images) {
        if (image.inode != st.st_ino) continue;
        if (image.width != width || image.height != height) return -EIO;
        ++image.references;
        ++g_imports;
        if (out_id != nullptr) *out_id = id;
        return 0;
    }
    return -EIO; // not one of "the server's" images
}

FAKE_EXPORT int mobilegl_shared_image_release(uint64_t id) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_images.find(id);
    if (it == g_images.end()) return -EINVAL;
    if (--it->second.references == 0) {
        ::close(it->second.fd);
        g_images.erase(it);
    }
    return 0;
}

FAKE_EXPORT int fake_shared_image_live(void) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_images.size());
}

FAKE_EXPORT int fake_shared_image_imports(void) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_imports;
}

// A YUV import names an image by its descriptor as an RGBA one does; the planes are not looked at.
FAKE_EXPORT int mobilegl_shared_image_import_planes(int fd, uint32_t width, uint32_t height, uint32_t fourcc,
                                                    const struct mobilegl_shared_image_planes* planes, uint64_t* out_id) {
    if (planes == nullptr || planes->plane_count != 2 || fourcc != MOBILEGL_SHARED_IMAGE_FOURCC_NV12) return -EINVAL;
    return mobilegl_shared_image_import(fd, width, height, fourcc, out_id);
}
