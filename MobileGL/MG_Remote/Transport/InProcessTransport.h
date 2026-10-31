// MobileGL - MobileGL/MG_Remote/Transport/InProcessTransport.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The `inproc` transport: two in-memory message queues and a pair of condvar doorbells, one
// connected endpoint at each end. Not a test double: inproc is a delivery mode of its own and
// the CI form of the split build. It carries only the control plane; records use the same
// shared-segment rings as spawn. Messages are queued whole, so framing is not exercised here
// (FramingTest covers it), but the size cap is enforced so a payload illegal on a socket fails
// here too. Descriptor passing is a plain dup(), since both ends are one process, behind the
// same API as the socket transport. Selected at run time by MOBILEGL_TRANSPORT=inproc.

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
        MobileGLResult ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband, std::uint64_t* outSidebandSize,
                                 std::uint32_t timeoutMs) override;
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
