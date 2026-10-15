# MobileGL - MobileGL/MG_Gbm/CMakeLists.txt
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# MobileGL - MobileGL/MG_Gbm/CMakeLists.txt
#
# mobilegl_gbm.so: the GBM backend whose buffers are MobileGL shared images (MobileGLGbm.cpp).
# A loadable module for the system GBM loader, NOT part of libMobileGL.so - it reaches the library
# at run time through dlsym. Built against the system's <gbm.h> and <gbm_backend_abi.h> (MIT,
# shipped with the GBM loader's development files); a system without them builds without it.
#
# Usable standalone too (cmake -S MobileGL/MG_Gbm), for building just the plugin.

cmake_minimum_required(VERSION 3.16)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(mobilegl_gbm CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

find_path(MOBILEGL_GBM_BACKEND_ABI_INCLUDE_DIR gbm_backend_abi.h)
find_path(MOBILEGL_GBM_INCLUDE_DIR gbm.h)
if (NOT MOBILEGL_GBM_BACKEND_ABI_INCLUDE_DIR OR NOT MOBILEGL_GBM_INCLUDE_DIR)
    message(STATUS "MobileGL: gbm.h / gbm_backend_abi.h not found, mobilegl_gbm is not built")
    return()
endif()

add_library(mobilegl_gbm MODULE MobileGLGbm.cpp)
set_target_properties(mobilegl_gbm PROPERTIES
    PREFIX ""
    OUTPUT_NAME mobilegl_gbm
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(mobilegl_gbm PRIVATE
    ${MOBILEGL_GBM_INCLUDE_DIR}
    ${MOBILEGL_GBM_BACKEND_ABI_INCLUDE_DIR}
    # SharedImageApi.h only: the plugin shares a C header with the library, not its code.
    ${CMAKE_CURRENT_SOURCE_DIR}/../MG_Impl
)
target_link_libraries(mobilegl_gbm PRIVATE ${CMAKE_DL_LIBS})
# Loaded into compositors: an unresolved symbol must fail the link, not the dlopen.
target_link_options(mobilegl_gbm PRIVATE "LINKER:--no-undefined")
