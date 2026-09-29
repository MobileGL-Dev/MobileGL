// MobileGL - MobileGL/MG_Remote/Server/AdoptInbox.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 B2 (MG_Remote/CONTRACT-P11.md B2): the server's end of T0's descriptor channel.
//
// WHO READS THE FD, AND WHY IT CANNOT DEADLOCK THE APPLY THREAD. The APPLY THREAD reads it,
// inline, when it decodes the map_persistent record whose seq the Offer names - nothing else reads
// a session's aux socket after the handshake, so there is no second reader to race. The wait
// cannot deadlock because of the client's order: it queues the Offer (ShareFd returns once the
// kernel - or the in-process queue - holds it) BEFORE it publishes the record, so when the apply
// thread sees the record the descriptor is already there, and the wait depends on nothing the
// client has still to do. A client emits at most one T0 map_persistent at a time (the row blocks
// on its reply), so at most one Offer is ever outstanding and the aux queue (10 datagrams on an
// Android kernel) cannot fill. The wait is still bounded (AdoptT0::kOfferWaitMs): an Offer that
// never comes is a named DECLINED, never a hang.
//
// Offers are matched by seq, not by arrival order. One that arrives for a LATER record is kept
// for it; one for an EARLIER record (its record already timed out and was declined) is closed
// and counted as stale.

#pragma once

#include <Includes.h>

#include "../Transport/AdoptT0.h"
#include "../Transport/ITransport.h"

#include <map>
#include <string>

namespace MobileGL::MG_Remote::Server {

    class AdoptInbox {
    public:
        enum class Outcome : Uint8 { Taken, TimedOut, Closed };

        AdoptInbox() = default;
        ~AdoptInbox();
        AdoptInbox(const AdoptInbox&) = delete;
        AdoptInbox& operator=(const AdoptInbox&) = delete;

        void Attach(Transport::ITransport* transport) { m_transport = transport; }

        // The Offer for record `seq` and its hop descriptor (owned by the caller on Taken). Waits
        // at most `timeoutMs` for it; `why` names what went wrong otherwise.
        Outcome Take(Uint64 seq, Uint32 timeoutMs, int* outFd, Transport::AdoptT0::Offer* outOffer,
                     std::string& why);

        // Closes every kept descriptor (session teardown).
        void Clear();

        Uint64 StaleOffers() const { return m_stale; }
        Uint64 MalformedOffers() const { return m_malformed; }
        SizeT Kept() const { return m_kept.size(); }

    private:
        struct KeptOffer {
            int fd = -1;
            Transport::AdoptT0::Offer offer{};
        };
        Transport::ITransport* m_transport = nullptr;
        std::map<Uint64, KeptOffer> m_kept;
        Uint64 m_stale = 0;
        Uint64 m_malformed = 0;
    };

} // namespace MobileGL::MG_Remote::Server
