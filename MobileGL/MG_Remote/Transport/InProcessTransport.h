// MobileGL - MobileGL/MG_Remote/Transport/InProcessTransport.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The `inproc` transport: two in-memory message queues and a pair of condvar
// doorbells, one connected endpoint at each end.
//
// It is not a test double. `inproc` is a delivery mode of its own (CMake
// option MOBILEGL_BUILD_DISAGGREGATED_INPROC): the server side is the
// monolith's own render thread, which is the single largest CPU lever this
// project has, and it is also the CI form of the split build. What it does NOT
// exercise is serialization of the byte stream, so the framing codec is
// covered separately by FramingTest.
//
// Messages are queued whole, so no framing bytes are involved; the size cap is
// still enforced so that a payload which would be illegal on a socket is
// illegal here too and does not pass CI only to fail after the switch to
// `spawn`.
//
// Descriptor passing is a plain dup(): both ends are the same process, so
// there is nothing to transfer, but the API stays identical so callers can be
// written once.

#pragma once

#include "Doorbell.h"
#include "ITransport.h"

#include <memory>

namespace MobileGL::MG_Remote::Transport {

    class InProcessChannel;

    class InProcessTransport final : public ITransport {
    public:
        ~InProcessTransport() override;

        // Creates one connected pair. Endpoint 0 is the client, endpoint 1 the
        // server; both share one channel and either may be destroyed first.
        static void CreatePair(std::unique_ptr<InProcessTransport>& outClient,
                               std::unique_ptr<InProcessTransport>& outServer);

        MobileGLResult SendFrame(MobileGLByteSpan bytes) override;
        MobileGLResult ReceiveFrame(MobileGLMutableByteSpan buffer, std::uint64_t* outSize,
                                    std::uint32_t timeoutMs) override;
        std::uint64_t PeekFrameSize() override;
        MobileGLResult ShareFd(int fd, MobileGLByteSpan sideband) override;
        MobileGLResult ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband,
                                 std::uint64_t* outSidebandSize, std::uint32_t timeoutMs) override;
        void Shutdown() override;
        TransportRole Role() const override { return TransportRole::InProcess; }

        // The wake channel for the SEG_CMD/SEG_STAGE rings living beside this
        // transport: ring the peer's bell after publishing a watermark (only
        // when its park flag is set - see NotifyIfParked), park on your own.
        Doorbell& PeerDoorbell();
        Doorbell& SelfDoorbell();

    private:
        InProcessTransport(std::shared_ptr<InProcessChannel> channel, int endpoint);

        std::shared_ptr<InProcessChannel> m_channel;
        int m_endpoint = 0;
    };

} // namespace MobileGL::MG_Remote::Transport
