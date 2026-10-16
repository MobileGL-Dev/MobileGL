// MobileGL - MobileGL/MG_Remote/Server/AdoptInbox.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server's end of the aux-socket descriptor channel: T0 map_persistent Offers and shared-image
// descriptors, matched by record seq and told apart by their sideband's magic. Only the apply
// thread reads it, inline, when it decodes the record the descriptor belongs to. It cannot
// deadlock: the client queues the descriptor before it publishes the record, and a T0
// map_persistent blocks on its reply, so at most one Offer is outstanding (an Android aux queue
// holds 10). The wait is still bounded (AdoptT0::kOfferWaitMs); a missing Offer is a named
// DECLINED. A descriptor for a later record is kept for it; one for an earlier, already declined
// record is closed and counted as stale.

#pragma once

#include <Includes.h>

#include "../Transport/AdoptT0.h"
#include "../Transport/ITransport.h"

#include <map>
#include <string>

namespace MobileGL::MG_Remote::Server {

    class AdoptInbox {
    public:
        enum class Outcome : Uint8 {
            Taken,
            TimedOut,
            Closed
        };

        AdoptInbox() = default;
        ~AdoptInbox();
        AdoptInbox(const AdoptInbox&) = delete;
        AdoptInbox& operator=(const AdoptInbox&) = delete;

        void Attach(Transport::ITransport* transport) { m_transport = transport; }

        // The Offer for record `seq` and its hop descriptor (owned by the caller on Taken). Waits
        // at most `timeoutMs` for it; `why` names what went wrong otherwise.
        Outcome Take(Uint64 seq, Uint32 timeoutMs, int* outFd, Transport::AdoptT0::Offer* outOffer, std::string& why);

        // A shared image's descriptor for record `seq` (an Import; sideband MGPSharedImageFdOffer).
        // Same matching and the same bound as Take; the two kinds share the aux socket and are kept
        // apart by their sideband's magic.
        Outcome TakeSharedImageFd(Uint64 seq, Uint32 timeoutMs, int* outFd, std::string& why);

        // Closes every kept descriptor (session teardown).
        void Clear();

        Uint64 StaleOffers() const { return m_stale; }
        Uint64 MalformedOffers() const { return m_malformed; }
        SizeT Kept() const { return m_kept.size(); }

    private:
        enum class Kind : Uint8 {
            T0,
            SharedImage
        };
        struct KeptOffer {
            int fd = -1;
            Transport::AdoptT0::Offer offer{};
        };
        static Uint64 KeyFor(Kind kind, Uint64 seq) { return (seq << 1) | (kind == Kind::SharedImage ? 1u : 0u); }
        Outcome TakeKind(Kind kind, Uint64 seq, Uint32 timeoutMs, int* outFd, Transport::AdoptT0::Offer* outOffer,
                         std::string& why);
        Transport::ITransport* m_transport = nullptr;
        std::map<Uint64, KeptOffer> m_kept;
        Uint64 m_stale = 0;
        Uint64 m_malformed = 0;
    };

} // namespace MobileGL::MG_Remote::Server
