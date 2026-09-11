// MobileGL - MobileGL/MG_Remote/Wire/PipeWireCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0's stubs for package w1. Every body is MGLOG_F + std::abort and NOT a silent no-op:
// an unimplemented codec that returns quietly is exactly how a split lane runs monolith and
// goes green, which is the failure the whole phase is built to make impossible.

#include "PipeWireCodec.h"

#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Wire {

    // Table 0's first row, mechanised: this enum and the schema's SegmentKind are ONE id
    // space, and the only place they are compared is here. A schema edit that renumbers a
    // segment is a build break rather than a wrong pointer on a ring.
    //
    // Fully qualified from the global namespace on purpose: the generated header's namespace
    // is `MobileGL::Wire` and we are inside `MobileGL::MG_Remote::Wire`, so a bare `Wire::`
    // resolves to THIS namespace and the assertion would silently be about the wrong enum -
    // or, as it first was, fail to compile for a reason that looks unrelated.
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::None) == kSegNone);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Cmd) == kSegCmd);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Stage) == kSegStage);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Reply) == kSegReply);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Event) == kSegEvent);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Shadow) == kSegShadow);
    static_assert(static_cast<Uint32>(::MobileGL::Wire::SegmentKind::Adopt) == kSegAdopt);
    // And the other half of table 0's rule: MG_Pipe's "no segment" sentinel is the same 0.
    static_assert(static_cast<Uint32>(MG_Pipe::kMGHostSpanSegNone) == kSegNone,
                  "kMGHostSpanSegNone and SegmentId::kSegNone must be the same value");

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedWireCodec, \"%s\"} - P5 package w1 has not landed "                        \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    void SegmentTable::Install(SegmentId, SegmentView) { MGP5_C0_STUB("SegmentTable::Install"); }

    SegmentView SegmentTable::Get(SegmentId) const { MGP5_C0_STUB("SegmentTable::Get"); }

    const void* SegmentTable::Resolve(Uint32, Uint64, Uint64) const {
        MGP5_C0_STUB("SegmentTable::Resolve");
    }

    void SegmentTable::InstallProcessResolver() { MGP5_C0_STUB("SegmentTable::InstallProcessResolver"); }

    void SegmentTable::UninstallProcessResolver() {
        MGP5_C0_STUB("SegmentTable::UninstallProcessResolver");
    }

    // NOT a stub: the two Fatal helpers are the one thing every package needs on day one, and
    // a Fatal that is itself unimplemented would report the wrong failure.
    void WireProtocolFatal(const char* what, const char* detail) {
        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} %s", what, detail != nullptr ? detail : "");
        std::abort();
    }

    void WireProtocolFatalAt(const char* what, Uint64 got, Uint64 expected) {
        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} got=%llu expected=%llu", what,
                static_cast<unsigned long long>(got), static_cast<unsigned long long>(expected));
        std::abort();
    }

    void CheckBlobIsHonest(MG_Pipe::MGPWireOp, const MG_Pipe::MGPBlobRef&, const SegmentTable&) {
        MGP5_C0_STUB("CheckBlobIsHonest");
    }

    void CheckHostSpanIsHonest(const MG_Pipe::MGHostSpan&) { MGP5_C0_STUB("CheckHostSpanIsHonest"); }

    PipeWireEncoder::PipeWireEncoder(Transport::RingControl* control, Transport::RingProducer* cmd,
                                     Transport::RingProducer* stage, SegmentTable* segments)
        : m_control(control), m_cmd(cmd), m_stage(stage), m_segments(segments) {}

    Bool PipeWireEncoder::Valid() const { return m_control != nullptr && m_cmd != nullptr; }

    MG_Pipe::MGPBlobRef PipeWireEncoder::StageBytes(const void*, Uint64) {
        MGP5_C0_STUB("PipeWireEncoder::StageBytes");
    }

    Uint64 PipeWireEncoder::EncodeRecord(MG_Pipe::MGPWireOp, const void*, Uint64, const void*, Uint64) {
        MGP5_C0_STUB("PipeWireEncoder::EncodeRecord");
    }

    void PipeWireEncoder::Publish() { MGP5_C0_STUB("PipeWireEncoder::Publish"); }

    Uint64 PipeWireEncoder::EmitSeq() const { return m_emitSeq; }

    Uint64 PipeWireEncoder::MaxRecordBytesSeen() const { return m_maxRecordBytes; }

    PipeWireDecoder::PipeWireDecoder(Transport::RingControl* control, SegmentTable* segments,
                                     ReplySink* replies)
        : m_control(control), m_segments(segments), m_replies(replies) {}

    Bool PipeWireDecoder::Valid() const { return m_control != nullptr && m_segments != nullptr; }

    Bool PipeWireDecoder::DecodeAndApply(const Transport::RingRecordView&) {
        MGP5_C0_STUB("PipeWireDecoder::DecodeAndApply");
    }

    Uint64 PipeWireDecoder::AppliedSeq() const { return m_applySeq; }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Wire
