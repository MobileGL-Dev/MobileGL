// MobileGL - MobileGL/MG_Remote/Server/AdoptInbox.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "AdoptInbox.h"

#include "../Transport/FdPassing.h"

#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Util/Debug/Log.h>

#include <chrono>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Server {

    namespace {
        void CloseFd(int fd) {
#if !defined(_WIN32)
            if (fd >= 0) ::close(fd);
#else
            (void)fd;
#endif
        }
    } // namespace

    AdoptInbox::~AdoptInbox() { Clear(); }

    void AdoptInbox::Clear() {
        for (auto& [seq, kept] : m_kept) CloseFd(kept.fd);
        m_kept.clear();
    }

    AdoptInbox::Outcome AdoptInbox::Take(Uint64 seq, Uint32 timeoutMs, int* outFd,
                                         Transport::AdoptT0::Offer* outOffer, std::string& why) {
        return TakeKind(Kind::T0, seq, timeoutMs, outFd, outOffer, why);
    }

    AdoptInbox::Outcome AdoptInbox::TakeSharedImageFd(Uint64 seq, Uint32 timeoutMs, int* outFd, std::string& why) {
        Transport::AdoptT0::Offer unused{};
        return TakeKind(Kind::SharedImage, seq, timeoutMs, outFd, &unused, why);
    }

    // Both kinds are 32-byte sidebands keyed by the record's seq. A shared-image offer
    // (MGPSharedImageFdOffer) is carried in the T0 Offer's storage - only its magic and seq are read.
    AdoptInbox::Outcome AdoptInbox::TakeKind(Kind kind, Uint64 seq, Uint32 timeoutMs, int* outFd,
                                             Transport::AdoptT0::Offer* outOffer, std::string& why) {
        using Clock = std::chrono::steady_clock;
        static_assert(sizeof(MG_Pipe::MGPSharedImageFdOffer) == sizeof(Transport::AdoptT0::Offer),
                      "the two aux sidebands share one size");
        *outFd = -1;
        *outOffer = Transport::AdoptT0::Offer{};
        if (auto it = m_kept.find(KeyFor(kind, seq)); it != m_kept.end()) {
            *outFd = it->second.fd;
            *outOffer = it->second.offer;
            m_kept.erase(it);
            return Outcome::Taken;
        }
        if (m_transport == nullptr) {
            why = "the session has no control transport to receive the descriptor on";
            return Outcome::Closed;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const auto now = Clock::now();
            const Uint32 left = now >= deadline ? 0u
                : static_cast<Uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            int fd = -1;
            Uint8 sideband[Transport::FdPassing::kMaxSidebandBytes] = {};
            std::uint64_t sidebandSize = 0;
            const MobileGLResult received = m_transport->ReceiveFd(
                &fd, MobileGLMutableByteSpan{sideband, sizeof(sideband)}, &sidebandSize, left);
            if (received == MOBILEGL_ERR_TIMEOUT) {
                why = "no descriptor arrived on the aux socket within " + std::to_string(timeoutMs) +
                      " ms of its record";
                return Outcome::TimedOut;
            }
            if (received != MOBILEGL_OK) {
                why = "the aux socket closed (rc=" + std::to_string(static_cast<int>(received)) +
                      ") before the descriptor arrived";
                return Outcome::Closed;
            }
            Transport::AdoptT0::Offer offer{};
            if (sidebandSize != sizeof(offer)) {
                ++m_malformed;
                MGLOG_W("MG_Remote server: an aux descriptor with a %llu-byte sideband is neither a T0 store nor a "
                        "shared image; closed",
                        static_cast<unsigned long long>(sidebandSize));
                CloseFd(fd);
                continue;
            }
            std::memcpy(&offer, sideband, sizeof(offer));
            Kind arrived;
            if (offer.magic == Transport::AdoptT0::kOfferMagic && offer.version == Transport::AdoptT0::kOfferVersion) {
                arrived = Kind::T0;
            } else if (offer.magic == MG_Pipe::kMGPSharedImageFdMagic) {
                arrived = Kind::SharedImage;
            } else {
                ++m_malformed;
                MGLOG_W("MG_Remote server: an aux descriptor whose sideband is neither a T0 Offer nor a shared "
                        "image (magic 0x%08x version %u); closed",
                        offer.magic, offer.version);
                CloseFd(fd);
                continue;
            }
            if (arrived == kind && offer.seq == seq) {
                *outFd = fd;
                *outOffer = offer;
                return Outcome::Taken;
            }
            if (offer.seq < seq) {
                // Its record already timed out and was declined; this late copy is only a reference.
                ++m_stale;
                MGLOG_W("MG_Remote server: the descriptor for record %llu arrived after its record was "
                        "answered; closed (stale)",
                        static_cast<unsigned long long>(offer.seq));
                CloseFd(fd);
                continue;
            }
            // For a later record: keep it for that record. Replacing a kept one would leak it.
            const Uint64 key = KeyFor(arrived, offer.seq);
            if (auto it = m_kept.find(key); it != m_kept.end()) {
                ++m_malformed;
                CloseFd(it->second.fd);
                it->second = KeptOffer{fd, offer};
                continue;
            }
            m_kept.emplace(key, KeptOffer{fd, offer});
        }
    }

} // namespace MobileGL::MG_Remote::Server
