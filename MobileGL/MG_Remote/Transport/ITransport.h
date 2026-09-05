// MobileGL - MobileGL/MG_Remote/Transport/ITransport.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The control-plane transport interface.
//
// It is deliberately dumb: complete messages in, complete messages out, plus
// the one thing shared memory cannot do without help - handing a file
// descriptor to the peer. No session routing, no seq accounting, no
// serialization; those live above, in the protocol layer.
//
// Everything on the hot path bypasses this interface entirely: records go into
// the SEG_CMD ring (Ring.h) and the peer is woken through a Doorbell
// (Doorbell.h). ITransport carries the handshake, surface ops, resync, aux
// requests and fatals - the rare, variable-length, must-evolve traffic that
// plan section 7.1 assigns to FlatBuffers tables.
//
// This header stays dependency-light on purpose (mg_protocol_base.h plus the
// standard library): it is included by both roles and by the eventual
// server-side binary, and nothing about a byte pipe needs the GL frontend's
// umbrella header.
//
// Threading: one instance is not internally synchronized for send; callers
// serialize sends. ReceiveFrame/ReceiveFd may be called from one dedicated
// reader thread concurrently with sends from another.

#pragma once

#include "../Protocol/mg_protocol_base.h"

#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    // Which end of the connection this instance is.
    enum class TransportRole : std::uint32_t {
        Server = 1,    // accepts the client connection
        Client = 2,    // connects to the server endpoint
        InProcess = 3, // same-process hand-off (CI / inproc delivery mode)
    };

    class ITransport {
    public:
        virtual ~ITransport() = default;

        ITransport(const ITransport&) = delete;
        ITransport& operator=(const ITransport&) = delete;

        // ---- control plane -------------------------------------------------

        // Sends one complete message. `bytes` is borrowed: the implementation
        // either copies it or completes the underlying write before returning.
        // A payload larger than Framing::kMaxFramePayloadSize is rejected with
        // MOBILEGL_ERR_INVALID_ARGUMENT - bulk bytes belong in shm, never here.
        virtual MobileGLResult SendFrame(MobileGLByteSpan bytes) = 0;

        // Receives the next complete message.
        //
        //   MOBILEGL_OK                   - copied into `buffer`, *outSize is
        //                                   the message size, message consumed.
        //   MOBILEGL_ERR_BUFFER_TOO_SMALL - `buffer` is too small. *outSize is
        //                                   the size required and THE MESSAGE
        //                                   STAYS QUEUED: call again with a
        //                                   buffer of at least that size and it
        //                                   is still there.
        //   MOBILEGL_ERR_TIMEOUT          - nothing arrived within timeoutMs
        //                                   (0 = non-blocking poll).
        //   MOBILEGL_ERR_TRANSPORT_CLOSED - peer gone, nothing left buffered.
        //   MOBILEGL_ERR_PROTOCOL_MISMATCH- framing violated; the transport is
        //                                   latched failed and never recovers.
        //
        // The buffer-too-small half of that contract is the whole point of
        // having one: the earlier branch's transport failed the call AND
        // dropped the message, which wedges the stream permanently the first
        // time a message is bigger than the reader's guess.
        virtual MobileGLResult ReceiveFrame(MobileGLMutableByteSpan buffer, std::uint64_t* outSize,
                                            std::uint32_t timeoutMs) = 0;

        // Size of the next pending message, or 0 when none is buffered. Lets a
        // caller size its buffer without a failed receive first.
        virtual std::uint64_t PeekFrameSize() = 0;

        // ---- descriptor passing --------------------------------------------

        // Hands `fd` to the peer. POSIX: SCM_RIGHTS over the aux socket (see
        // FdPassing.h). Windows: not applicable, returns
        // MOBILEGL_ERR_UNSUPPORTED - the section name travels inside SegmentRef
        // instead. The caller keeps ownership of `fd` and closes it itself.
        //
        // This is a first-class member of the interface, not a later phase: the
        // earlier branch deferred it and hardcoded `out->fd = -1` in its offer
        // poll, so its data plane could not move a single byte on the only
        // platform that matters.
        virtual MobileGLResult ShareFd(int fd, MobileGLByteSpan sideband) = 0;

        // Receives one fd previously shared by the peer. On success *outFd owns
        // a descriptor this process must close. `sideband` receives the bytes
        // that travelled with it (may be empty) and must be at least
        // FdPassing::kMaxSidebandBytes: an fd offer is one datagram and cannot
        // be half-consumed, so the capacity is checked BEFORE anything is read
        // and a short buffer returns MOBILEGL_ERR_BUFFER_TOO_SMALL with the
        // required size, having consumed nothing and dropped no descriptor.
        virtual MobileGLResult ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband,
                                         std::uint64_t* outSidebandSize, std::uint32_t timeoutMs) = 0;

        // ---- lifecycle ------------------------------------------------------

        // Idempotent. Tears down the WHOLE connection, not just this end:
        // both directions are half-closed, so after either endpoint calls it
        // neither side can send any more (SendFrame returns
        // MOBILEGL_ERR_TRANSPORT_CLOSED) and every waiter on either side is
        // unblocked. That is what closing a socket does, and the spawn
        // transport behaves the same way, so a one-sided contract here would
        // be a promise only the in-process implementation could keep.
        //
        // Messages already queued stay readable until drained: a peer that
        // shuts down right after sending does not lose its last message.
        virtual void Shutdown() = 0;

        virtual TransportRole Role() const = 0;

    protected:
        ITransport() = default;
    };

} // namespace MobileGL::MG_Remote::Transport
