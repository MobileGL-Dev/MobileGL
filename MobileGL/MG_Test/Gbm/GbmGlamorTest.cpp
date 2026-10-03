// MobileGL - MobileGL/MG_Test/Gbm/GbmGlamorTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// mobilegl_gbm.so THROUGH THE SYSTEM'S GBM LOADER, the way Xwayland's glamor drives it
// (hw/xwayland/xwayland-glamor-gbm.c, 24.1): gbm_create_device on a character device with
// GBM_BACKEND=mobilegl, window pixmaps from gbm_bo_create_with_modifiers2 with the compositor's
// {INVALID, LINEAR} (else gbm_bo_create), their planes exported for EGL and linux-dmabuf, and a
// DRI3 client's buffer imported back with GBM_BO_IMPORT_FD / _FD_MODIFIER. The shared images come
// from a fake of libMobileGL's C ABI (FakeSharedImageLibrary.cpp).

#include <gbm.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

namespace {
    using CountFn = int (*)();

    class GbmGlamor : public ::testing::Test {
    protected:
        void SetUp() override {
            ::setenv("GBM_BACKEND", "mobilegl", 1);
            ::setenv("GBM_BACKENDS_PATH", MOBILEGL_TEST_GBM_BACKEND_DIR, 1);
            ::setenv("MOBILEGL_GBM_LIBRARY", MOBILEGL_TEST_FAKE_SHARED_IMAGE_LIBRARY, 1);
            m_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            ASSERT_GE(m_fd, 0);
            m_gbm = gbm_create_device(m_fd);
            ASSERT_NE(m_gbm, nullptr) << "libgbm did not take the mobilegl backend";
            void* fake = dlopen(MOBILEGL_TEST_FAKE_SHARED_IMAGE_LIBRARY, RTLD_NOW | RTLD_NOLOAD);
            ASSERT_NE(fake, nullptr) << "the backend did not load the shared-image library";
            m_live = reinterpret_cast<CountFn>(dlsym(fake, "fake_shared_image_live"));
            m_imports = reinterpret_cast<CountFn>(dlsym(fake, "fake_shared_image_imports"));
            ASSERT_TRUE(m_live && m_imports);
            m_liveAtStart = m_live();
        }
        void TearDown() override {
            if (m_gbm != nullptr) gbm_device_destroy(m_gbm);
            if (m_fd >= 0) ::close(m_fd);
        }

        int m_fd = -1;
        gbm_device* m_gbm = nullptr;
        CountFn m_live = nullptr;
        CountFn m_imports = nullptr;
        int m_liveAtStart = 0;
    };
} // namespace

TEST_F(GbmGlamor, TheDeviceIsMobileGLsAndTakesTheFormatsGlamorAsksFor) {
    // Xwayland names the screen's GLX vendor after the backend (anything but "drm").
    EXPECT_STREQ(gbm_device_get_backend_name(m_gbm), "mobilegl");
    // gbm_format_for_depth: 24 -> XRGB8888 (ARGB8888 under GLES glamor), 32 -> ARGB8888.
    EXPECT_TRUE(gbm_device_is_format_supported(m_gbm, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING));
    EXPECT_TRUE(gbm_device_is_format_supported(m_gbm, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT));
    EXPECT_FALSE(gbm_device_is_format_supported(m_gbm, GBM_FORMAT_RGB565, GBM_BO_USE_RENDERING));
}

TEST_F(GbmGlamor, AWindowPixmapIsASharedImageWithOneExportablePlane) {
    // The compositor's tranche for a MobileGL EGL lists INVALID and LINEAR (implicit modifiers).
    const uint64_t modifiers[] = {DRM_FORMAT_MOD_INVALID, DRM_FORMAT_MOD_LINEAR};
    gbm_bo* bo = gbm_bo_create_with_modifiers2(m_gbm, 640, 480, GBM_FORMAT_XRGB8888, modifiers, 2,
                                               GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    ASSERT_NE(bo, nullptr) << std::strerror(errno);
    EXPECT_EQ(m_live(), m_liveAtStart + 1);
    EXPECT_EQ(gbm_bo_get_width(bo), 640u);
    EXPECT_EQ(gbm_bo_get_height(bo), 480u);
    EXPECT_EQ(gbm_bo_get_format(bo), GBM_FORMAT_XRGB8888);
    EXPECT_EQ(gbm_bo_get_modifier(bo), DRM_FORMAT_MOD_INVALID) << "the layout is the allocator's";
    // xwl_glamor_gbm_create_pixmap_for_bo / init_buffer_params_with_modifiers:
    ASSERT_EQ(gbm_bo_get_plane_count(bo), 1);
    const int fd = gbm_bo_get_fd_for_plane(bo, 0);
    ASSERT_GE(fd, 0);
    EXPECT_NE(gbm_bo_get_handle_for_plane(bo, 0).u32, 0u);
    EXPECT_GE(gbm_bo_get_stride_for_plane(bo, 0), 640u * 4u);
    EXPECT_EQ(gbm_bo_get_offset(bo, 0), 0u);
    // Exports are duplicates of one dma-buf: the same inode every time.
    const int again = gbm_bo_get_fd(bo);
    struct stat a {}, b {};
    ASSERT_EQ(::fstat(fd, &a), 0);
    ASSERT_EQ(::fstat(again, &b), 0);
    EXPECT_EQ(a.st_ino, b.st_ino);
    ::close(fd);
    ::close(again);
    gbm_bo_destroy(bo);
    EXPECT_EQ(m_live(), m_liveAtStart) << "destroying the bo released the image";
}

TEST_F(GbmGlamor, AModifierListWithoutInvalidIsRefusedAndGlamorsFallbackWorks) {
    const uint64_t linearOnly[] = {DRM_FORMAT_MOD_LINEAR};
    EXPECT_EQ(gbm_bo_create_with_modifiers2(m_gbm, 64, 64, GBM_FORMAT_XRGB8888, linearOnly, 1, GBM_BO_USE_RENDERING),
              nullptr);
    // glamor's implicit-modifier fallback.
    gbm_bo* bo = gbm_bo_create(m_gbm, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    ASSERT_NE(bo, nullptr);
    EXPECT_EQ(gbm_bo_get_modifier(bo), DRM_FORMAT_MOD_INVALID);
    gbm_bo_destroy(bo);
    // ...which with a LINEAR-only tranche asks for a linear layout, and that no shared image promises.
    EXPECT_EQ(gbm_bo_create(m_gbm, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR), nullptr);
}

TEST_F(GbmGlamor, ADri3ClientsBufferImportsBackAsTheSameImage) {
    // The GLX client's side: an image it allocated (here through the same ABI) handed over as a
    // dma-buf with PixmapFromBuffer(s), modifier INVALID.
    gbm_bo* clientBo = gbm_bo_create(m_gbm, 320, 200, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    ASSERT_NE(clientBo, nullptr);
    const int fd = gbm_bo_get_fd(clientBo);
    const int importsBefore = m_imports();

    // glamor_pixmap_from_fds, modifier INVALID: GBM_BO_IMPORT_FD with the depth's format.
    gbm_import_fd_data plain{};
    plain.fd = fd;
    plain.width = 320;
    plain.height = 200;
    plain.stride = gbm_bo_get_stride(clientBo);
    plain.format = GBM_FORMAT_XRGB8888;
    gbm_bo* imported = gbm_bo_import(m_gbm, GBM_BO_IMPORT_FD, &plain, GBM_BO_USE_RENDERING);
    ASSERT_NE(imported, nullptr);
    EXPECT_EQ(m_imports(), importsBefore + 1);
    // ...and re-exported for EGL_LINUX_DMA_BUF_EXT: still that image.
    const int reexport = gbm_bo_get_fd_for_plane(imported, 0);
    struct stat a {}, b {};
    ::fstat(fd, &a);
    ::fstat(reexport, &b);
    EXPECT_EQ(a.st_ino, b.st_ino);
    ::close(reexport);

    // A client that sent an explicit modifier list: GBM_BO_IMPORT_FD_MODIFIER.
    gbm_import_fd_modifier_data withModifier{};
    withModifier.width = 320;
    withModifier.height = 200;
    withModifier.format = GBM_FORMAT_XRGB8888;
    withModifier.num_fds = 1;
    withModifier.fds[0] = fd;
    withModifier.strides[0] = static_cast<int>(gbm_bo_get_stride(clientBo));
    withModifier.modifier = DRM_FORMAT_MOD_INVALID;
    gbm_bo* importedModifier = gbm_bo_import(m_gbm, GBM_BO_IMPORT_FD_MODIFIER, &withModifier, GBM_BO_USE_RENDERING);
    ASSERT_NE(importedModifier, nullptr);

    // A size that disagrees with the image is refused.
    plain.width = 321;
    EXPECT_EQ(gbm_bo_import(m_gbm, GBM_BO_IMPORT_FD, &plain, GBM_BO_USE_RENDERING), nullptr);

    gbm_bo_destroy(importedModifier);
    gbm_bo_destroy(imported);
    gbm_bo_destroy(clientBo);
    ::close(fd);
    EXPECT_EQ(m_live(), m_liveAtStart) << "every reference was dropped";
}

TEST_F(GbmGlamor, AForeignDmaBufIsRefused) {
    // A buffer the MobileGL server never allocated (another driver's client) cannot become a pixmap.
    const int foreign = ::memfd_create("foreign", MFD_CLOEXEC);
    ASSERT_GE(foreign, 0);
    ASSERT_EQ(::ftruncate(foreign, 64 * 64 * 4), 0);
    gbm_import_fd_data data{};
    data.fd = foreign;
    data.width = 64;
    data.height = 64;
    data.stride = 256;
    data.format = GBM_FORMAT_XRGB8888;
    EXPECT_EQ(gbm_bo_import(m_gbm, GBM_BO_IMPORT_FD, &data, GBM_BO_USE_RENDERING), nullptr);
    ::close(foreign);
}
