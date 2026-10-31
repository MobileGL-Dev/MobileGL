# MobileGL - tools/anland-mobilegl-plasma/scripts/xsysroot/aarch64-arch.cmake
# Copyright (c) 2025-2026 MobileGL-Dev
# Licensed under the GNU Lesser General Public License v3.0:
#   https://www.gnu.org/licenses/gpl-3.0.txt
#   https://www.gnu.org/licenses/lgpl-3.0.txt
# SPDX-License-Identifier: LGPL-3.0-only
# End of Source File Header

# Cross toolchain: WSL clang -> aarch64 Arch Linux ARM (glibc) container "arch-kde-mgl".
# Sysroot exported from the container by refresh-sysroot.sh (default $HOME/sysroots/arch-kde-mgl).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_triple aarch64-unknown-linux-gnu)
if (DEFINED ENV{MGL_SYSROOT})
    set(_sysroot "$ENV{MGL_SYSROOT}")
else()
    set(_sysroot "$ENV{HOME}/sysroots/arch-kde-mgl")
endif()
# The container's GCC install dir (crtbegin/crtend, libgcc, libstdc++ via /usr/include/c++/<ver>).
file(GLOB _gcc_dirs LIST_DIRECTORIES true "${_sysroot}/usr/lib/gcc/${_triple}/*")
list(SORT _gcc_dirs COMPARE NATURAL ORDER DESCENDING)
list(GET _gcc_dirs 0 _gcc_install_dir)

set(CMAKE_SYSROOT "${_sysroot}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_triple})
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_NM llvm-nm)
set(CMAKE_OBJCOPY llvm-objcopy)
set(CMAKE_STRIP llvm-strip)

set(_xflags "--gcc-install-dir=${_gcc_install_dir}")
set(CMAKE_C_FLAGS_INIT "${_xflags}")
set(CMAKE_CXX_FLAGS_INIT "${_xflags}")
set(CMAKE_ASM_FLAGS_INIT "${_xflags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_LINKER_TYPE LLD)

set(CMAKE_FIND_ROOT_PATH "${_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_sysroot}")
set(ENV{PKG_CONFIG_LIBDIR} "${_sysroot}/usr/lib/pkgconfig:${_sysroot}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
