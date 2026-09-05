// MobileGL - MobileGL/MG_Remote/Transport/InProcessTransport.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "InProcessTransport.h"

#include "FdPassing.h"
#include "Framing.h"

#include <MG_Util/Debug/Log.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Transport {

    namespace {
        struct FdOffer {
            int fd = -1;
            std::vector<std::uint8_t> sideband;
        };
    } // namespace

    // One direction of the channel: everything queued FOR one endpoint.
    class InProcessChannel {
    public:
        struct Direction {
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<std::vector<std::uint8_t>> messages;
            std::deque<FdOffer> fdOffers;
            bool closed = false;
        };

        ~InProcessChannel() {
            for (Direction& dir : m_directions) {
                for (FdOffer& offer : dir.fdOffers) {
#if !defined(_WIN32)
                    if (offer.fd >= 0) {
                        ::close(offer.fd);
                    }
#endif
                }
                dir.fdOffers.clear();
            }
        }

        Direction& Inbox(int endpoint) { return m_directions[endpoint]; }
        Direction& Outbox(int endpoint) { return m_directions[1 - endpoint]; }
        CondVarDoorbell& Bell(int endpoint) { return m_bells[endpoint]; }

        void Close() {
            for (Direction& dir : m_directions) {
                {
                    std::lock_guard<std::mutex> lock(dir.mutex);
                    dir.closed = true;
                }
                dir.cv.notify_all();
            }
            // Anything parked on a ring doorbell has to come back too, or a
            // shutdown mid-frame hangs the peer forever.
            for (CondVarDoorbell& bell : m_bells) {
                bell.Notify();
            }
        }

    private:
        Direction m_directions[2];
        CondVarDoorbell m_bells[2];
    };

    InProcessTransport::InProcessTransport(std::shared_ptr<InProcessChannel> channel, int endpoint)
        : m_channel(std::move(channel)), m_endpoint(endpoint) {}

    InProcessTransport::~InProcessTransport() = default;

    void InProcessTransport::CreatePair(std::unique_ptr<InProcessTransport>& outClient,
                                        std::unique_ptr<InProcessTransport>& outServer) {
        auto channel = std::make_shared<InProcessChannel>();
        outClient.reset(new InProcessTransport(channel, 0));
        outServer.reset(new InProcessTransport(channel, 1));
    }

    MobileGLResult InProcessTransport::SendFrame(MobileGLByteSpan bytes) {
        if (bytes.size != 0 && bytes.data == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // Same cap as the byte-stream transports, so nothing legal here becomes
        // illegal the day the delivery mode changes to `spawn`.
        if (bytes.size > kMaxFramePayloadSize) {
            MGLOG_E("MG_Remote inproc: refusing a %llu byte message (cap %llu)",
                    static_cast<unsigned long long>(bytes.size),
                    static_cast<unsigned long long>(kMaxFramePayloadSize));
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        InProcessChannel::Direction& dir = m_channel->Outbox(m_endpoint);
        {
            std::lock_guard<std::mutex> lock(dir.mutex);
            if (dir.closed) {
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            const auto* first = static_cast<const std::uint8_t*>(bytes.data);
            dir.messages.emplace_back(first, first + bytes.size);
        }
        dir.cv.notify_one();
        return MOBILEGL_OK;
    }

    MobileGLResult InProcessTransport::ReceiveFrame(MobileGLMutableByteSpan buffer,
                                                    std::uint64_t* outSize,
                                                    std::uint32_t timeoutMs) {
        if (outSize != nullptr) {
            *outSize = 0;
        }
        InProcessChannel::Direction& dir = m_channel->Inbox(m_endpoint);
        std::unique_lock<std::mutex> lock(dir.mutex);
        if (dir.messages.empty() && !dir.closed && timeoutMs != 0) {
            const auto ready = [&dir] { return !dir.messages.empty() || dir.closed; };
            if (timeoutMs == kWaitForever) {
                dir.cv.wait(lock, ready);
            } else {
                dir.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
            }
        }
        if (dir.messages.empty()) {
            // Queued messages outlive the peer's Shutdown; only an empty inbox
            // is a closed one.
            return dir.closed ? MOBILEGL_ERR_TRANSPORT_CLOSED : MOBILEGL_ERR_TIMEOUT;
        }

        const std::vector<std::uint8_t>& front = dir.messages.front();
        const std::uint64_t size = front.size();
        if (outSize != nullptr) {
            *outSize = size;
        }
        if (buffer.size < size) {
            // Contract: the message STAYS QUEUED. The earlier branch's
            // transport failed the call and popped the message anyway, which
            // wedges the stream permanently the first time a reader guesses the
            // size wrong.
            return MOBILEGL_ERR_BUFFER_TOO_SMALL;
        }
        if (size != 0) {
            if (buffer.data == nullptr) {
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            std::memcpy(buffer.data, front.data(), static_cast<std::size_t>(size));
        }
        dir.messages.pop_front();
        return MOBILEGL_OK;
    }

    std::uint64_t InProcessTransport::PeekFrameSize() {
        InProcessChannel::Direction& dir = m_channel->Inbox(m_endpoint);
        std::lock_guard<std::mutex> lock(dir.mutex);
        return dir.messages.empty() ? 0 : dir.messages.front().size();
    }

    MobileGLResult InProcessTransport::ShareFd(int fd, MobileGLByteSpan sideband) {
#if defined(_WIN32)
        (void)fd;
        (void)sideband;
        return MOBILEGL_ERR_UNSUPPORTED;
#else
        if (fd < 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (sideband.size > FdPassing::kMaxSidebandBytes ||
            (sideband.size != 0 && sideband.data == nullptr)) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // Same ownership rule as SCM_RIGHTS: the peer gets its own descriptor
        // for the same open file description and the caller keeps its own.
        const int duplicate = ::dup(fd);
        if (duplicate < 0) {
            MGLOG_E("MG_Remote inproc: dup failed (errno=%d)", errno);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        FdOffer offer;
        offer.fd = duplicate;
        if (sideband.size != 0) {
            const auto* first = static_cast<const std::uint8_t*>(sideband.data);
            offer.sideband.assign(first, first + sideband.size);
        }

        InProcessChannel::Direction& dir = m_channel->Outbox(m_endpoint);
        {
            std::lock_guard<std::mutex> lock(dir.mutex);
            if (dir.closed) {
                ::close(duplicate);
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            dir.fdOffers.push_back(std::move(offer));
        }
        dir.cv.notify_one();
        return MOBILEGL_OK;
#endif
    }

    MobileGLResult InProcessTransport::ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband,
                                                 std::uint64_t* outSidebandSize,
                                                 std::uint32_t timeoutMs) {
        if (outFd == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *outFd = -1;
        if (outSidebandSize != nullptr) {
            *outSidebandSize = 0;
        }
#if defined(_WIN32)
        (void)sideband;
        (void)timeoutMs;
        return MOBILEGL_ERR_UNSUPPORTED;
#else
        // Symmetric with FdPassing::ReceiveFd so callers behave identically in
        // both delivery modes.
        if (sideband.size < FdPassing::kMaxSidebandBytes) {
            if (outSidebandSize != nullptr) {
                *outSidebandSize = FdPassing::kMaxSidebandBytes;
            }
            return MOBILEGL_ERR_BUFFER_TOO_SMALL;
        }
        if (sideband.data == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        InProcessChannel::Direction& dir = m_channel->Inbox(m_endpoint);
        std::unique_lock<std::mutex> lock(dir.mutex);
        if (dir.fdOffers.empty() && !dir.closed && timeoutMs != 0) {
            const auto ready = [&dir] { return !dir.fdOffers.empty() || dir.closed; };
            if (timeoutMs == kWaitForever) {
                dir.cv.wait(lock, ready);
            } else {
                dir.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
            }
        }
        if (dir.fdOffers.empty()) {
            return dir.closed ? MOBILEGL_ERR_TRANSPORT_CLOSED : MOBILEGL_ERR_TIMEOUT;
        }

        FdOffer offer = std::move(dir.fdOffers.front());
        dir.fdOffers.pop_front();
        if (!offer.sideband.empty()) {
            std::memcpy(sideband.data, offer.sideband.data(), offer.sideband.size());
        }
        if (outSidebandSize != nullptr) {
            *outSidebandSize = offer.sideband.size();
        }
        *outFd = offer.fd;
        return MOBILEGL_OK;
#endif
    }

    void InProcessTransport::Shutdown() { m_channel->Close(); }

    Doorbell& InProcessTransport::PeerDoorbell() { return m_channel->Bell(1 - m_endpoint); }

    Doorbell& InProcessTransport::SelfDoorbell() { return m_channel->Bell(m_endpoint); }

} // namespace MobileGL::MG_Remote::Transport
