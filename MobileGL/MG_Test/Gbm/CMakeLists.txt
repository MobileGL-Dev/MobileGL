# MobileGL - MobileGL/MG_Test/Gbm/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

cmake_minimum_required(VERSION 3.14)

# mobilegl_gbm.so through the system's GBM loader, the calls Xwayland's glamor makes, over a fake of
# libMobileGL's shared-image C ABI. Only where the GBM backend itself is built (gbm headers found).
find_library(MOBILEGL_TEST_GBM_LIBRARY gbm)
if (NOT TARGET mobilegl_gbm OR NOT MOBILEGL_TEST_GBM_LIBRARY)
    message(STATUS "MobileGL: GbmGlamorTest not built (no mobilegl_gbm target or no libgbm)")
    return()
endif()

add_library(mobilegl_fake_shared_images MODULE FakeSharedImageLibrary.cpp)
set_target_properties(mobilegl_fake_shared_images PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
target_include_directories(mobilegl_fake_shared_images PRIVATE ${MGL_ROOT}/MobileGL/MG_Impl)

add_executable(GbmGlamorTest GbmGlamorTest.cpp)
add_dependencies(GbmGlamorTest mobilegl_gbm mobilegl_fake_shared_images)
target_include_directories(GbmGlamorTest PRIVATE ${MOBILEGL_GBM_INCLUDE_DIR})
target_compile_definitions(GbmGlamorTest PRIVATE
        MOBILEGL_TEST_GBM_BACKEND_DIR="$<TARGET_FILE_DIR:mobilegl_gbm>"
        MOBILEGL_TEST_FAKE_SHARED_IMAGE_LIBRARY="$<TARGET_FILE:mobilegl_fake_shared_images>"
)
target_link_libraries(GbmGlamorTest PRIVATE MobileGLTestMain ${MOBILEGL_TEST_GBM_LIBRARY} ${CMAKE_DL_LIBS})

include(GoogleTest)
gtest_discover_tests(GbmGlamorTest DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
