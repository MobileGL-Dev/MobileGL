// MobileGL - MobileGL/MG_Remote/Transport/ShmSegmentWin32.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Windows half of ShmSegment: a named file-mapping section in the Local\
// namespace, which the peer opens by the name carried in SegmentRef.
//
// UNTESTED. This project's Windows machine is not a correctness gate (its
// Vulkan lacks vkCreateHeadlessSurfaceEXT and accounts for most of its
// baseline integration failures), and the whole disaggregated build is gated
// behind MOBILEGL_BUILD_DISAGGREGATED, which is OFF by default. It is written
// now so the abstraction is shaped by two real platforms rather than one.

#include "ShmSegment.h"

#if defined(_WIN32)

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace MobileGL::MG_Remote::Transport {

    namespace {
        std::atomic<std::uint32_t> g_sectionCounter{0};

        bool ToWide(const char* utf8, wchar_t* out, int outChars) {
            if (utf8 == nullptr || out == nullptr || outChars <= 0) {
                return false;
            }
            const int written = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, outChars);
            return written > 0;
        }
    } // namespace

    MobileGLResult ShmSegment::Create(const char* nameHint, std::uint64_t size, ShmSegment& out) {
        if (size == 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        out.Close();

        char name[kShmNameMax];
        std::snprintf(name, sizeof(name), "Local\\mgl-%lu-%u-%s",
                      static_cast<unsigned long>(::GetCurrentProcessId()),
                      g_sectionCounter.fetch_add(1, std::memory_order_relaxed),
                      nameHint != nullptr ? nameHint : "seg");

        wchar_t wide[kShmNameMax];
        if (!ToWide(name, wide, static_cast<int>(kShmNameMax))) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        HANDLE section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                              static_cast<DWORD>(size >> 32),
                                              static_cast<DWORD>(size & 0xFFFFFFFFull), wide);
        if (section == nullptr) {
            MGLOG_E("MG_Remote shm: CreateFileMappingW(%s, %llu) failed (GetLastError=%lu)", name,
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long>(::GetLastError()));
            return MOBILEGL_ERR_SHM_EXHAUSTED;
        }
        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
            ::CloseHandle(section);
            MGLOG_E("MG_Remote shm: section name %s already exists", name);
            return MOBILEGL_ERR_SHM_EXHAUSTED;
        }

        std::snprintf(out.m_name, kShmNameMax, "%s", name);
        out.m_nativeHandle = section;
        out.m_size = size;
        out.m_fd = -1;
        out.m_mapping = nullptr;
        out.m_readOnly = false;
        return MOBILEGL_OK;
    }

    MobileGLResult ShmSegment::Adopt(int, std::uint64_t, ShmSegment&) {
        // No SCM_RIGHTS here: Windows peers resolve the section by name.
        return MOBILEGL_ERR_UNSUPPORTED;
    }

    MobileGLResult ShmSegment::OpenNamed(const char* name, std::uint64_t size, ShmSegment& out) {
        if (name == nullptr || name[0] == '\0' || size == 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        out.Close();

        wchar_t wide[kShmNameMax];
        if (!ToWide(name, wide, static_cast<int>(kShmNameMax))) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        HANDLE section = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide);
        if (section == nullptr) {
            MGLOG_E("MG_Remote shm: OpenFileMappingW(%s) failed (GetLastError=%lu)", name,
                    static_cast<unsigned long>(::GetLastError()));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        std::snprintf(out.m_name, kShmNameMax, "%s", name);
        out.m_nativeHandle = section;
        out.m_size = size;
        out.m_fd = -1;
        out.m_mapping = nullptr;
        out.m_readOnly = false;
        return MOBILEGL_OK;
    }

    MobileGLResult ShmSegment::Map(bool readOnly) {
        if (m_nativeHandle == nullptr || m_size == 0) {
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        if (m_mapping != nullptr) {
            if (m_readOnly == readOnly) {
                return MOBILEGL_OK;
            }
            Unmap();
        }
        void* view = ::MapViewOfFile(static_cast<HANDLE>(m_nativeHandle),
                                     readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS, 0, 0,
                                     static_cast<SIZE_T>(m_size));
        if (view == nullptr) {
            MGLOG_E("MG_Remote shm: MapViewOfFile of %llu bytes failed (GetLastError=%lu)",
                    static_cast<unsigned long long>(m_size),
                    static_cast<unsigned long>(::GetLastError()));
            return MOBILEGL_ERR_OUT_OF_MEMORY;
        }
        m_mapping = view;
        m_readOnly = readOnly;
        return MOBILEGL_OK;
    }

    void ShmSegment::Unmap() {
        if (m_mapping != nullptr) {
            ::UnmapViewOfFile(m_mapping);
            m_mapping = nullptr;
        }
    }

    void ShmSegment::Close() {
        Unmap();
        if (m_nativeHandle != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(m_nativeHandle));
            m_nativeHandle = nullptr;
        }
        m_size = 0;
        m_readOnly = false;
        m_name[0] = '\0';
    }

} // namespace MobileGL::MG_Remote::Transport

#endif // _WIN32
