// MobileGL - MobileGL/MG_Remote/Transport/ShmSegmentPosix.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShmSegment.h"

#if !defined(_WIN32)

#include <MG_Util/Debug/Log.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/sharedmem.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

namespace MobileGL::MG_Remote::Transport {

    namespace {
        void CopyName(char (&dst)[kShmNameMax], const char* src) {
            if (src == nullptr) {
                dst[0] = '\0';
                return;
            }
            std::snprintf(dst, kShmNameMax, "%s", src);
        }

#if !defined(__ANDROID__)
        // Unique per process; only used by the shm_open fallback, whose name
        // must not collide with a concurrent creator's.
        std::atomic<std::uint32_t> g_shmCounter{0};
#endif
    } // namespace

    MobileGLResult ShmSegment::Create(const char* nameHint, std::uint64_t size, ShmSegment& out) {
        if (size == 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        out.Close();

        char label[kShmNameMax];
        std::snprintf(label, sizeof(label), "mgl-%s", nameHint != nullptr ? nameHint : "seg");

        int fd = -1;
#if defined(__ANDROID__)
        // API 26. libc's memfd_create wrapper is API 30, above MobileGL's floor.
        fd = ASharedMemory_create(label, static_cast<size_t>(size));
        if (fd < 0) {
            MGLOG_W("MG_Remote shm: ASharedMemory_create(%s, %llu) failed (errno=%d)", label,
                    static_cast<unsigned long long>(size), errno);
        }
#elif defined(__linux__)
        // Raw syscall, not the glibc wrapper: the wrapper is too recent to rely
        // on across the sysroots this builds against.
        fd = static_cast<int>(::syscall(SYS_memfd_create, label, MFD_CLOEXEC));
        if (fd >= 0 && ::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            MGLOG_E("MG_Remote shm: ftruncate(%llu) failed (errno=%d)",
                    static_cast<unsigned long long>(size), errno);
            ::close(fd);
            fd = -1;
        }
#endif

#if !defined(__ANDROID__)
        if (fd < 0) {
            // Fallback: shm_open + immediate unlink. The name disappears at
            // once; the descriptor is what keeps the object alive and what
            // travels by SCM_RIGHTS.
            char shmName[kShmNameMax];
            std::snprintf(shmName, sizeof(shmName), "/mgl-%d-%u-%s", static_cast<int>(::getpid()),
                          g_shmCounter.fetch_add(1, std::memory_order_relaxed),
                          nameHint != nullptr ? nameHint : "seg");
            fd = ::shm_open(shmName, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd < 0) {
                MGLOG_E("MG_Remote shm: shm_open(%s) failed (errno=%d)", shmName, errno);
                return MOBILEGL_ERR_SHM_EXHAUSTED;
            }
            ::shm_unlink(shmName);
            if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
                MGLOG_E("MG_Remote shm: ftruncate(%llu) failed (errno=%d)",
                        static_cast<unsigned long long>(size), errno);
                ::close(fd);
                return MOBILEGL_ERR_SHM_EXHAUSTED;
            }
            CopyName(out.m_name, shmName);
        } else {
            CopyName(out.m_name, label);
        }
#else
        if (fd < 0) {
            return MOBILEGL_ERR_SHM_EXHAUSTED;
        }
        CopyName(out.m_name, label);
#endif

        out.m_fd = fd;
        out.m_size = size;
        out.m_nativeHandle = nullptr;
        out.m_mapping = nullptr;
        out.m_readOnly = false;
        return MOBILEGL_OK;
    }

    MobileGLResult ShmSegment::Adopt(int fd, std::uint64_t size, ShmSegment& out) {
        if (fd < 0 || size == 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // The peer's declared size is not trusted: a segment smaller than what
        // the announcement claims would turn every later offset into an
        // out-of-bounds map.
        struct stat st{};
        if (::fstat(fd, &st) == 0 && st.st_size > 0 &&
            static_cast<std::uint64_t>(st.st_size) < size) {
            MGLOG_E("MG_Remote shm: peer announced %llu bytes but the descriptor is %lld",
                    static_cast<unsigned long long>(size), static_cast<long long>(st.st_size));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        out.Close();
        out.m_fd = fd; // ownership transferred
        out.m_size = size;
        out.m_nativeHandle = nullptr;
        out.m_mapping = nullptr;
        out.m_readOnly = false;
        CopyName(out.m_name, "adopted");
        return MOBILEGL_OK;
    }

    MobileGLResult ShmSegment::OpenNamed(const char*, std::uint64_t, ShmSegment&) {
        // POSIX shares descriptors, not names.
        return MOBILEGL_ERR_UNSUPPORTED;
    }

    MobileGLResult ShmSegment::Map(bool readOnly) {
        if (m_fd < 0 || m_size == 0) {
            return MOBILEGL_ERR_NOT_INITIALIZED;
        }
        if (m_mapping != nullptr) {
            if (m_readOnly == readOnly) {
                return MOBILEGL_OK;
            }
            Unmap();
        }
        const int prot = readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
        void* addr = ::mmap(nullptr, static_cast<size_t>(m_size), prot, MAP_SHARED, m_fd, 0);
        if (addr == MAP_FAILED) {
            MGLOG_E("MG_Remote shm: mmap of %llu bytes failed (errno=%d)",
                    static_cast<unsigned long long>(m_size), errno);
            return MOBILEGL_ERR_OUT_OF_MEMORY;
        }
        m_mapping = addr;
        m_readOnly = readOnly;
        return MOBILEGL_OK;
    }

    void ShmSegment::Unmap() {
        if (m_mapping != nullptr) {
            ::munmap(m_mapping, static_cast<size_t>(m_size));
            m_mapping = nullptr;
        }
    }

    void ShmSegment::Close() {
        Unmap();
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_size = 0;
        m_readOnly = false;
        m_name[0] = '\0';
    }

} // namespace MobileGL::MG_Remote::Transport

#endif // !_WIN32
