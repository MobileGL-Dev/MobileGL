// MobileGL - MobileGL/MG_Remote/Server/PipeApplier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for package v1 (with p1 for the stamp rule).

#include "PipeApplier.h"

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

    void ReplyPool::PostReply(Uint64, Int32, const void*, Uint64) { MGP5_C0_STUB("ReplyPool::PostReply"); }

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
