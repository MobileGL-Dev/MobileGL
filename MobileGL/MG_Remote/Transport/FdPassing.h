// MobileGL - MobileGL/MG_Remote/Transport/FdPassing.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SCM_RIGHTS descriptor passing over an AF_UNIX socket pair. POSIX only.
//
// This is the FIRST transport commit, deliberately (inherited design, plan
// section 8.1, "SCM_RIGHTS must be implemented in the first transport
// commit"). The earlier branch pushed it to a later phase and hardcoded
// `out->fd = -1` in its offer poll, so on the only platform that matters its
// data plane could never move a byte: every segment announcement resolved to
// "no descriptor". A transport whose shm cannot cross the process boundary is
// not a transport.
//
// Channel shape: a dedicated AF_UNIX SOCK_DGRAM socketpair, NOT the control
// byte stream. Two reasons:
//   - SOCK_DGRAM preserves message boundaries on every POSIX (SOCK_SEQPACKET
//     does not exist on macOS), so one sendmsg is exactly one recvmsg and the
//     ancillary data can never be split away from its payload;
//   - ancillary data attached to a byte stream binds to whichever ordinary
//     byte happens to be at the front of the reader's buffer, which is
//     unmanageable once frames are being reassembled.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <cstdint>

namespace MobileGL::MG_Remote::Transport::FdPassing {

    // Upper bound for the bytes that travel with a descriptor (a SegmentRef
    // sized announcement, not payload).
    inline constexpr std::uint64_t kMaxSidebandBytes = 256;

    // False on platforms without SCM_RIGHTS (Windows).
    bool Supported();

    // Creates the aux socket pair. Both descriptors are CLOEXEC and owned by
    // the caller. outFds[0] is conventionally the client end, [1] the server's
    // (the one that is inherited or passed to the spawned process).
    MobileGLResult CreateSocketPair(int outFds[2]);

    // Sends `fd` with `sideband` attached. The caller keeps ownership of `fd`
    // (the peer gets its own descriptor for the same open file description).
    // sideband.size must be <= kMaxSidebandBytes.
    MobileGLResult SendFd(int socket, int fd, MobileGLByteSpan sideband);

    // Receives one descriptor and its sideband bytes.
    //
    // `sideband` must be at least kMaxSidebandBytes: a datagram cannot be
    // partially consumed, so the capacity is checked BEFORE anything is read.
    // A short buffer returns MOBILEGL_ERR_BUFFER_TOO_SMALL with
    // *outSidebandSize = kMaxSidebandBytes and consumes nothing, so no
    // descriptor is ever dropped on the floor.
    //
    // On success *outFd owns a descriptor this process must close.
    // MOBILEGL_ERR_TIMEOUT when nothing arrived (timeoutMs 0 = poll),
    // MOBILEGL_ERR_TRANSPORT_CLOSED on peer close.
    MobileGLResult ReceiveFd(int socket, int* outFd, MobileGLMutableByteSpan sideband,
                             std::uint64_t* outSidebandSize, std::uint32_t timeoutMs);

} // namespace MobileGL::MG_Remote::Transport::FdPassing
