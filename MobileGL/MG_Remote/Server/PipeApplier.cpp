// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for package v1 (with p1 for the stamp rule).

#include "PipeApplier.h"

#include "../Transport/ReplySlot.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Server {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedPipeApplier, \"%s\"} - P5 package v1 has not landed "                      \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    ReplyPool::ReplyPool(void* base, Uint64 sizeBytes, Uint32 slotCount, Uint32 slotBytes)
        : m_base(static_cast<Uint8*>(base)), m_size(sizeBytes), m_slots(slotCount), m_slotBytes(slotBytes) {}

    // PACKAGE s1's, not v1's, even though the class is declared in v1's header: the SEG_REPLY
    // slot pool is s1's deliverable (BRIEF §5) and its addressing lives in one place,
    // Transport/ReplySlot.h, which the CLIENT reads the same slots back through. Duplicating
    // `seq % slots` on this side is how the two halves come to disagree about which slot an
    // answer is in - and because seq IS the reply-slot id (R-3), a disagreement reads another
    // call's answer instead of failing.
    //
    // The view is rebuilt per call rather than stored, so that this body does not change
    // ReplyPool's four members and therefore does not touch v1's header at all.
    void ReplyPool::PostReply(Uint64 seq, Int32 status, const void* bytes, Uint64 size) {
        Transport::ReplySlotPool pool(m_base, m_size, m_slots);
        // Fatal inside Post when the answer does not fit a slot: P5 does not chunk replies,
        // and the client knows an answer's size before it emits the record.
        pool.Post(seq, status, bytes, size);
    }

    Uint32 ReplyPool::SlotBytes() const { return m_slotBytes; }

    PipeApplier::PipeApplier(Wire::SegmentTable* segments, ReplyPool* replies)
        : m_segments(segments), m_replies(replies) {}

    Bool PipeApplier::ApplyOne(const Transport::RingRecordView&) { MGP5_C0_STUB("PipeApplier::ApplyOne"); }

    void PipeApplier::StampVerbBoundary(MG_Pipe::MGPWireOp) {
        MGP5_C0_STUB("PipeApplier::StampVerbBoundary");
    }

    Uint64 PipeApplier::ResidualPullCount() const { return m_residualPulls; }

    void PipeApplier::PoisonRetiredStageBytes(Uint64, Uint64) {
        MGP5_C0_STUB("PipeApplier::PoisonRetiredStageBytes");
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Server
