// MobileGL - MobileGL/MG_Remote/Transport/FdPassing.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// SCM_RIGHTS descriptor passing over an AF_UNIX socket pair (POSIX only); without it the shared
// segments cannot cross the process boundary. The channel is a dedicated SOCK_DGRAM socketpair,
// not the control byte stream: datagrams keep message boundaries on every POSIX (macOS has no
// SOCK_SEQPACKET), so one sendmsg is one recvmsg and ancillary data stays with its payload; on a
// byte stream it would bind to whichever byte is at the front of the reader's buffer, which the
// frame reassembler cannot manage.

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
    //
    // `dontWait` sends with MSG_DONTWAIT: a receive queue the peer is not draining
    // (net.unix.max_dgram_qlen datagrams, 10 on an Android kernel) answers
    // MOBILEGL_ERR_TIMEOUT at once instead of blocking the sender until the peer
    // reads. A peer that has closed its end answers MOBILEGL_ERR_TRANSPORT_CLOSED
    // either way. The TCP supervisor's hand-off (ServerMain.cpp RouteWhileBusy)
    // sends this way because the session child stops reading the hand-off once its
    // data connection is bound, and a supervisor blocked in sendmsg serves nobody.
    MobileGLResult SendFd(int socket, int fd, MobileGLByteSpan sideband, bool dontWait = false);

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
