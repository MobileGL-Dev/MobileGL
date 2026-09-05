// MobileGL - MobileGL/MG_Remote/Transport/ShmSegment.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// One shared-memory segment: SEG_CMD, SEG_STAGE, SEG_REPLY, SEG_EVENT, a
// per-object SEG_SHADOW or a SEG_ADOPT store (inherited segment layout, plan
// section 8.1).
//
// Creation matrix (earlier plan section 6.1):
//   - Android:      ASharedMemory_create (API 26; libc's memfd_create wrapper
//                   only appears at API 30, which is above our floor)
//   - desktop Linux: syscall(SYS_memfd_create, ...) directly, for the same
//                   reason - the glibc wrapper is recent and this file has to
//                   build against old sysroots
//   - other POSIX:  shm_open + immediate shm_unlink, the fd keeps it alive
//   - Windows:      CreateFileMappingW in the Local\ namespace
//
// Transfer is NOT done here. On POSIX the fd travels by SCM_RIGHTS
// (FdPassing.h / ITransport::ShareFd) and the name is only a debugging label;
// on Windows the section name travels inside the SegmentRef table.
//
// The Windows implementation is compile-guarded and untested at the time it
// was written: no Windows machine is a correctness gate for this project.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <cstddef>
#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    inline constexpr std::size_t kShmNameMax = 128;

    class ShmSegment {
    public:
        ShmSegment() = default;
        ~ShmSegment();

        ShmSegment(const ShmSegment&) = delete;
        ShmSegment& operator=(const ShmSegment&) = delete;
        ShmSegment(ShmSegment&& other) noexcept;
        ShmSegment& operator=(ShmSegment&& other) noexcept;

        // Creates a segment of `size` bytes owned by this process. `nameHint`
        // is a short debug label (Windows: part of the section name peers
        // resolve). The segment is NOT mapped yet.
        static MobileGLResult Create(const char* nameHint, std::uint64_t size, ShmSegment& out);

        // POSIX only: adopts a descriptor received over SCM_RIGHTS. Takes
        // ownership of `fd` on success; on failure the caller still owns it.
        static MobileGLResult Adopt(int fd, std::uint64_t size, ShmSegment& out);

        // Windows only: opens a section the peer published by name.
        static MobileGLResult OpenNamed(const char* name, std::uint64_t size, ShmSegment& out);

        // Maps the whole segment. Read-only mappings are what the peer gets for
        // a segment it does not own (SEG_CMD/SEG_STAGE on the server side).
        MobileGLResult Map(bool readOnly);
        void Unmap();
        void Close(); // unmaps and releases the descriptor/handle

        bool Valid() const;
        void* Data() const { return m_mapping; }
        std::uint64_t Size() const { return m_size; }
        bool MappedReadOnly() const { return m_readOnly; }
        const char* Name() const { return m_name; }
        // POSIX: the descriptor to hand to ShareFd. -1 on Windows.
        int Fd() const { return m_fd; }

    private:
        void Steal(ShmSegment&& other) noexcept;

        char m_name[kShmNameMax] = {};
        void* m_mapping = nullptr;
        void* m_nativeHandle = nullptr; // Windows HANDLE; unused on POSIX
        std::uint64_t m_size = 0;
        int m_fd = -1;
        bool m_readOnly = false;
    };

} // namespace MobileGL::MG_Remote::Transport
