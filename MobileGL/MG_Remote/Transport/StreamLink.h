// MobileGL - MobileGL/MG_Remote/Transport/StreamLink.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// ILink's second implementation, DECLARED BY c6 AND IMPLEMENTED BY NOBODY.
// Authority: CONTRACT-P6.md §8.4. Its phase is P6.5.
//
// WHY A HEADER FOR SOMETHING THAT DOES NOT EXIST. ILink's adequacy - "can this
// seam name what a byte stream needs?" - is the one claim c6 makes that an
// argument cannot settle and a compile can. If ILink cannot express progress
// transmission, a send window, a flush, span resolution without a mapping, and
// replies that are messages rather than slots, then StreamLink cannot be
// declared against it, and the build says so. That is c6's red-once: delete
// one ILink method this class declares and the build goes red.
//
// It has already earned its keep. Writing it forced THREE methods that the
// original brief's list - progress transmission, send window, flush - did not
// name:
//
//   ResolveSpan    - a stream has no mapping to do pointer arithmetic over, so
//                    the eleven ResolveOrFatal sites in the decoder need the
//                    link to answer, not a segment table. Without it the seam
//                    was unimplementable for the case it exists for.
//   FlushProgress  - separate from Flush because the triggers differ: Flush is
//                    about records the peer has not seen, FlushProgress is
//                    about watermarks the peer is WAITING on. On a shared page
//                    a release-store is both; on a stream, conflating them
//                    either sends too much or hangs a waiter.
//   PostReply /    - SEG_REPLY carries no message today: the server memcpys
//   ReadReply        into a slot addressed `seq % slotCount` and the client
//                    reads it after observing appliedSeq. A stream turns that
//                    into a real message carrying its own seq, which DELETES
//                    the slot pool, the modulus addressing and the stamp
//                    self-check - so the seam has to own the operation rather
//                    than hand out the pool.
//
// WHAT P6.5 STILL HAS TO SETTLE, recorded here so it is not rediscovered:
//   - SEG_STAGE's three ring cursors are DELIBERATELY DEAD (Ring.h's "DEAD IN P5, DELIBERATELY",
//     pinned by a test). Staging is an encoder-local linear allocator that
//     reclaims on retiredSeq. A send window either revives them COMPLETELY or
//     does not use them; the header names the middle state as a guaranteed
//     hang rather than a slow path.
//   - appliedSeq is excluded BY NAME from lazy publication (Ring.h's "BATCHING MAY ONLY MAKE A WATERMARK LATE").
//     P6.5 AMENDS that rule with a flush-on-idle discipline; it does not infer
//     around it.
//   - one dedicated reader thread per direction per process, or the event-ring
//     flow-control deadlock returns as write() blocked against write().

#pragma once

#include "ILink.h"

namespace MobileGL::MG_Remote::Transport {

    // Every entry point is a named refusal. There is no partial implementation
    // and there may not be one: a half-wired stream link is the shape Ring.h
    // warns about, where FreeBytes() falls to zero on the first lap and never
    // recovers.
    //
    // The refusals live in StreamLink.cpp (package P6.5) rather than inline,
    // for the reason ILink.h gives: no header under Transport/ pulls the logger
    // in, and this one does not start.
    class StreamLink final : public ILink {
    public:
        StreamLink();
        ~StreamLink() override;

        LinkCapabilities Capabilities() const override;

        LinkProgress* Progress() override;
        LinkEventFlags* EventFlags() override;
        LinkArena* RecordArena() override;
        LinkCursor* RecordCursor() override;

        MobileGLResult StageBytes(std::uint64_t bytes, LinkSpan* outSpan,
                                  void** outWritePtr) override;
        MobileGLResult ResolveSpan(LinkSegment segment, LinkSpan span,
                                   const void** outPtr) override;

        MobileGLResult PostReply(std::uint64_t seq, std::int32_t status,
                                 const void* payload, std::uint64_t size) override;
        MobileGLResult ReadReply(std::uint64_t seq, std::int32_t* outStatus,
                                 const void** outPayload, std::uint64_t* outSize) override;

        LinkArena* EventArena() override;
        LinkCursor* EventCursor() override;

        Doorbell& ConsumerBell() override;
        Doorbell& ProducerBell() override;

        MobileGLResult Flush() override;
        MobileGLResult FlushProgress() override;

        bool Attached() const override;
        void Detach() override;
        TransportRoleTag Role() const override;
    };

} // namespace MobileGL::MG_Remote::Transport
