// MobileGL - MobileGL/MG_Remote/Wire/PipeWireCodec.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package w1: the G3 record codec.
//
// ONE CALL IN, BYTES OUT; BYTES IN, ONE MGPipeApply* CALL OUT. This file owns NO semantics -
// MG_Pipe/PipeApply.cpp is not edited by this package and every decoder arm ends in an
// existing free function or in the verb sink v1 installs. An arm that "handles" a record
// itself rather than delegating is a review failure (R-4's rule, one level down).
//
// THE THING THAT MOST NEARLY WENT WRONG, WRITTEN AT THE TOP BECAUSE IT IS INVISIBLE:
// MGPWireRecHeader::Flags and Transport::RingRecordHeader::flags ARE THE SAME 16-BIT FIELD,
// and the two flag spaces COLLIDE. MGPipeCallFlags::kVarTail is 1<<2 and
// RingRecordFlags::kRecPad is 1<<2, so an encoder that stamped MGPipeCallFlagsFor(op) into
// the header - which the generated comment ("MGPipeCallFlags of the call") invites - would
// make RingConsumer::Pop skip every one of the nine kVarTail records as a WRAP FILLER.
// Silently, with no checksum anywhere on this ring. MGPipeCallFlags::kHostSpan (1<<3) lands
// on kRecBorrowSlot the same way and would retire those slots on the GPU timeline instead of
// on apply. So: THE HEADER CARRIES RING FRAMING FLAGS, and the call's own flags are read from
// the opcode through MGPipeCallFlagsFor (R-13.4), which is why that table was generated in the
// first place. The static_asserts below pin the collision so it cannot be re-introduced by
// someone who reads the comment and not this file.

#include "PipeWireCodec.h"

#include <Config.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
// R-6's tier gate, and the ONE spelling of it (b1's file, unchanged by this package): the
// MapPersistent arm below asks it the same question MGPipeApplyMapPersistent asks.
#include <MG_Remote/Client/PersistentMapTracker.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_Util/Debug/Log.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Wire {

    using namespace MobileGL::MG_Pipe;

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

    // ---- the collision, asserted EXHAUSTIVELY rather than described -------------------
    //
    // Naming the two overlaps somebody happened to notice is not enough: the review found a
    // THIRD (kReplySlot == kRecVarTail) that four hand-written asserts had missed, and the
    // fourth would have been found by a user. So the assertions below cover the whole of both
    // enums - every bit, the exact overlap mask, and the completeness of the translation - and
    // a new enumerator on either side breaks the build rather than being dropped in silence.
    namespace FlagSpace {
        constexpr Uint16 kCallAll = static_cast<Uint16>(MG_Pipe::kNeedsAck) |
                                    static_cast<Uint16>(MG_Pipe::kHasBlob) |
                                    static_cast<Uint16>(MG_Pipe::kVarTail) |
                                    static_cast<Uint16>(MG_Pipe::kHostSpan) |
                                    static_cast<Uint16>(MG_Pipe::kReplySlot) |
                                    static_cast<Uint16>(MG_Pipe::kOptional);
        constexpr Uint16 kRingAll = static_cast<Uint16>(Transport::kRecNeedsAck) |
                                    static_cast<Uint16>(Transport::kRecHasBlob) |
                                    static_cast<Uint16>(Transport::kRecPad) |
                                    static_cast<Uint16>(Transport::kRecBorrowSlot) |
                                    static_cast<Uint16>(Transport::kRecVarTail);
        // The three call flags the encoder TRANSLATES, and the three it deliberately drops
        // because MGPipeCallFlagsFor(op) recovers them from the opcode.
        constexpr Uint16 kTranslated = static_cast<Uint16>(MG_Pipe::kNeedsAck) |
                                       static_cast<Uint16>(MG_Pipe::kHasBlob) |
                                       static_cast<Uint16>(MG_Pipe::kVarTail);
        constexpr Uint16 kDropped = static_cast<Uint16>(MG_Pipe::kHostSpan) |
                                    static_cast<Uint16>(MG_Pipe::kReplySlot) |
                                    static_cast<Uint16>(MG_Pipe::kOptional);
    } // namespace FlagSpace

    static_assert(FlagSpace::kCallAll == 0x3Fu,
                  "MGPipeCallFlags gained or lost an enumerator; re-derive the translation in "
                  "EncodeRecord and extend the overlap assertions below before assuming the "
                  "new bit is safe to drop");
    static_assert(FlagSpace::kRingAll == 0x1Fu,
                  "RingRecordFlags gained or lost an enumerator; the two spaces share one "
                  "16-bit field, so a new ring bit may now alias a call flag");
    static_assert((FlagSpace::kTranslated | FlagSpace::kDropped) == FlagSpace::kCallAll &&
                      (FlagSpace::kTranslated & FlagSpace::kDropped) == 0,
                  "every MGPipeCallFlags bit must be either translated or deliberately "
                  "dropped; a seventh enumerator that needs a ring bit would otherwise be "
                  "dropped in silence");
    // FIVE OF THE SIX CALL-FLAG BITS ALIAS A RING BIT. Only kOptional (1<<5) is free, and it
    // is free by luck rather than by design - RingRecordFlags simply has not reached 1<<5 yet.
    static_assert((FlagSpace::kCallAll & FlagSpace::kRingAll) == FlagSpace::kRingAll,
                  "the overlap between the two flag spaces moved");
    // Named individually so a failure says WHICH pair, and so the three that actually bite are
    // impossible to overlook while reading.
    static_assert(static_cast<Uint16>(MG_Pipe::kNeedsAck) ==
                      static_cast<Uint16>(Transport::kRecNeedsAck),
                  "kNeedsAck == kRecNeedsAck (harmless: the meanings agree)");
    static_assert(static_cast<Uint16>(MG_Pipe::kHasBlob) ==
                      static_cast<Uint16>(Transport::kRecHasBlob),
                  "kHasBlob == kRecHasBlob (harmless: the meanings agree)");
    static_assert(static_cast<Uint16>(MG_Pipe::kVarTail) ==
                      static_cast<Uint16>(Transport::kRecPad),
                  "kVarTail == kRecPad - THE DANGEROUS ONE. Stamping MGPipeCallFlags into the "
                  "header would make RingConsumer::Pop discard all nine kVarTail opcodes as "
                  "wrap fillers. The encoder must translate, never stamp; if this ever stops "
                  "being true, keep translating anyway - two flag spaces in one field is the "
                  "hazard, not this particular overlap");
    static_assert(static_cast<Uint16>(MG_Pipe::kHostSpan) ==
                      static_cast<Uint16>(Transport::kRecBorrowSlot),
                  "kHostSpan == kRecBorrowSlot - a stamped host-span record would look like a "
                  "slot borrowed into the GPU timeline and retire on completedFrameSerial");
    static_assert(static_cast<Uint16>(MG_Pipe::kReplySlot) ==
                      static_cast<Uint16>(Transport::kRecVarTail),
                  "kReplySlot == kRecVarTail - live in the REVERSE direction: the encoder "
                  "stamps kRecVarTail on nine opcodes, and a reader who believes "
                  "PipeWire.inc's 'MGPipeCallFlags of the call' comment reads those nine as "
                  "kReplySlot");
    static_assert((static_cast<Uint16>(MG_Pipe::kOptional) & FlagSpace::kRingAll) == 0,
                  "kOptional is the one call flag with no ring alias, and only because "
                  "RingRecordFlags has not reached 1<<5");

    // ---- and the two headers really are one layout -----------------------------------
    //
    // The decoder walks backwards from RingRecordView::payload to the MGPWireRecHeader in
    // front of it, so the two structs being separately asserted to be eight bytes is not
    // enough: if RingRecordHeader ever reorders its fields, every flag assert above still
    // passes and every payload read shifts by the difference.
    static_assert(sizeof(MGPWireRecHeader) == sizeof(Transport::RingRecordHeader));
    static_assert(offsetof(MGPWireRecHeader, Op) == offsetof(Transport::RingRecordHeader, kind),
                  "MGPWireRecHeader::Op and RingRecordHeader::kind are the same two bytes");
    static_assert(offsetof(MGPWireRecHeader, Flags) ==
                      offsetof(Transport::RingRecordHeader, flags),
                  "MGPWireRecHeader::Flags and RingRecordHeader::flags are the same two bytes");
    static_assert(offsetof(MGPWireRecHeader, Size) == offsetof(Transport::RingRecordHeader, size),
                  "MGPWireRecHeader::Size and RingRecordHeader::size are the same four bytes");

    // ---------------------------------------------------------------------------------
    // The catalogue, once. Name and payload type per opcode.
    // ---------------------------------------------------------------------------------
    //
    // ONE LIST, THREE READERS: the name table (Fatal lines), the payload-size table (the tail
    // arithmetic's base) and the decoder's own completeness assertion. Hand-maintained lists
    // that say the same thing three times are how a catalogue edit lands in two of them.
    // gen_pipe.py owns PipeCalls.def and PipeWire.inc; this list is held to them by the
    // kOpCount assertion under it, which is the strongest statement a non-generated file can
    // make about a generated enum.
#define MGPW_FOR_EACH_CALL(X)                                                                  \
    X(GetCaps, MGPCaps)                                                                        \
    X(ResourceCreate, MGPResourceDesc)                                                         \
    X(ResourceRespecify, MGPResourceDesc)                                                      \
    X(ResourceDestroy, MGPHandleOnly)                                                          \
    X(MapPersistent, MGPHandleOnly)                                                            \
    X(UnmapPersistent, MGPHandleOnly)                                                          \
    X(FenceCreate, MGPHandleOnly)                                                              \
    X(FenceStatus, MGPHandleOnly)                                                              \
    X(FenceWait, MGPFenceWait)                                                                 \
    X(FenceDestroy, MGPHandleOnly)                                                             \
    X(QueryCreate, MGPQueryDesc)                                                               \
    X(QueryBegin, MGPQueryDesc)                                                                \
    X(QueryEnd, MGPQueryDesc)                                                                  \
    X(QueryAvailable, MGPHandleOnly)                                                           \
    X(QueryResult, MGPQueryResultRequest)                                                      \
    X(QueryDestroy, MGPHandleOnly)                                                             \
    X(CreateRenderState, MGPRenderStateDesc)                                                   \
    X(BindRenderState, MGPBindRenderState)                                                     \
    X(DeleteRenderState, MGPHandleOnly)                                                        \
    X(CreateVertexElements, MGPVertexElements)                                                 \
    X(BindVertexElements, MGPHandleOnly)                                                       \
    X(DeleteVertexElements, MGPHandleOnly)                                                     \
    X(CreateSamplerState, MGPSamplerDesc)                                                      \
    X(DeleteSamplerState, MGPHandleOnly)                                                       \
    X(CreateSamplerView, MGPSamplerView)                                                       \
    X(DeleteSamplerView, MGPHandleOnly)                                                        \
    X(CreateShaderState, MGPProgramDesc)                                                       \
    X(BindShaderState, MGPHandleOnly)                                                          \
    X(DeleteShaderState, MGPHandleOnly)                                                        \
    X(SetDynamicState, MGPDynamicState)                                                        \
    X(SetFramebufferState, MGPFramebufferState)                                                \
    X(SetVertexBuffers, MGPVertexBuffers)                                                      \
    X(SetIndexBuffer, MGPIndexBuffer)                                                          \
    X(SetIndirectBuffers, MGPIndirectBuffers)                                                  \
    X(SetSamplerViews, MGPSamplerViews)                                                        \
    X(BindSamplerStates, MGPSamplerStates)                                                     \
    X(SetShaderImages, MGPShaderImages)                                                        \
    X(SetShaderBuffers, MGPShaderBuffers)                                                      \
    X(SetStreamOutputTargets, MGPStreamOutputTargets)                                          \
    X(SetGlobalConstants, MGPGlobalConstants)                                                  \
    X(SetVertexAttribDefaults, MGPVertexAttribDefaults)                                        \
    X(SetPixelPackState, MGPPixelPackState)                                                    \
    X(SetPatchState, MGPPatchState)                                                            \
    X(SetDrawProgram, MGPHandleOnly)                                                           \
    X(SetDispatchProgram, MGPHandleOnly)                                                       \
    X(SetResidualValueState, MGPResidualValueState)                                            \
    X(SetTextureParams, MGPTextureParams)                                                      \
    X(ResourceSubData, MGPSubData)                                                             \
    X(BufferSubDataResident, MGPSubData)                                                       \
    X(ResourceSubDataComplete, MGPSubDataComplete)                                             \
    X(ResourceFlushRange, MGPFlushRange)                                                       \
    X(ResourceReadback, MGPReadback)                                                           \
    X(ResourceCopyRegion, MGPCopyRegion)                                                       \
    X(GenerateMipmap, MGPMipPlan)                                                              \
    X(GetTextureImage, MGPReadbackInfo)                                                        \
    X(Blit, MGPBlit)                                                                           \
    X(Clear, MGPClear)                                                                         \
    X(ReadPixels, MGPReadbackInfo)                                                             \
    X(DrawVbo, MGPDrawInfo)                                                                    \
    X(LaunchGrid, MGPGridInfo)                                                                 \
    X(MemoryBarrier, MGPMemoryBarrier)                                                         \
    X(BeginStreamOutput, MGPStreamOutputBegin)                                                 \
    X(EndStreamOutput, MGPXfbAccounting)                                                       \
    X(PauseStreamOutput, MGPStreamOutputControl)                                               \
    X(ResumeStreamOutput, MGPStreamOutputControl)                                              \
    X(Flush, MGPFlush)                                                                         \
    X(Present, MGPPresent)                                                                     \
    X(SetSwapInterval, MGPSwapInterval)                                                        \
    X(QueryTimestamp, MGPTimestampRequest)                                                     \
    X(QueryCounter, MGPQueryDesc)                                                              \
    X(FenceWaitServer, MGPFenceWait)                                                           \
    X(BindShaderImage, MGPImageBind)                                                           \
    X(PatchParameter, MGPPatchParameter)                                                       \
    X(BindStreamOutput, MGPStreamOutputBind)                                                   \
    X(SetStorageBlockBinding, MGPStorageBlockBinding)                                          \
    X(CopyFramebufferToTexture, MGPCopyFromFramebuffer)                                        \
    X(ApplierReset, MGPApplierReset)                                                           \
    X(ObjectDeath, MGPHandleOnly)                                                              \
    X(SetContextValues, MGPContextValues)                                                      \
    X(SetProgramBindings, MGPProgramBindings)

    namespace {

        constexpr Uint64 Align8(Uint64 value) { return (value + 7u) & ~Uint64(7u); }

#define MGPW_NAME_ROW(Name, Payload) #Name,
        constexpr const char* kWireOpNames[] = {
            "<kInvalid>",
            MGPW_FOR_EACH_CALL(MGPW_NAME_ROW)
        };
#undef MGPW_NAME_ROW

        // The catalogue grew or shrank. Add the new opcode to MGPW_FOR_EACH_CALL, give it an
        // arm in ApplyChecked, and decide in CONTRACT-P5.md table 1 whether it carries bytes.
        static_assert(sizeof(kWireOpNames) / sizeof(kWireOpNames[0]) ==
                          static_cast<SizeT>(MGPWireOp::kOpCount),
                      "PipeCalls.def and this file's call list disagree");

        // The payload struct's own sizeof, which is what the tail arithmetic starts from.
        // NOT sizeof(MGPWireRec_X) - 8: three payloads are not multiples of 8 and the record
        // is rounded up around them.
#define MGPW_SIZE_ROW(Name, Payload) sizeof(MG_Pipe::Payload),
        constexpr Uint64 kWirePayloadBytes[] = {
            0,
            MGPW_FOR_EACH_CALL(MGPW_SIZE_ROW)
        };
#undef MGPW_SIZE_ROW
        static_assert(sizeof(kWirePayloadBytes) / sizeof(kWirePayloadBytes[0]) ==
                          static_cast<SizeT>(MGPWireOp::kOpCount));

        // The header's Size field is 32 bits by wire contract (Ring.h's kMaxRingCapacity
        // comment). Every count-derived total is checked against this BEFORE it is used, so a
        // corrupt Count can never wrap the arithmetic that is supposed to catch it.
        constexpr Uint64 kMaxRecordBytesOnTheWire = 0xFFFFFFFFull;

        // MGPResidualValueState's blob is the block itself, which only ever ratchets DOWN.
        static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE);

        // CreateShaderState's seven blob members are one contiguous run, which is what lets
        // the honesty pass walk them as an array.
        static_assert(offsetof(MGPProgramDesc, Reflection) ==
                          offsetof(MGPProgramDesc, Spirv) + 6 * sizeof(MGPBlobRef),
                      "MGPProgramDesc's seven MGPBlobRefs must stay contiguous");
        static_assert(offsetof(MGPCaps, RendererInfo) ==
                          offsetof(MGPCaps, FormatCapabilities) + sizeof(MGPBlobRef),
                      "MGPCaps's two MGPBlobRefs must stay contiguous");

        // Where an op's MGPBlobRef members live inside its payload. Ten rows: the eight
        // kHasBlob calls plus the two R-13.1 corrected ones. A row's Count is how many
        // CONSECUTIVE MGPBlobRefs start at Offset.
        struct BlobSlots {
            Uint32 Offset = 0;
            Uint32 Count = 0;
        };

        BlobSlots BlobSlotsFor(MGPWireOp op) {
            switch (op) {
            case MGPWireOp::GetCaps:
                return {static_cast<Uint32>(offsetof(MGPCaps, FormatCapabilities)), 2};
            case MGPWireOp::CreateRenderState:
                return {static_cast<Uint32>(offsetof(MGPRenderStateDesc, Blob)), 1};
            case MGPWireOp::CreateVertexElements:
                return {static_cast<Uint32>(offsetof(MGPVertexElements, Blob)), 1};
            case MGPWireOp::CreateSamplerState:
                return {static_cast<Uint32>(offsetof(MGPSamplerDesc, Parameters)), 1};
            case MGPWireOp::CreateShaderState:
                return {static_cast<Uint32>(offsetof(MGPProgramDesc, Spirv)), 7};
            case MGPWireOp::SetDynamicState:
                return {static_cast<Uint32>(offsetof(MGPDynamicState, Blob)), 1};
            case MGPWireOp::SetGlobalConstants:
                return {static_cast<Uint32>(offsetof(MGPGlobalConstants, Blob)), 1};
            case MGPWireOp::SetResidualValueState:
                return {static_cast<Uint32>(offsetof(MGPResidualValueState, Blob)), 1};
            case MGPWireOp::ResourceSubData:
            case MGPWireOp::BufferSubDataResident:
                return {static_cast<Uint32>(offsetof(MGPSubData, Blob)), 1};
            // P5b: the block NAME rides SEG_STAGE (CONTRACT-P5B.md i1).
            case MGPWireOp::SetStorageBlockBinding:
                return {static_cast<Uint32>(offsetof(MGPStorageBlockBinding, Name)), 1};
            default:
                return {};
            }
        }

        Uint32 PopCount32(Uint32 value) {
            Uint32 count = 0;
            while (value != 0) {
                value &= value - 1u;
                ++count;
            }
            return count;
        }

        // The one place a count-times-element-size becomes a byte count. Uint32 * a small
        // constant cannot overflow 64 bits, and the total is bounded above before anyone
        // indexes with it.
        Uint64 TailBytesFor(Uint64 count, Uint64 elementBytes) { return count * elementBytes; }

        // The process resolver is a plain function pointer with no user datum
        // (MGPipeHostSpan.h:46), so the table it resolves through has to be found here.
        SegmentTable* g_processResolverTable = nullptr;

        const void* ProcessResolverThunk(Uint32 seg, Uint64 offset, Uint64 size) {
            if (g_processResolverTable == nullptr) {
                return nullptr;
            }
            return g_processResolverTable->Resolve(seg, offset, size);
        }

        // The decoder currently inside MGPipeApplyWireRecord. Thread-local rather than a file
        // static: there is one apply thread by construction, but a unit suite drives several
        // decoders from several test threads and a shared slot would make those cases depend
        // on each other.
        thread_local PipeWireDecoder* t_activeDecoder = nullptr;

    } // namespace

    const char* WireOpName(MGPWireOp op) {
        const SizeT index = static_cast<SizeT>(op);
        if (index >= static_cast<SizeT>(MGPWireOp::kOpCount)) {
            return "<opcode>";
        }
        return kWireOpNames[index];
    }

    // ---------------------------------------------------------------------------------
    // The Fatal arms, worded once
    // ---------------------------------------------------------------------------------

    void WireProtocolFatal(const char* what, const char* detail) {
        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} %s", what, detail != nullptr ? detail : "");
        std::abort();
    }

    void WireProtocolFatalAt(const char* what, Uint64 got, Uint64 expected) {
        MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} got=%llu expected=%llu", what,
                static_cast<unsigned long long>(got), static_cast<unsigned long long>(expected));
        std::abort();
    }

    // ---------------------------------------------------------------------------------
    // SegmentTable
    // ---------------------------------------------------------------------------------

    void SegmentTable::Install(SegmentId seg, SegmentView view) {
        if (seg == kSegNone || static_cast<Uint32>(seg) > static_cast<Uint32>(kSegAdopt)) {
            WireProtocolFatalAt("SegmentTable::Install", static_cast<Uint64>(seg),
                                static_cast<Uint64>(kSegAdopt));
        }
        m_views[static_cast<SizeT>(seg)] = view;
    }

    SegmentView SegmentTable::Get(SegmentId seg) const {
        if (seg == kSegNone || static_cast<Uint32>(seg) > static_cast<Uint32>(kSegAdopt)) {
            return SegmentView{};
        }
        return m_views[static_cast<SizeT>(seg)];
    }

    const void* SegmentTable::Resolve(Uint32 seg, Uint64 offset, Uint64 size) const {
        // 0 is "no segment", ALWAYS (table 0), and 0xFFFFFFFF is P8's index-mirror sentinel
        // which this phase does not resolve. Both land here as "unknown", and the CALLER
        // escalates to Fatal - a unit case has to be able to exercise this arithmetic without
        // dying.
        if (seg == kSegNone || seg > static_cast<Uint32>(kSegAdopt)) {
            return nullptr;
        }
        if (size == 0) {
            return nullptr;
        }
        const SegmentView& view = m_views[static_cast<SizeT>(seg)];
        if (view.Base == nullptr) {
            return nullptr;
        }
        // Written as a subtraction so offset + size cannot wrap: both are attacker-controlled
        // in the only sense that matters here - they came off a shared page.
        if (offset > view.Size || size > view.Size - offset) {
            return nullptr;
        }
        return static_cast<const Uint8*>(view.Base) + offset;
    }

    void SegmentTable::InstallProcessResolver() {
        if (MG_Pipe::gMGPipeSegmentResolver != nullptr && g_processResolverTable != this) {
            // Table 3: there is exactly ONE gMGPipeSegmentResolver per process, the SERVER
            // role installs it before the apply thread starts, and the client never resolves a
            // span at all. Two roles racing on one inline variable is loud rather than silent
            // because of this line.
            WireProtocolFatal("SegmentTable::InstallProcessResolver",
                              "a process segment resolver is already installed; the server role "
                              "installs it once, before the apply thread starts");
        }
        g_processResolverTable = this;
        MG_Pipe::gMGPipeSegmentResolver = &ProcessResolverThunk;
    }

    void SegmentTable::UninstallProcessResolver() {
        // Teardown order (table 3): uninstall AFTER the join, never before - a record still in
        // flight can still resolve.
        MG_Pipe::gMGPipeSegmentResolver = nullptr;
        g_processResolverTable = nullptr;
    }

    // ---------------------------------------------------------------------------------
    // R-2's honesty arms
    // ---------------------------------------------------------------------------------

    void CheckBlobIsHonest(MGPWireOp op, const MGPBlobRef& blob, const SegmentTable& segments) {
        if (blob.Size == 0) {
            if (blob.Seg != kSegNone || blob.Offset != 0) {
                // The monolith shape: Seg = None, Offset = a host address, Size = 0. Legal
                // there, and precisely what must not cross (rule A). Reading it as "absent"
                // would silently drop the bytes of every record an unconverted emitter sent.
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s.blob\"} a blob with Size 0 "
                        "declares Seg=%u Offset=%llu; under split a blobref is either fully "
                        "declared or all three fields zero",
                        WireOpName(op), static_cast<unsigned>(blob.Seg),
                        static_cast<unsigned long long>(blob.Offset));
                std::abort();
            }
            return;
        }
        if (blob.Seg == kSegNone) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s.blob\"} Size=%llu with no segment "
                    "(R-2.3): a non-zero Size must name a real SEG_*",
                    WireOpName(op), static_cast<unsigned long long>(blob.Size));
            std::abort();
        }
        // R-2.3's SECOND HALF: "inside SOME segment" IS NOT THE RULE. Contract table 1 gives
        // every client->server content blob - groups A, B and C, all nineteen rows - the ONE
        // carrier SEG_STAGE, and R-10 sends blobs there whole. Until this arm existed the only
        // test was that the run resolved, so `CreateSamplerState.Parameters={Seg=SEG_REPLY,...}`
        // was accepted and APPLIED: a server-owned segment, whose reuse is the reply pool's
        // business and has nothing to do with stage retirement, carrying bytes the applier
        // then read. It also went unpoisoned - NoteResolvedRun skipped every non-stage carrier
        // - so rule C's only mechanical control read zero on exactly the record that needed it.
        //
        // The segment is checked BEFORE the resolve, deliberately: a forged SEG_REPLY run that
        // happens to lie inside a mapped reply pool must be refused for naming the wrong
        // carrier, not left to pass or fail on whether that pool is mapped at all.
        //
        // NOT A NEW FATAL FAMILY. The review suggested `Fatal{BlobNotStaged}`; this is
        // ProtocolCorruption like every other R-2 honesty arm, because the families are the
        // vocabulary the operator and the CI greps share (ProtocolCorruption, AbiMismatch,
        // UnmigratedVerb, UnmigratedPipeInput, UnsetCallMask, RingOverrun) and a one-off
        // seventh name would be a token nothing else in the tree recognises. The SEGMENT is in
        // the message, which is what has to be greppable.
        if (blob.Seg != kSegStage) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s.blob\"} seg=%u offset=%llu "
                    "size=%llu is not SEG_STAGE(%u); every client->server content blob is "
                    "staged whole in SEG_STAGE (contract table 1 groups A/B/C, R-10) and no "
                    "other segment may carry one",
                    WireOpName(op), static_cast<unsigned>(blob.Seg),
                    static_cast<unsigned long long>(blob.Offset),
                    static_cast<unsigned long long>(blob.Size),
                    static_cast<unsigned>(kSegStage));
            std::abort();
        }
        if (segments.Resolve(blob.Seg, blob.Offset, blob.Size) == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s.blob\"} seg=%u offset=%llu size=%llu "
                    "does not lie inside that segment (R-2.3)",
                    WireOpName(op), static_cast<unsigned>(blob.Seg),
                    static_cast<unsigned long long>(blob.Offset),
                    static_cast<unsigned long long>(blob.Size));
            std::abort();
        }
    }

    void RequireDeclaredBlob(MGPWireOp op, const MGPBlobRef& blob, const SegmentTable& segments) {
        if (blob.Size == 0) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s.blob\"} the record's own fields say "
                    "it carries content and its blob declares none (R-2.2 / rule A)",
                    WireOpName(op));
            std::abort();
        }
        CheckBlobIsHonest(op, blob, segments);
    }

    void CheckHostSpanIsHonest(const MGHostSpan& span) {
        if (span.Ptr != nullptr) {
            // Rule B. In one address space this pointer WORKS, which is the whole reason the
            // rule has to be mechanical: an inproc implementation that kept using it is
            // indistinguishable from a correct one until the day it is a second process.
            WireProtocolFatal("host-span",
                              "MGHostSpan::Ptr is non-null under split; the encoder writes "
                              "nullptr and names SEG_STAGE (R-2.1)");
        }
        if (span.Size != 0 && span.Seg == kMGHostSpanSegNone) {
            WireProtocolFatalAt("host-span.seg", span.Size, 0);
        }
        if (span.Size == 0 && span.Seg != kMGHostSpanSegNone) {
            WireProtocolFatalAt("host-span.size", span.Seg, 0);
        }
    }

    void CheckHostSpanIsHonest(const MGHostSpan& span, const SegmentTable& segments) {
        CheckHostSpanIsHonest(span);
        if (span.Size == 0) {
            // Fully absent, and the arms above already proved Seg agrees with that.
            return;
        }
        // ARM 4, WHICH THE OTHER OVERLOAD CANNOT DO. A span naming a real segment and a run
        // past the end of it used to pass every check and reach WireVerbSink::OnDrawVbo, which
        // this file's header promises is "a DECODED, VALIDATED argument list". It resolves to
        // nullptr through MGPipeHostBytes - a draw from a null index pointer - or, for any
        // consumer that adds Offset to its own SEG_STAGE base instead of going through the
        // resolver, reads outside the segment. P5 emits no spans, so this was latent; P8 arms
        // it, which is exactly when nobody will be reading this code.
        if (segments.Resolve(span.Seg, span.Offset, span.Size) == nullptr) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"host-span\"} seg=%u offset=%llu "
                    "size=%llu does not lie inside that segment (R-2.3 arm 4)",
                    static_cast<unsigned>(span.Seg),
                    static_cast<unsigned long long>(span.Offset),
                    static_cast<unsigned long long>(span.Size));
            std::abort();
        }
    }

    void CheckDrawUserIndices(const MGPDrawInfo& info, const MGPDrawRange* ranges,
                              const MGHostSpan& span) {
        if ((info.Flags & kDrawHasUserIndices) == 0 || (info.Flags & kDrawIsIndirect) != 0 ||
            info.NumDraws != 1 || ranges == nullptr) {
            WireProtocolFatal("DrawVbo.userIndices.shape",
                              "a user-index span requires exactly one direct indexed range");
        }
        if (info.IndexSize != 1 && info.IndexSize != 2 && info.IndexSize != 4) {
            WireProtocolFatalAt("DrawVbo.userIndices.IndexSize", info.IndexSize, 4);
        }
        // Client arrays are staged from their first index; Start only addresses an EBO.
        if (ranges[0].Start != 0) {
            WireProtocolFatalAt("DrawVbo.userIndices.Start", ranges[0].Start, 0);
        }
        const Uint64 required = static_cast<Uint64>(ranges[0].Count) * info.IndexSize;
        if (required > span.Size) {
            WireProtocolFatalAt("DrawVbo.userIndices.extent", required, span.Size);
        }
    }

    // ---------------------------------------------------------------------------------
    // The record layout: the tail arithmetic both sides run
    // ---------------------------------------------------------------------------------

    Bool MGPipeWireRecordLayout(MGPWireOp op, const void* payload, WireRecordLayout& out) {
        out = WireRecordLayout{};
        const SizeT index = static_cast<SizeT>(op);
        if (index == 0 || index >= static_cast<SizeT>(MGPWireOp::kOpCount)) {
            return false;
        }
        out.PayloadBytes = kWirePayloadBytes[index];
        const Uint64 afterPayload = Align8(sizeof(MGPWireRecHeader) + out.PayloadBytes);
        out.TotalBytes = afterPayload;
        if (payload == nullptr) {
            return true;
        }

        // Tail sizes. The COUNTS come off the wire, so every product is bounded before it is
        // believed, and the bound is the header's own 32-bit Size field.
        Uint64 tail0 = 0;
        Uint64 tail1 = 0;
        Uint64 tail2 = 0; // P5e: set_program_bindings is the one three-tail row
        Uint32 tails = 0;
        switch (op) {
        case MGPWireOp::SetVertexBuffers: {
            const auto& p = *static_cast<const MGPVertexBuffers*>(payload);
            if (p.Count > kMGPipeMaxVertexAttribs ||
                static_cast<Uint64>(p.Start) + p.Count > kMGPipeMaxVertexAttribs) {
                WireProtocolFatalAt("SetVertexBuffers.Count",
                                    static_cast<Uint64>(p.Start) + p.Count, kMGPipeMaxVertexAttribs);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPVertexBuffer));
            tails = 1;
            break;
        }
        case MGPWireOp::SetSamplerViews: {
            const auto& p = *static_cast<const MGPSamplerViews*>(payload);
            if (static_cast<Uint64>(p.Start) + p.Count > kMGPipeMaxTextureUnits) {
                WireProtocolFatalAt("SetSamplerViews.Count", static_cast<Uint64>(p.Start) + p.Count,
                                    kMGPipeMaxTextureUnits);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPBoundView));
            tails = 1;
            break;
        }
        case MGPWireOp::BindSamplerStates: {
            const auto& p = *static_cast<const MGPSamplerStates*>(payload);
            if (static_cast<Uint64>(p.Start) + p.Count > kMGPipeMaxTextureUnits) {
                WireProtocolFatalAt("BindSamplerStates.Count",
                                    static_cast<Uint64>(p.Start) + p.Count, kMGPipeMaxTextureUnits);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPipeHandle));
            tails = 1;
            break;
        }
        case MGPWireOp::SetShaderImages: {
            const auto& p = *static_cast<const MGPShaderImages*>(payload);
            if (static_cast<Uint64>(p.Start) + p.Count > kMGPipeMaxImageUnits) {
                WireProtocolFatalAt("SetShaderImages.Count", static_cast<Uint64>(p.Start) + p.Count,
                                    kMGPipeMaxImageUnits);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPImageView));
            tails = 1;
            break;
        }
        case MGPWireOp::SetShaderBuffers: {
            // TWO TAILS. HostSpanCount is 0 or Count and NEVER anything else
            // (MGPipeTypes.h:820-823), because the two arrays stay index-aligned; a third
            // value would let a record describe spans for ranges it does not have.
            const auto& p = *static_cast<const MGPShaderBuffers*>(payload);
            if (p.HostSpanCount != 0 && p.HostSpanCount != p.Count) {
                WireProtocolFatalAt("SetShaderBuffers.HostSpanCount", p.HostSpanCount, p.Count);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPBufferRange));
            tail1 = TailBytesFor(p.HostSpanCount, sizeof(MGHostSpan));
            tails = p.HostSpanCount != 0 ? 2 : 1;
            out.SecondTailIsHostSpans = true;
            break;
        }
        case MGPWireOp::SetStreamOutputTargets: {
            // TWO TAILS, one Count. MGPBufferRange[Count] then Uint32[Count].
            const auto& p = *static_cast<const MGPStreamOutputTargets*>(payload);
            tail0 = TailBytesFor(p.Count, sizeof(MGPBufferRange));
            tail1 = TailBytesFor(p.Count, sizeof(Uint32));
            tails = 2;
            break;
        }
        case MGPWireOp::SetProgramBindings: {
            // THREE TAILS, THREE COUNTS, THREE INDEX SPACES (P5e,
            // MG_Remote/CONTRACT-P5E.md §1): Int32[BlockBindingCount] in GL uniform-block index
            // order, MGPProgramSamplerUnit[SamplerUnitCount] ascending by Location, and
            // MGPProgramStorageOverride[StorageOverrideCount] whose Name is a host span staged
            // whole - the override map is name-keyed by design.
            //
            // EACH COUNT IS REFUSED AGAINST ITS DECLARED BOUND, never truncated: a sampler
            // array past 256 locations is a program this record cannot describe, and silently
            // describing 256 of them would put uniforms on the wrong units with no marker
            // anywhere. The counts come off the wire, so every one is bounded before it is
            // multiplied.
            const auto& p = *static_cast<const MGPProgramBindings*>(payload);
            if (p.BlockBindingCount > kMGPipeMaxProgramBlockBindings) {
                WireProtocolFatalAt("SetProgramBindings.BlockBindingCount", p.BlockBindingCount,
                                    kMGPipeMaxProgramBlockBindings);
            }
            if (p.SamplerUnitCount > kMGPipeMaxProgramSamplerUnits) {
                WireProtocolFatalAt("SetProgramBindings.SamplerUnitCount", p.SamplerUnitCount,
                                    kMGPipeMaxProgramSamplerUnits);
            }
            if (p.StorageOverrideCount > kMGPipeMaxProgramStorageOverrides) {
                WireProtocolFatalAt("SetProgramBindings.StorageOverrideCount",
                                    p.StorageOverrideCount, kMGPipeMaxProgramStorageOverrides);
            }
            tail0 = TailBytesFor(p.BlockBindingCount, sizeof(Int32));
            tail1 = TailBytesFor(p.SamplerUnitCount, sizeof(MGPProgramSamplerUnit));
            tail2 = TailBytesFor(p.StorageOverrideCount, sizeof(MGPProgramStorageOverride));
            // ALWAYS THREE, even when a count is 0. The tails are positional and the third one's
            // offset is derived from the first two, so a record that declared "one tail" when
            // only the block bindings were present would put the same bytes at a different
            // offset than a record that declared three - and the two halves of the wire would
            // disagree about where the overrides start. An empty tail is zero bytes at an
            // 8-aligned offset, which costs nothing.
            tails = 3;
            // NOT SecondTailIsHostSpans: the spans are MEMBERS of the third tail's elements,
            // not a bare MGHostSpan array, so the encoder's blanket honesty pass would read 40
            // bytes of {span, binding, pad} as one span and a quarter of garbage. The arm in
            // ApplyChecked walks the elements and checks each Name itself.
            break;
        }
        case MGPWireOp::SetVertexAttribDefaults: {
            // TWO DECLARANTS THAT MUST AGREE (contract table 1 row 15): Count and
            // popcount(Mask). Nothing checks this today; a disagreement is a wire fault that
            // scatters attribute values onto the wrong locations.
            const auto& p = *static_cast<const MGPVertexAttribDefaults*>(payload);
            if (p.Count != PopCount32(p.Mask)) {
                WireProtocolFatalAt("SetVertexAttribDefaults.Count", p.Count, PopCount32(p.Mask));
            }
            if (p.Count > kMGPipeMaxVertexAttribs) {
                WireProtocolFatalAt("SetVertexAttribDefaults.Count", p.Count, kMGPipeMaxVertexAttribs);
            }
            tail0 = TailBytesFor(p.Count, sizeof(MGPAttribValue));
            tails = 1;
            break;
        }
        case MGPWireOp::ResourceSubData: {
            const auto& p = *static_cast<const MGPSubData*>(payload);
            tail0 = TailBytesFor(p.RegionCount, sizeof(MGPSubRegion));
            tails = 1;
            break;
        }
        case MGPWireOp::BufferSubDataResident: {
            // NO TAIL, because its catalogue row has no kVarTail (PipeCalls.def gives it
            // kHasBlob|kOptional) and MGPipeApplyBufferSubDataResident takes no regions. The
            // layout used to share ResourceSubData's arm, which meant a record with
            // RegionCount = 2 was REQUIRED to carry 80 bytes the arm then dropped on the
            // floor - and MGPipeBuildSubDataRecord is the shared builder that fills
            // RegionCount for both halves, so that was one routing change away from being
            // live. The resident path is the BUFFER half only and a buffer record declares no
            // regions, so a non-zero count is a fault rather than a tail.
            const auto& p = *static_cast<const MGPSubData*>(payload);
            if (p.RegionCount != 0) {
                WireProtocolFatalAt("BufferSubDataResident.RegionCount", p.RegionCount, 0);
            }
            break;
        }
        case MGPWireOp::DrawVbo: {
            // MGPDrawRange[NumDraws], then a CONDITIONAL MGHostSpan. The span's start is
            // realigned to 8 because MGPDrawRange is twelve bytes: see WireRecordLayout's
            // header comment.
            const auto& p = *static_cast<const MGPDrawInfo*>(payload);
            tail0 = TailBytesFor(p.NumDraws, sizeof(MGPDrawRange));
            const Bool userIndices = (p.Flags & kDrawHasUserIndices) != 0;
            const Bool indirect = (p.Flags & kDrawIsIndirect) != 0;
            // P5b (CONTRACT-P5B.md d1): the second tail is the user-index span OR the indirect
            // block, never both - an indirect draw takes its indices from the bound element
            // buffer by GL rule - and an indirect draw declares no ranges, because the server
            // never reads the indirect buffer to learn a count and the client has none to send.
            if (userIndices && indirect) {
                WireProtocolFatal("DrawVbo.Flags",
                                  "kDrawHasUserIndices and kDrawIsIndirect are exclusive; an "
                                  "indirect draw's indices come from the bound element buffer");
            }
            if (indirect && p.NumDraws != 0) {
                WireProtocolFatalAt("DrawVbo.NumDraws", p.NumDraws, 0);
            }
            if (userIndices) {
                tail1 = sizeof(MGHostSpan);
                tails = 2;
                out.SecondTailIsHostSpans = true;
            } else if (indirect) {
                tail1 = sizeof(MGPDrawIndirect);
                tails = 2;
            } else {
                tails = 1;
            }
            break;
        }
        default:
            break;
        }

        if (tails >= 1) {
            out.TailOffset[0] = afterPayload;
            out.TailBytes[0] = tail0;
            out.TotalBytes = afterPayload + tail0;
        }
        if (tails >= 2) {
            out.TailOffset[1] = Align8(out.TailOffset[0] + tail0);
            out.TailBytes[1] = tail1;
            out.TotalBytes = out.TailOffset[1] + tail1;
        }
        // P5e: the third tail, realigned to 8 for the second one's reason - set_program_bindings'
        // first tail is Int32[] and its elements are four bytes, so the array behind it would
        // otherwise start at a 4-aligned offset and every MGHostSpan in the third would be
        // misaligned on a host that cares.
        if (tails >= 3) {
            out.TailOffset[2] = Align8(out.TailOffset[1] + tail1);
            out.TailBytes[2] = tail2;
            out.TotalBytes = out.TailOffset[2] + tail2;
        }
        out.TailCount = tails;
        out.TotalBytes = Align8(out.TotalBytes);
        if (out.TotalBytes > kMaxRecordBytesOnTheWire) {
            WireProtocolFatalAt("record.Size", out.TotalBytes, kMaxRecordBytesOnTheWire);
        }
        return true;
    }

    // ---------------------------------------------------------------------------------
    // Encoder
    // ---------------------------------------------------------------------------------

    PipeWireEncoder::PipeWireEncoder(Transport::RingControl* control, Transport::RingProducer* cmd,
                                     Transport::RingProducer* stage, SegmentTable* segments)
        : m_control(control), m_cmd(cmd), m_stage(stage), m_segments(segments) {}

    Bool PipeWireEncoder::Valid() const { return m_control != nullptr && m_cmd != nullptr; }

    Uint8* PipeWireEncoder::StageAllocate(Uint64 size) {
        if (m_stageBase == nullptr) {
            const SegmentView view = m_segments != nullptr ? m_segments->Get(kSegStage)
                                                           : SegmentView{};
            if (view.Base == nullptr || view.Size == 0) {
                WireProtocolFatal("PipeWireEncoder::StageBytes", "SEG_STAGE has no segment view");
            }
            // The RingProducer c0's constructor takes is the authority on how many of the
            // segment's bytes are really the staging area - the rest is whatever the session
            // put in front of it. It is read, never written: SEG_STAGE's cursor triple belongs
            // to nobody in P5 (see ReclaimStagedBytes' header).
            Uint64 capacity = view.Size;
            if (m_stage != nullptr && m_stage->Valid()) {
                if (m_stage->Capacity() > view.Size) {
                    WireProtocolFatalAt("SEG_STAGE.capacity", m_stage->Capacity(), view.Size);
                }
                capacity = m_stage->Capacity();
            }
            m_stageBase = static_cast<Uint8*>(view.Base);
            m_stageCapacity = capacity;
        }

        const Uint64 need = Align8(size);
        if (need > m_stageCapacity) {
            MGLOG_F("MGPipe: Fatal{RingOverrun, \"SEG_STAGE\"} a %llu byte blob cannot fit a "
                    "%llu byte staging segment at any occupancy; P5 does not chunk (R-10) - "
                    "raise MOBILEGL_IPC_STAGE_MB or report the record to the integrator",
                    static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(m_stageCapacity));
            std::abort();
        }

        bool reclaimed = false;
        bool waited = false;
        for (;;) {
            // THE WRAP SKIP MAY ONLY BE CHARGED AGAINST BYTES THAT ARE STILL IN FLIGHT. When
            // there are none the allocator starts over at offset zero, so a blob the segment
            // can hold whole is never refused (see RebaseEmptyStage). On retry this runs
            // AFTER ReclaimStagedBytes, which is the case the finding describes: 8 MiB
            // allocated, then retired, then a 28 MiB request that used to abort.
            RebaseEmptyStage();
            const Uint64 offset = m_stageHead % m_stageCapacity;
            // A run is always contiguous: one that would straddle the end skips the remainder,
            // exactly as the ring's wrap pad does, and the skipped bytes are reclaimed with
            // everything else behind them.
            const Uint64 skip = offset + need > m_stageCapacity ? m_stageCapacity - offset : 0;
            if ((m_stageHead + skip + need) - m_stageTail <= m_stageCapacity) {
                const Uint64 at = (m_stageHead + skip) % m_stageCapacity;
                m_stageHead += skip + need;
                return m_stageBase + at;
            }
            if (!reclaimed) {
                // Lazy reclamation of already-retired bytes is NOT a producer wait.
                ReclaimStagedBytes();
                reclaimed = true;
                continue;
            }
            if (m_stageRetirementBell == nullptr || m_stageMarkFront == m_stageMarks.size()) break;
            const Uint64 pending = m_stageMarks[m_stageMarkFront].Seq;
            const auto ready = [&] {
                return m_control->retiredSeq.load(std::memory_order_acquire) >= pending;
            };
            if (!ready()) {
                // The allocation still cannot progress after reclamation. Count this
                // blocked allocation once, not each watermark poll or each reclaimed mark.
                if (!waited) { ++m_stageReclaimWaits; waited = true; }
                if (!m_stageRetirementBell->Wait(m_control->producerParked, ready, 0, 5000)) {
                    MGLOG_F("MGPipe: Fatal{RetirementWaitFailed, \"SEG_STAGE\"} producer wait "
                            "ended before the pending allocation retired (shutdown or timeout)");
                    std::abort();
                }
            }
            ReclaimStagedBytes();
        }
        MGLOG_F("MGPipe: Fatal{RingOverrun, \"SEG_STAGE\"} a %llu byte blob does not fit a %llu "
                "byte staging segment with %llu bytes still in flight (retiredSeq=%llu); P5 "
                "does not chunk (R-10) - raise MOBILEGL_IPC_STAGE_MB or report the record to "
                "the integrator",
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(m_stageCapacity),
                static_cast<unsigned long long>(m_stageHead - m_stageTail),
                static_cast<unsigned long long>(
                    m_control != nullptr ? m_control->retiredSeq.load(std::memory_order_acquire) : 0));
        std::abort();
    }

    MGPBlobRef PipeWireEncoder::StageBytes(const void* bytes, Uint64 size) {
        if (!Valid() || m_segments == nullptr) {
            WireProtocolFatal("PipeWireEncoder::StageBytes",
                              "no command producer or segment table installed");
        }
        if (size == 0) {
            // "The record declared no blob" and "the record declared an empty blob" must not
            // be spelled the same way on a wire (R-2.2), so this is a programming error rather
            // than an empty ref.
            WireProtocolFatal("PipeWireEncoder::StageBytes",
                              "a content blob may not declare zero bytes (R-2.2)");
        }
        if (bytes == nullptr) {
            WireProtocolFatal("PipeWireEncoder::StageBytes", "non-zero size with a null source");
        }

        Uint8* slot = StageAllocate(size);
        std::memcpy(slot, bytes, static_cast<SizeT>(size));
        const Uint64 offset = static_cast<Uint64>(slot - m_stageBase);

        MGPBlobRef ref{};
        ref.Seg = kSegStage;
        ref.Offset = offset;
        ref.Size = size;
        ref.Pad0 = 0;
        // The self-check that keeps the two halves of "SEG_STAGE" one thing: the segment view
        // the decoder resolves through must cover the bytes this allocator just wrote into. A
        // view installed over the CONTROL page, or over the segment plus a header, resolves to
        // a plausible pointer that is not these bytes.
        if (m_segments->Resolve(ref.Seg, ref.Offset, ref.Size) != slot) {
            WireProtocolFatal("PipeWireEncoder::StageBytes",
                              "the SEG_STAGE segment view does not cover the staging area; the "
                              "two would resolve to different addresses");
        }
        return ref;
    }

    Uint64 PipeWireEncoder::EncodeRecord(MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                         const void* varTail, Uint64 varTailBytes) {
        WireTail tail{varTail, varTailBytes};
        return EncodeRecord(op, payload, payloadBytes, &tail, varTail != nullptr ? 1u : 0u);
    }

    Uint64 PipeWireEncoder::EncodeRecord(MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                         const WireTail* tails, Uint32 tailCount) {
        if (!Valid()) {
            WireProtocolFatal("PipeWireEncoder::EncodeRecord", "no SEG_CMD producer installed");
        }
        if (payload == nullptr) {
            WireProtocolFatal("PipeWireEncoder::EncodeRecord", "null payload");
        }

        WireRecordLayout layout{};
        if (!MGPipeWireRecordLayout(op, payload, layout)) {
            WireProtocolFatalAt("PipeWireEncoder::EncodeRecord",
                                static_cast<Uint64>(op),
                                static_cast<Uint64>(MGPWireOp::kOpCount));
        }
        if (payloadBytes != layout.PayloadBytes) {
            WireProtocolFatalAt("EncodeRecord.payloadBytes", payloadBytes, layout.PayloadBytes);
        }
        // The caller's tails are held to the layout the PAYLOAD declares, which is the same
        // arithmetic the decoder will run. A Count that says 4000 while the tail holds 8 bytes
        // dies here, on the producing side, rather than on a peer that can only say "corrupt".
        //
        // A caller may SUPPLY FEWER TAILS THAN THE LAYOUT HAS, but only while the ones it left
        // out are empty - Count == 0 is a legal record for every kVarTail row, and requiring a
        // {nullptr, 0} entry for it would be a trap rather than a check. Anything else is a
        // disagreement between the counts the payload declares and the bytes the caller holds.
        if (tailCount > layout.TailCount) {
            WireProtocolFatalAt("EncodeRecord.tailCount", tailCount, layout.TailCount);
        }
        for (Uint32 i = 0; i < layout.TailCount; ++i) {
            const Uint64 supplied = i < tailCount ? tails[i].Size : 0;
            if (supplied != layout.TailBytes[i]) {
                WireProtocolFatalAt("EncodeRecord.tailBytes", supplied, layout.TailBytes[i]);
            }
            if (supplied != 0 && tails[i].Bytes == nullptr) {
                WireProtocolFatal("EncodeRecord.tail", "non-zero tail length with a null pointer");
            }
        }

        if (op == MGPWireOp::DrawVbo && layout.SecondTailIsHostSpans) {
            MGHostSpan span{};
            std::memcpy(&span, tails[1].Bytes, sizeof(span));
            const auto& info = *static_cast<const MGPDrawInfo*>(payload);
            CheckDrawUserIndices(info, static_cast<const MGPDrawRange*>(tails[0].Bytes), span);
        }

        const Uint64 total = layout.TotalBytes;
        if (total > m_cmd->MaxRecordBytes()) {
            // R-10: P5 does no chunking and must instead PROVE it never needs any. This is
            // where the proof fails loudly if it was wrong.
            MGLOG_F("MGPipe: Fatal{RingOverrun, \"%s\"} a %llu byte record exceeds "
                    "RingProducer::MaxRecordBytes() == %llu (half of a %llu byte SEG_CMD); P5 "
                    "does not chunk (R-10) - report it to the integrator, who decides between "
                    "early chunking and a bigger default ring",
                    WireOpName(op), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(m_cmd->MaxRecordBytes()),
                    static_cast<unsigned long long>(m_cmd->Capacity()));
            std::abort();
        }

        // MGPipeCallFlags -> RingRecordFlags. See the file header: these are two flag spaces
        // in one 16-bit field and stamping the wrong one loses whole opcodes silently.
        const Uint32 callFlags = MGPipeCallFlagsFor(op);
        Uint16 ringFlags = Transport::kRecNone;
        if ((callFlags & static_cast<Uint32>(kNeedsAck)) != 0) ringFlags |= Transport::kRecNeedsAck;
        if ((callFlags & static_cast<Uint32>(kHasBlob)) != 0) ringFlags |= Transport::kRecHasBlob;
        if ((callFlags & static_cast<Uint32>(kVarTail)) != 0) ringFlags |= Transport::kRecVarTail;

        // BOTH WRAP READINGS ARE TAKEN FROM THE PRODUCER'S OWN CURSOR, not from a flag Reserve
        // does not return. The cursor is a monotonic byte count and the ring is indexed
        // `cursor & mask`, so `cursor / capacity` is the number of times the byte area has been
        // reused - and Reserve advances the cursor by `total` for a contiguous record and by
        // `spaceToEnd + total` when it had to lay a kRecPad filler down to the boundary first
        // (Ring.cpp). Everything below is exact arithmetic over those two facts, which keeps
        // both counters in the encoder - where R-10's maximum already lives - rather than
        // adding members to a Transport class the wire package does not own.
        //
        // AND THEY ARE TWO COUNTERS BECAUSE THEY ARE TWO EVENTS, which one measurement made
        // unmissable: a workload whose records repeat at a uniform stride that DIVIDES the
        // capacity lands on the boundary exactly, every time, for ever. Driving 1310824 bytes
        // of clears and draws through a 1 MiB SEG_CMD produced ZERO pads - the ring went round
        // once and a half and never straddled - so "did a pad happen" is NOT the question "did
        // this ring wrap", and a lane that asked the first one while meaning the second would
        // have gone red for a property of its own arithmetic.
        //
        //   m_cmdWraps      the head crossed a multiple of the capacity: the ring went round.
        //                   Guaranteed once more bytes are written than the ring holds, which
        //                   is what makes it something exit gate E3(e) can ASSERT.
        //   m_cmdWrapPads   a kRecPad filler was laid because a record would have straddled
        //                   the boundary. R-9's "a pad does not advance seq, both sides skip
        //                   it and count again" is about THIS one, and it is RECORDED rather
        //                   than asserted, because whether it ever happens is a property of
        //                   the record sizes and not of the ring.
        const Uint64 headBeforeReserve = m_cmd->LocalHead();
        void* slot = m_cmd->Reserve(static_cast<Uint16>(op), ringFlags,
                                    total - sizeof(MGPWireRecHeader));
        if (slot != nullptr) {
            const Uint64 headAfterReserve = m_cmd->LocalHead();
            if ((headAfterReserve - headBeforeReserve) > total) {
                ++m_cmdWrapPads;
            }
            const Uint64 capacity = m_cmd->Capacity();
            if (capacity != 0) {
                // A record is capped at capacity/2 and its pad at capacity/2 too, so one
                // Reserve can cross at most one boundary; the subtraction is still written as
                // a difference of quotients rather than as a Bool, because that stays correct
                // if the cap ever changes.
                m_cmdWraps += (headAfterReserve / capacity) - (headBeforeReserve / capacity);
            }
        }
        if (slot == nullptr) {
            // The ring is full, not the record too big - Reserve refuses an oversized record
            // above, and we already proved this one is not. The caller publishes, waits for
            // the apply side and retries.
            return kInvalidSeq;
        }

        auto* bytes = static_cast<Uint8*>(slot);
        std::memcpy(bytes, payload, static_cast<SizeT>(payloadBytes));
        // Reserve does not zero the alignment padding it hands back, and these bytes leave the
        // process under spawn: a record must be a function of what it declares, not of
        // whatever the client's ring last held.
        const Uint64 payloadSlack = layout.TailCount > 0
                                        ? layout.TailOffset[0] - sizeof(MGPWireRecHeader) - payloadBytes
                                        : total - sizeof(MGPWireRecHeader) - payloadBytes;
        if (payloadSlack != 0) {
            std::memset(bytes + payloadBytes, 0, static_cast<SizeT>(payloadSlack));
        }
        Uint64 written = layout.TailCount > 0 ? layout.TailOffset[0] - sizeof(MGPWireRecHeader)
                                              : total - sizeof(MGPWireRecHeader);
        for (Uint32 i = 0; i < tailCount; ++i) {
            const Uint64 at = layout.TailOffset[i] - sizeof(MGPWireRecHeader);
            if (at > written) {
                std::memset(bytes + written, 0, static_cast<SizeT>(at - written));
            }
            if (tails[i].Size != 0) {
                std::memcpy(bytes + at, tails[i].Bytes, static_cast<SizeT>(tails[i].Size));
            }
            written = at + tails[i].Size;
        }
        const Uint64 recordBody = total - sizeof(MGPWireRecHeader);
        if (written < recordBody) {
            std::memset(bytes + written, 0, static_cast<SizeT>(recordBody - written));
        }

        // R-2 arms 1 and 3, on the PRODUCING side, over the bytes actually written. Doing it
        // here rather than only in the decoder is what makes `inproc` worth running: the
        // encoder is the half that can still be wrong in a way the decoder would never see,
        // because under inproc a host pointer resolves.
        if (m_segments != nullptr) {
            const BlobSlots slots = BlobSlotsFor(op);
            for (Uint32 i = 0; i < slots.Count; ++i) {
                MGPBlobRef blob{};
                std::memcpy(&blob, bytes + slots.Offset + i * sizeof(MGPBlobRef), sizeof(MGPBlobRef));
                CheckBlobIsHonest(op, blob, *m_segments);
                // THE ENCODER MUST NOT ACCEPT A RECORD THE DECODER FATALS ON. w1's ruling that
                // CreateShaderState's modules travel inside the Reflection archive lived only
                // in the decoder, so an emitter that declared a per-stage run got a valid seq
                // here and a Fatal on a peer - exactly the asymmetry EncodeRecord's own
                // comment says it exists to prevent.
                if (op == MGPWireOp::CreateShaderState && i < 6 && blob.Size != 0) {
                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"CreateShaderState.Spirv[%u]\"} "
                            "declares %llu bytes at the ENCODER; under split the modules travel "
                            "inside the Reflection archive and the six per-stage runs stay "
                            "undeclared",
                            static_cast<unsigned>(i),
                            static_cast<unsigned long long>(blob.Size));
                    std::abort();
                }
            }
        }
        if ((callFlags & static_cast<Uint32>(kHostSpan)) != 0 && layout.TailCount == 2 &&
            layout.SecondTailIsHostSpans && layout.TailBytes[1] != 0 && m_segments != nullptr) {
            const Uint64 at = layout.TailOffset[1] - sizeof(MGPWireRecHeader);
            const Uint64 spans = layout.TailBytes[1] / sizeof(MGHostSpan);
            for (Uint64 i = 0; i < spans; ++i) {
                MGHostSpan span{};
                std::memcpy(&span, bytes + at + i * sizeof(MGHostSpan), sizeof(MGHostSpan));
                // All four arms, including the one that needs the table: a producer that
                // computed an offset wrongly is caught here rather than on a peer.
                CheckHostSpanIsHonest(span, *m_segments);
            }
        }

        if (total > m_maxRecordBytes) {
            m_maxRecordBytes = total;
            // WHICH ROW IT WAS, not just how big. R-10 makes the integrator choose between early
            // chunking and a bigger default ring when the maximum climbs, and that choice is
            // about a specific record family - a var-tail whose length the GL limits bound, or
            // one the emitter has to split itself. A number with no row attached leaves the
            // reader to guess which, and the guess in c1-v2.md §10 was SetGlobalConstants while
            // the measured answer on the reduced path is a different row entirely.
            m_maxRecordOp = op;
        }
        ++m_emitSeq;
        // The stage mark: where SEG_STAGE stood once everything this record names had been
        // staged. ReclaimStagedBytes releases up to the newest mark the server has retired.
        m_stageMarks.push_back(StageMark{m_emitSeq, m_stageHead});
        return m_emitSeq;
    }

    // THE EMPTY-STAGE REBASE. Head and tail are monotonic byte counts, so "every staged byte
    // has retired" reads head == tail, NOT head == tail == 0, and `head % capacity` is left
    // wherever the last run ended. Charging a wrap skip against that offset then costs the
    // unused suffix a second time: with head == tail == 64 in a 256 KiB stage, the allocator's
    // test became `2*capacity - 64 <= capacity`, which is false at EVERY occupancy, so a blob
    // that fits the segment whole was refused with `Fatal{RingOverrun, "SEG_STAGE"}` - whose
    // own message then reported `0 bytes still in flight`. ReclaimStagedBytes cannot help,
    // because an already-empty tail has nothing left to move.
    //
    // THE MARK QUEUE COMES WITH IT. A mark holds the ABSOLUTE head cursor it was pushed at and
    // ReclaimStagedBytes assigns that value straight to m_stageTail. Every mark not yet
    // consumed has StageCursor <= head == tail - the head is monotonic and marks are pushed in
    // order - so each of them names a region that is already reclaimed and zero is the
    // truthful rebasing of it. Without that, one reclaim after a rebase would put the tail
    // AHEAD of the head and StagedBytesInFlight() would underflow to about 2^64.
    void PipeWireEncoder::RebaseEmptyStage() {
        if (m_stageHead != m_stageTail || m_stageHead == 0) {
            return;
        }
        m_stageHead = 0;
        m_stageTail = 0;
        for (SizeT i = 0; i < m_stageMarks.size(); ++i) {
            m_stageMarks[i].StageCursor = 0;
        }
    }

    void PipeWireEncoder::ReclaimStagedBytes() {
        if (m_control == nullptr) {
            return;
        }
        const Uint64 retired = m_control->retiredSeq.load(std::memory_order_acquire);
        Uint64 upTo = m_stageTail;
        while (m_stageMarkFront < m_stageMarks.size() &&
               m_stageMarks[m_stageMarkFront].Seq <= retired) {
            upTo = m_stageMarks[m_stageMarkFront].StageCursor;
            ++m_stageMarkFront;
        }

        // COMPACT ON A THRESHOLD, NOT ONLY ON A FULL DRAIN. The queue used to be cleared only
        // when front reached size(), so any pipelining deeper than "fully drained at every
        // reclaim" - which is precisely the regime this design exists to survive when R-1's
        // barrier retires family by family - left front < size() for ever and grew the vector
        // 16 bytes per encoded record for the life of the context. Erasing the consumed prefix
        // once it is half the queue is amortised O(1) and bounds the storage at twice the
        // records actually in flight.
        if (m_stageMarkFront == m_stageMarks.size()) {
            m_stageMarks.clear();
            m_stageMarkFront = 0;
        } else if (m_stageMarkFront != 0 && m_stageMarkFront * 2 >= m_stageMarks.size()) {
            m_stageMarks.erase(m_stageMarks.begin(),
                               m_stageMarks.begin() + static_cast<std::ptrdiff_t>(m_stageMarkFront));
            m_stageMarkFront = 0;
        }

        // SEG_STAGE is CLIENT-OWNED memory (contract table 1: "client stages, server copies")
        // and the server only reads it, so the client is both the allocator and the thing that
        // frees. What it may not do is free ahead of retiredSeq - the whole content of R-11 on
        // this side - and what it may ALSO not do is write RingControl's stage tails, which
        // Ring.h makes consumer-owned. So the reclaim watermark is this local counter and the
        // shared triple is untouched.
        if (upTo > m_stageTail) {
            m_stageTail = upTo;
        }
    }

    Uint64 PipeWireEncoder::StagedBytesInFlight() const { return m_stageHead - m_stageTail; }

    // THE STORAGE, NOT THE LIVE COUNT. `size() - front` is the number of marks still in
    // flight, and it stays at one or two even while the vector behind it grows for ever - so a
    // control written against it would have gone green through exactly the leak it was meant
    // to catch. What leaks is the container, so that is what this reports.
    SizeT PipeWireEncoder::StageMarksHeld() const { return m_stageMarks.size(); }

    void PipeWireEncoder::Publish() {
        if (m_cmd == nullptr) {
            return;
        }
        // Publish then notify. The order is pinned by RingTest and must not be swapped:
        // notify-then-publish loses the wakeup. The doorbell itself belongs to the SESSION
        // (s1) - the codec does not own a Doorbell and must not, or a unit case could not
        // drive encoder -> ring -> decoder without one.
        //
        // SEG_STAGE needs no publish: the decoder reads those bytes by OFFSET, never by
        // popping a ring, and the release store on SEG_CMD's head below is what orders the
        // staged writes before the record that names them.
        m_cmd->Publish();
        if (m_control != nullptr) {
            // submittedSeq is the one watermark the PRODUCER owns (Ring.h's five-watermark
            // block). Nobody waits on it; it answers "how far ahead of the server is the
            // client right now".
            m_control->submittedSeq.store(m_emitSeq, std::memory_order_release);
        }
        // The reclaim has a trigger in this package, rather than depending on a c1 barrier
        // that does not exist yet: every publish is a chance to notice what the server has
        // already retired, and it costs one acquire load.
        ReclaimStagedBytes();
    }

    Uint64 PipeWireEncoder::EmitSeq() const { return m_emitSeq; }

    Uint64 PipeWireEncoder::MaxRecordBytesSeen() const { return m_maxRecordBytes; }

    const char* PipeWireEncoder::MaxRecordOpName() const {
        return m_maxRecordOp == MG_Pipe::MGPWireOp::kOpCount ? "none" : WireOpName(m_maxRecordOp);
    }

    Uint64 PipeWireEncoder::MaxRecordBytesCap() const {
        // Read from the ring, not recomputed from MOBILEGL_IPC_RING_MB: the number the proof
        // has to hold against is the capacity this process's producer actually got, and the
        // two differ the moment a session clamps or rounds the configured size.
        return (m_cmd != nullptr && m_cmd->Valid()) ? m_cmd->MaxRecordBytes() : 0;
    }

    Uint64 PipeWireEncoder::CmdWraps() const { return m_cmdWraps; }

    Uint64 PipeWireEncoder::CmdWrapPads() const { return m_cmdWrapPads; }

    Uint64 PipeWireEncoder::StageReclaimWaits() const { return m_stageReclaimWaits; }

    Uint64 PipeWireEncoder::CmdBytesWritten() const {
        // LocalHead(), not RingControl::head: the producer's own cursor includes records
        // reserved but not yet published, and this number is about what the PRODUCER wrote.
        return (m_cmd != nullptr && m_cmd->Valid()) ? m_cmd->LocalHead() : 0;
    }

    // ---------------------------------------------------------------------------------
    // Decoder
    // ---------------------------------------------------------------------------------

    PipeWireDecoder::PipeWireDecoder(Transport::RingControl* control, SegmentTable* segments,
                                     ReplySink* replies)
        : m_control(control), m_segments(segments), m_replies(replies) {
        m_auditPoison = MG_Config::Ipc.Audit;
        // Once, at construction, beside the resolver it is modelled on - not per record from
        // the apply thread. The thunk is inert without a decoder on the calling thread, so an
        // early install changes nothing for a monolith caller of MGPipeApplyWireRecord.
        InstallApplyHook();
    }

    void PipeWireDecoder::InstallApplyHook() {
        MG_Pipe::gMGPipeWireRecordApply = &MGPipeWireRecordApplyThunk;
    }

    void PipeWireDecoder::UninstallApplyHook() { MG_Pipe::gMGPipeWireRecordApply = nullptr; }

    Bool PipeWireDecoder::Valid() const { return m_control != nullptr && m_segments != nullptr; }

    Uint64 PipeWireDecoder::AppliedSeq() const { return m_applySeq; }

    void PipeWireDecoder::SetVerbSink(WireVerbSink* sink) { m_verbs = sink; }

    WireVerbSink* PipeWireDecoder::VerbSink() const { return m_verbs; }

    void PipeWireDecoder::SetAuditPoison(Bool enabled) { m_auditPoison = enabled; }

    Bool PipeWireDecoder::AuditPoison() const { return m_auditPoison; }

    Uint64 PipeWireDecoder::PoisonedStageBytes() const { return m_poisonedBytes; }

    Bool PipeWireDecoder::LastAcceptanceKnown() const { return m_lastAcceptanceKnown; }

    Bool PipeWireDecoder::LastAcceptance() const { return m_lastAcceptance; }

    Uint64 PipeWireDecoder::AcceptedRecords() const { return m_accepted; }

    Uint64 PipeWireDecoder::DeclinedRecords() const { return m_declined; }

    void PipeWireDecoder::PostReply(MGPWireOp op, Uint64 seq, Int32 status, const void* bytes,
                                    Uint64 size) {
        // THE ONE GATE ON SEG_REPLY. s1 sizes ReplyPool from MGPipeCallFlagsFor - table 0 says
        // that table is what "every package" reads - so writing SEG_REPLY[seq % slots] for a
        // record the pool reserved no slot for silently overwrites a waiter's answer. And
        // because the slot header stamps the WRITER's seq for self-check, the waiter's check
        // then fails for ever: the barrier HANGS rather than returning something wrong, which
        // is the harder failure to diagnose of the two.
        if ((MGPipeCallFlagsFor(op) & static_cast<Uint32>(kReplySlot)) == 0) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} the decoder tried to answer into "
                    "a reply slot for a call the catalogue gives no kReplySlot; s1 sizes "
                    "ReplyPool from MGPipeCallFlagsFor and reserved none",
                    WireOpName(op));
            std::abort();
        }
        if (m_replies != nullptr) {
            m_replies->PostReply(seq, status, bytes, size);
        }
    }

    Bool MGPipeWireRecordApplyThunk(MGPWireOp op, const void* record, Uint64 size, Uint64 remaining) {
        (void)remaining;
        if (t_activeDecoder == nullptr) {
            return false;
        }
        return t_activeDecoder->ApplyChecked(op, record, size);
    }

    void PipeWireDecoder::NoteResolvedRun(MGPWireOp op, const MGPBlobRef& blob) {
        // UNREACHABLE NOW, AND LOUD RATHER THAN SILENT. This used to `return`, and that made
        // the audit's bookkeeping quietly optional: a record naming a non-SEG_STAGE carrier
        // was applied AND recorded nothing, so PoisonedStageBytes() stayed zero and rule C's
        // only mechanical control was dark on exactly the record it existed to catch. The one
        // caller is ResolveOrFatal, which runs RequireDeclaredBlob first, and that now refuses
        // both an undeclared blob and a non-SEG_STAGE one by name. If either ever arrives here
        // the audit has stopped covering the carrier, which is the same failure as no audit at
        // all - the reason the run-count overflow just below is a Fatal too.
        if (blob.Size == 0 || blob.Seg != kSegStage) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} the audit was asked to record a "
                    "resolved run with seg=%u size=%llu; only declared SEG_STAGE(%u) runs "
                    "reach the poison fill (R-2.5)",
                    WireOpName(op), static_cast<unsigned>(blob.Seg),
                    static_cast<unsigned long long>(blob.Size),
                    static_cast<unsigned>(kSegStage));
            std::abort();
        }
        if (m_resolvedCount >= sizeof(m_resolved) / sizeof(m_resolved[0])) {
            // LOUD, NOT A SILENT DROP. This array is what the 0xDD fill covers, and a poison
            // that quietly stopped covering a run is the same failure as no poison at all -
            // rule C's only mechanical control going dark without a line in the log.
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} more than %llu SEG_STAGE runs in "
                    "one record; the audit fill would stop covering them",
                    WireOpName(op),
                    static_cast<unsigned long long>(sizeof(m_resolved) / sizeof(m_resolved[0])));
            std::abort();
        }
        m_resolved[m_resolvedCount++] = blob;
    }

    const void* PipeWireDecoder::ResolveOrFatal(MGPWireOp op, const MGPBlobRef& blob) {
        RequireDeclaredBlob(op, blob, *m_segments);
        const void* bytes = m_segments->Resolve(blob.Seg, blob.Offset, blob.Size);
        if (bytes == nullptr) {
            // RequireDeclaredBlob already ran the same arithmetic, so reaching here means the
            // table moved under us rather than that the record is wrong. Same Fatal either
            // way: there is no recovery from a segment that stopped covering its own runs.
            WireProtocolFatalAt("segment-resolve", blob.Offset, blob.Size);
        }
        NoteResolvedRun(op, blob);
        return bytes;
    }

    void PipeWireDecoder::PoisonResolvedRuns() {
        if (!m_auditPoison) {
            m_resolvedCount = 0;
            return;
        }
        for (Uint32 i = 0; i < m_resolvedCount; ++i) {
            const MGPBlobRef& blob = m_resolved[i];
            const SegmentView view = m_segments->Get(kSegStage);
            // The SUBTRACTION form, the same one Resolve uses, so the addition cannot wrap.
            // Every run here has already been through Resolve, so this is unreachable today -
            // but it is the one place in the file where a wrap would be an arbitrary 0xDD
            // memset, and "unreachable" is not a reason to write the weaker test.
            if (view.Base == nullptr || blob.Offset > view.Size ||
                blob.Size > view.Size - blob.Offset) {
                continue;
            }
            // R-2.5. The record has been applied and its bytes are retired, so an applier that
            // kept the pointer reads 0xDD next frame instead of bytes that merely happen to
            // still be there. Exactly the runs this record resolved, not a conservative window.
            std::memset(static_cast<Uint8*>(view.Base) + blob.Offset, 0xDD,
                        static_cast<SizeT>(blob.Size));
            m_poisonedBytes += blob.Size;
        }
        m_resolvedCount = 0;
    }

    Bool PipeWireDecoder::DecodeAndApply(const Transport::RingRecordView& record) {
        if (!Valid()) {
            WireProtocolFatal("PipeWireDecoder::DecodeAndApply", "no control page or segment table");
        }
        if ((record.flags & Transport::kRecPad) != 0 ||
            record.kind == Transport::kRingPadRecordKind) {
            // R-9: a pad does not advance seq, and both sides skip it BEFORE counting.
            // RingConsumer::Pop already does; one that reached here has been counted, and a
            // seq that drifted by one silently reads another call's reply slot.
            WireProtocolFatal("PipeWireDecoder::DecodeAndApply",
                              "a kRecPad wrap filler reached the decoder; the caller skips pads "
                              "before counting (R-9)");
        }

        const auto* base = static_cast<const Uint8*>(record.payload) - sizeof(MGPWireRecHeader);
        const Uint64 size = record.payloadSize + sizeof(MGPWireRecHeader);
        const MGPWireOp op = static_cast<MGPWireOp>(record.kind);

        m_resolvedCount = 0;
        m_lastAcceptanceKnown = false;
        PipeWireDecoder* previous = t_activeDecoder;
        t_activeDecoder = this;
        // The generated gate first, ALWAYS: MGPipeApplyWireRecord owns the per-opcode
        // `size >= sizeof(MGPWireRec_X)` check, because that is the half that follows from the
        // opcode alone and therefore belongs to the generator. It then calls back into
        // ApplyChecked through the hook, which owns the half that needs the payload. The hook
        // was installed once, at construction.
        const Bool applied = MG_Pipe::MGPipeApplyWireRecord(op, base, size, size);
        t_activeDecoder = previous;

        PoisonResolvedRuns();

        // THE DECODER'S OWN TALLY, AND NOT THE SHARED WATERMARK. RingControl::appliedSeq has
        // exactly one writer - s1's SessionConsumer::ApplyOne, +1 per record, pads never
        // counted - and this class does not write RingControl at all. Keeping a private count
        // beside it is what makes R-9's batching ban CHECKABLE rather than merely stated: the
        // session's watermark and this number must agree after every record, and a test that
        // compares them catches a batched publish that a single counter could not.
        ++m_applySeq;
        return applied;
    }

    // ---------------------------------------------------------------------------------
    // The 71 arms
    // ---------------------------------------------------------------------------------
    //
    // EVERY ARM ENDS IN AN EXISTING MGPipeApply* FREE FUNCTION, in the verb sink, or in
    // `false`. None of them interprets a field: the packed MGPSubData::Target, the
    // MGPImageView::Access encoding and MGPFramebufferState::DrawBuffers' -1 token are all
    // read by the applier that already owns them, which is why table 0 could say "a decoder
    // that open-codes it is the class-1 defect" and this file could obey.
    //
    // `false` means "this build deliberately does not implement it" and is the answer for the
    // 30-odd rows off P5's reduced path (BRIEF §4's exclusion list: the fence family, the
    // query family, compute, XFB, indirect, copy-region, GetTextureImage, GenerateMipmap,
    // SetShaderBuffers, SetStreamOutputTargets, ResourceSubDataComplete). They are NOT
    // unchecked: every one of them runs the same bounds gate, the same tail cross-check and
    // the same blob honesty pass before it declines, so the phase that implements one inherits
    // a validated record rather than a validation problem.
    Bool PipeWireDecoder::ApplyChecked(MGPWireOp op, const void* record, Uint64 size) {
        const auto* base = static_cast<const Uint8*>(record);
        const void* payload = base + sizeof(MGPWireRecHeader);

        WireRecordLayout layout{};
        if (!MGPipeWireRecordLayout(op, payload, layout)) {
            WireProtocolFatalAt("opcode", static_cast<Uint64>(op),
                                static_cast<Uint64>(MGPWireOp::kOpCount));
        }
        // THE TAIL CROSS-CHECK (BRIEF §5 w1). The generated gate proved `size >= sizeof(the
        // record type)`; it CANNOT SEE THE TAIL, so this is the first thing that holds a
        // record declaring Count = 4000 while carrying 8 bytes to its own arithmetic.
        if (size != layout.TotalBytes) {
            MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"%s\"} the record declares Size=%llu and "
                    "its own count fields describe %llu bytes (fixed payload %llu + tails "
                    "%llu/%llu/%llu)",
                    WireOpName(op), static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(layout.TotalBytes),
                    static_cast<unsigned long long>(layout.PayloadBytes),
                    static_cast<unsigned long long>(layout.TailBytes[0]),
                    static_cast<unsigned long long>(layout.TailBytes[1]),
                    static_cast<unsigned long long>(layout.TailBytes[2]));
            std::abort();
        }

        // The record's own ordinal, which is its reply-slot id (R-3). 1-based, and m_applySeq
        // only moves after the applier returns.
        const Uint64 seq = m_applySeq + 1;

        const auto tailAt = [&](Uint32 i) -> const Uint8* {
            return layout.TailBytes[i] != 0 ? base + layout.TailOffset[i] : nullptr;
        };
        // The four Bool-returning appliers answer ACCEPTANCE, not "applied" (R-5: the client
        // may not re-derive it, because an if-constexpr discard, a stale handle and a refused
        // record are all invisible from the call site).
        //
        // IT NOW RIDES THE REPLY SLOT, AND THAT IS THE RULING THIS COMMENT ASKED FOR. The text
        // that stood here said the answer could not ride a slot "until the integrator rules on
        // which half of the contract moves (table 0 says these four use DECLINED; the
        // catalogue gives them no slot)". Both halves have since moved the same way: ID-31
        // gave `ResourceCreate` kReplySlot and `kMGPipeCallFlags` now carries the flag on all
        // four (PipeWire.inc rows 2, 3, 47, 48), and P5 ruling R-17 states that the acceptance
        // answers "come back through the reply slot inside the barrier's wait". So the
        // conflict is resolved in favour of table 0, the pool reserves these seqs like any
        // other, and `PostReply`'s own Fatal - which trips on a row with no kReplySlot - is
        // what keeps this honest if a flag is ever taken away again.
        //
        // WITHOUT THIS THE CLIENT CANNOT RUN AT ALL: `ClientSession::EmitAndWait` asks the
        // catalogue, not the caller, whether a row owns a slot, so all four would wait for an
        // answer nobody wrote and take `Fatal{ReplyMissing}` inside the barrier. Recording the
        // acceptance locally as well is kept, because `LastAcceptance()` and the two counters
        // are what the monolith-side decoder cases are asserted on.
        //
        // OWNERSHIP: MG_Remote/Wire/* is package w1's and w1 is not in wave 2. This hunk is
        // four lines inside one lambda, made under R-17 by c1 because it is the server half of
        // the routing R-17 assigns, and it is called out in c1-v2.md so the integrator can
        // move it if the call belongs elsewhere.
        const auto noteAcceptance = [&](Bool accepted) {
            m_lastAcceptanceKnown = true;
            m_lastAcceptance = accepted;
            if (accepted) {
                ++m_accepted;
            } else {
                ++m_declined;
            }
            // DECLINED is a real answer and carries no payload (ReplySlot.h): the four Bool
            // rows say `false` with it, exactly as MapPersistent says nullptr with it.
            PostReply(op, seq, accepted ? ReplySink::kStatusOk : ReplySink::kStatusDeclined,
                      nullptr, 0);
        };

        switch (op) {

        // ---- screen ---------------------------------------------------------------------
        case MGPWireOp::GetCaps:
            // The SESSION answers this one: the snapshot is built from the server's live
            // backend and posted through ServerSession::PublishCapsSnapshot (s1/v1), and the
            // two blob serializers it needs are MG_Remote/CapsCodec's. The codec validates the
            // record and declines, rather than inventing a caps source of its own.
            for (Uint32 i = 0; i < 2; ++i) {
                MGPBlobRef blob{};
                std::memcpy(&blob,
                            base + sizeof(MGPWireRecHeader) + offsetof(MGPCaps, FormatCapabilities) +
                                i * sizeof(MGPBlobRef),
                            sizeof(MGPBlobRef));
                CheckBlobIsHonest(op, blob, *m_segments);
            }
            return false;

        // ---- resources -------------------------------------------------------------------
        case MGPWireOp::ResourceCreate:
            noteAcceptance(MGPipeApplyResourceCreate(*static_cast<const MGPResourceDesc*>(payload)));
            return true;

        case MGPWireOp::ResourceRespecify: {
            const auto& desc = *static_cast<const MGPResourceDesc*>(payload);
            // R-13.3: `initialBytes` IS ALWAYS NULL UNDER SPLIT. Initial content arrives as
            // resource_subdata records immediately after this one, which is what the texture
            // path already does (TextureEmit.h:1137).
            //
            // The SCOPE is contract table 1 row 19b's carrier, read only through the helpers:
            // the presence byte and the pair are one value in three pieces, and an open-coded
            // reader that forgets the byte reads level 0 of upload target 0 as a real scope.
            MGPRespecifiedLevel level{};
            const MGPRespecifiedLevel* scope = nullptr;
            if (!MGPipeRespecifyIsWholeResource(desc)) {
                level.UploadTarget = MGPipeRespecifiedUploadTargetOf(desc);
                level.Level = MGPipeRespecifiedLevelOf(desc);
                scope = &level;
            }
            noteAcceptance(MGPipeApplyResourceRespecify(desc, nullptr, scope));
            return true;
        }

        case MGPWireOp::ResourceDestroy:
            MGPipeApplyResourceDestroy(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::MapPersistent:
            // R-6 / R-2.4: A CONSTANT DECLINE IN P5, and the applier is not called at all.
            // Two reasons, and the second is the one worth writing down: the answer is a HOST
            // POINTER, which cannot cross; and MGPWireRec_MapPersistent's payload is a bare
            // MGPHandleOnly, so the record carries NEITHER the `size` NOR the `seedBytes` the
            // entry point takes. The phase that lands a real map_persistent needs a payload
            // change (MGPipeTypes.h, c0's file) before it can even call the applier.
            //
            // DECLINED is a real answer, not a failure: the three frontend sites already
            // tolerate it (BufferObject.cpp:238, :603-606, :657-660).
            //
            // THE TIER IS CONSULTED HERE, AND IT IS THE SAME CONJUNCTION THE MONOLITH APPLIER
            // USES (PipeApply.cpp's `Transport != Monolith && AdoptTierIsEmulate()`). The arm
            // used to decline UNCONDITIONALLY and AdoptTier had no reference anywhere on the
            // codec path, so MOBILEGL_IPC_ADOPT_TIER=0 and =1 - which contract §5 promises
            // "parse and are Fatal at use, naming P11" - decoded as an ordinary DECLINED and
            // the operator got a run that looked like a working T0. AdoptTierIsEmulate returns
            // true at T2 and ABORTS at T0/T1 on its own named diagnostic, so the return value
            // is deliberately not a branch: P5 declines at every tier it survives (R-6), and
            // the two forbidden ones never get this far.
            if (MG_Config::Transport != MG_Config::TransportMode::Monolith) {
                (void)MG_Remote::Client::AdoptTierIsEmulate();
            }
            PostReply(op, seq, ReplySink::kStatusDeclined, nullptr, 0);
            return true;

        case MGPWireOp::UnmapPersistent:
            MGPipeApplyUnmapPersistent(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::FenceCreate:
            return m_verbs != nullptr && m_verbs->OnFenceCreate(*static_cast<const MGPHandleOnly*>(payload));
        case MGPWireOp::FenceDestroy:
            return m_verbs != nullptr && m_verbs->OnFenceDestroy(*static_cast<const MGPHandleOnly*>(payload));
        case MGPWireOp::FenceWaitServer:
            return m_verbs != nullptr && m_verbs->OnFenceWaitServer(*static_cast<const MGPFenceWait*>(payload));
        case MGPWireOp::FenceStatus:
        case MGPWireOp::FenceWait: {
            Uint32 result = 0;
            const Bool ok = m_verbs != nullptr &&
                (op == MGPWireOp::FenceStatus
                     ? m_verbs->OnFenceStatus(*static_cast<const MGPHandleOnly*>(payload), result)
                     : m_verbs->OnFenceWait(*static_cast<const MGPFenceWait*>(payload), result));
            PostReply(op, seq, ok ? ReplySink::kStatusOk : ReplySink::kStatusDeclined,
                      ok ? &result : nullptr, ok ? sizeof(result) : 0);
            return ok;
        }
        // Query migration follows the measured first blockers.
        case MGPWireOp::QueryCreate:
        case MGPWireOp::QueryBegin:
        case MGPWireOp::QueryEnd:
        case MGPWireOp::QueryAvailable:
        case MGPWireOp::QueryResult:
        case MGPWireOp::QueryDestroy:
        case MGPWireOp::QueryTimestamp:
        case MGPWireOp::QueryCounter:
            return false;

        // ---- CSOs ------------------------------------------------------------------------
        case MGPWireOp::CreateRenderState: {
            const auto& desc = *static_cast<const MGPRenderStateDesc*>(payload);
            const void* chunks = nullptr;
            if (desc.ChunkMask != 0) {
                // Contract table 1 row 1: nothing reads Blob.Size today, so the decoder is
                // where the record is held to it. The expected length is not a guess - the
                // chunk table computes it from the mask the record itself carries.
                const Uint64 expected =
                    static_cast<Uint64>(MGPipePipelineChunkBlobBytes(desc.ChunkMask));
                RequireDeclaredBlob(op, desc.Blob, *m_segments);
                if (desc.Blob.Size != expected) {
                    WireProtocolFatalAt("CreateRenderState.Blob", desc.Blob.Size, expected);
                }
                chunks = ResolveOrFatal(op, desc.Blob);
            } else {
                CheckBlobIsHonest(op, desc.Blob, *m_segments);
            }
            MGPipeApplyCreateRenderState(desc, chunks);
            return true;
        }

        case MGPWireOp::BindRenderState:
            MGPipeApplyBindRenderState(*static_cast<const MGPBindRenderState*>(payload));
            return true;

        case MGPWireOp::DeleteRenderState:
            MGPipeApplyDeleteRenderState(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::CreateVertexElements: {
            const auto& desc = *static_cast<const MGPVertexElements*>(payload);
            // The blob's length cross-check against the two counts is the applier's already
            // (PipeApply.cpp:1990-1999) and is the model every other row copies; the decoder
            // adds only what the applier cannot see, which is that the bytes exist at all.
            const void* blob = ResolveOrFatal(op, desc.Blob);
            MGPipeApplyCreateVertexElements(desc, blob);
            return true;
        }

        case MGPWireOp::BindVertexElements:
            MGPipeApplyBindVertexElements(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::DeleteVertexElements:
            MGPipeApplyDeleteVertexElements(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::CreateSamplerState: {
            const auto& desc = *static_cast<const MGPSamplerDesc*>(payload);
            // R-13.1 gave this call its kHasBlob. The serialization IS the memcpy - a POD -
            // and borderColorForm must survive BYTE FOR BYTE, because all three colour
            // representations are always numerically populated and it is the only thing that
            // says which one the backend must use (MGPipeTypes.h:425-428).
            RequireDeclaredBlob(op, desc.Parameters, *m_segments);
            if (desc.Parameters.Size != sizeof(MobileGL::SamplerParameters)) {
                WireProtocolFatalAt("CreateSamplerState.Parameters", desc.Parameters.Size,
                                    sizeof(MobileGL::SamplerParameters));
            }
            const void* bytes = ResolveOrFatal(op, desc.Parameters);
            // Copied into a local rather than reinterpret_cast in place: the staged run is
            // 8-aligned by the ring, but SamplerParameters is a frontend type and this file
            // may not assume its alignment requirement is one the ring happens to satisfy.
            MobileGL::SamplerParameters parameters{};
            std::memcpy(&parameters, bytes, sizeof(parameters));
            MGPipeApplyCreateSamplerState(desc, &parameters);
            return true;
        }

        case MGPWireOp::DeleteSamplerState:
            MGPipeApplyDeleteSamplerState(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::CreateSamplerView:
            MGPipeApplyCreateSamplerView(*static_cast<const MGPSamplerView*>(payload));
            return true;

        case MGPWireOp::DeleteSamplerView:
            MGPipeApplyDeleteSamplerView(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::CreateShaderState: {
            const auto& desc = *static_cast<const MGPProgramDesc*>(payload);
            // ONE ARCHIVE, NOT SEVEN RUNS - w1's ruling, and the cheapest one available.
            // EncodeProgramArtifacts already serialises SpirvArtifacts::generatedSpirv, i.e.
            // every stage module, so Reflection names the WHOLE archive and Spirv[0..5] stay
            // undeclared. Shipping the modules a second time would double the largest record
            // in the catalogue for no reader at all: MGPipeApplyCreateShaderState takes
            // (desc, link*, spirv*) and reads the modules out of spirv->generatedSpirv -
            // PipeApply.cpp touches neither desc.Spirv[] nor desc.Reflection.
            //
            // A DECLARED Spirv[i] IS FATAL rather than ignored, for resource_flush_range's
            // reason (contract §6.3): a second, forgeable way to say the same thing is worse
            // than no way at all. Overturn it by measuring that a per-stage run beats one
            // archive - and then the archive has to stop carrying generatedSpirv, in the same
            // change, or the two disagree.
            for (Uint32 i = 0; i < 6; ++i) {
                CheckBlobIsHonest(op, desc.Spirv[i], *m_segments);
                if (desc.Spirv[i].Size != 0) {
                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"CreateShaderState.Spirv[%u]\"} "
                            "declares %llu bytes; under split the modules travel inside the "
                            "Reflection archive and the six per-stage runs stay undeclared",
                            static_cast<unsigned>(i),
                            static_cast<unsigned long long>(desc.Spirv[i].Size));
                    std::abort();
                }
            }
            const void* archive = ResolveOrFatal(op, desc.Reflection);
            MG_State::GLState::LinkArtifacts link;
            MG_State::GLState::SpirvArtifacts spirv;
            if (!MG_State::GLState::DecodeProgramArtifacts(static_cast<const Uint8*>(archive),
                                                           static_cast<SizeT>(desc.Reflection.Size),
                                                           link, spirv)) {
                WireProtocolFatal("CreateShaderState.Reflection",
                                  "DecodeProgramArtifacts refused the archive - a truncated "
                                  "stream, a codec version mismatch or a struct-size mismatch");
            }
            MGPipeApplyCreateShaderState(desc, &link, &spirv);
            return true;
        }

        case MGPWireOp::BindShaderState:
            MGPipeApplyBindShaderState(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::DeleteShaderState:
            MGPipeApplyDeleteShaderState(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        // ---- working state ---------------------------------------------------------------
        case MGPWireOp::SetDynamicState: {
            const auto& dyn = *static_cast<const MGPDynamicState*>(payload);
            const void* chunks = nullptr;
            if (dyn.ChunkMask != 0) {
                const Uint64 expected =
                    static_cast<Uint64>(MGPipeDynamicChunkBlobBytes(dyn.ChunkMask));
                RequireDeclaredBlob(op, dyn.Blob, *m_segments);
                if (dyn.Blob.Size != expected) {
                    WireProtocolFatalAt("SetDynamicState.Blob", dyn.Blob.Size, expected);
                }
                chunks = ResolveOrFatal(op, dyn.Blob);
            } else {
                CheckBlobIsHonest(op, dyn.Blob, *m_segments);
            }
            MGPipeApplySetDynamicState(dyn, chunks);
            return true;
        }

        case MGPWireOp::SetFramebufferState:
            MGPipeApplySetFramebufferState(*static_cast<const MGPFramebufferState*>(payload));
            return true;

        case MGPWireOp::SetVertexBuffers:
            MGPipeApplySetVertexBuffers(*static_cast<const MGPVertexBuffers*>(payload),
                                        reinterpret_cast<const MGPVertexBuffer*>(tailAt(0)));
            return true;

        case MGPWireOp::SetIndexBuffer:
            MGPipeApplySetIndexBuffer(*static_cast<const MGPIndexBuffer*>(payload));
            return true;

        case MGPWireOp::SetIndirectBuffers:
            // Indirect is off the reduced path (BRIEF §4) and has no applier entry point.
            return false;

        case MGPWireOp::SetSamplerViews:
            MGPipeApplySetSamplerViews(*static_cast<const MGPSamplerViews*>(payload),
                                       reinterpret_cast<const MGPBoundView*>(tailAt(0)));
            return true;

        case MGPWireOp::BindSamplerStates:
            MGPipeApplyBindSamplerStates(*static_cast<const MGPSamplerStates*>(payload),
                                         reinterpret_cast<const MGPipeHandle*>(tailAt(0)));
            return true;

        case MGPWireOp::SetShaderImages:
            MGPipeApplySetShaderImages(*static_cast<const MGPShaderImages*>(payload),
                                       reinterpret_cast<const MGPImageView*>(tailAt(0)));
            return true;

        case MGPWireOp::SetShaderBuffers: {
            // NO APPLIER ENTRY POINT EXISTS, and P5 does not invent one: the call is on
            // BRIEF §4's exclusion list, so a consumer written here would be a semantics
            // nobody can test this phase. What the record needs and cannot get later is its
            // TAIL ARITHMETIC, which MGPipeWireRecordLayout above has already run - including
            // the HostSpanCount-is-0-or-Count rule, the thing that keeps the two arrays
            // index-aligned.
            //
            // kCapNeedsHostUboBytes is 0 for the whole of P5 (table 0), so the second tail is
            // always absent here; the honesty pass below is what says so out loud if it ever
            // is not.
            if (layout.TailCount == 2 && layout.TailBytes[1] != 0) {
                const auto* spans = reinterpret_cast<const MGHostSpan*>(tailAt(1));
                const Uint64 count = layout.TailBytes[1] / sizeof(MGHostSpan);
                for (Uint64 i = 0; i < count; ++i) {
                    MGHostSpan span{};
                    std::memcpy(&span, reinterpret_cast<const Uint8*>(spans) + i * sizeof(MGHostSpan),
                                sizeof(MGHostSpan));
                    // ALL FOUR ARMS, the segment-range one included: this row and DrawVbo are
                    // the only two host-span carriers in the catalogue, and an out-of-segment
                    // span used to pass every check here.
                    CheckHostSpanIsHonest(span, *m_segments);
                }
            }
            return false;
        }

        case MGPWireOp::SetStreamOutputTargets:
            // Same as above: no applier, off the reduced path, both tails validated.
            return false;

        case MGPWireOp::SetProgramBindings: {
            // P5e, opcode 80 (MG_Remote/CONTRACT-P5E.md §1). NO APPLIER ENTRY POINT EXISTS YET
            // and c0e does not invent one: the record's consumer is the program twin's
            // rebuild, which package pg writes. What the record needs NOW and cannot get later
            // is its shape - the three declared counts against their bounds and the three-tail
            // arithmetic, both of which MGPipeWireRecordLayout above has already run - plus the
            // honesty of every name span it carries.
            //
            // THE SPANS ARE WALKED HERE RATHER THAN BY THE ENCODER'S BLANKET PASS because they
            // are MEMBERS of the third tail's 40-byte elements, not a bare MGHostSpan array:
            // SecondTailIsHostSpans reads a tail AS spans, and doing that to {span, binding,
            // pad} would read one span and a quarter of garbage. All four arms of
            // CheckHostSpanIsHonest run, the segment-range one included, so an override name
            // pointing outside SEG_STAGE is caught on the way in rather than at the rebuild.
            if (layout.TailCount == 3 && layout.TailBytes[2] != 0 && m_segments != nullptr) {
                const auto* overrides = tailAt(2);
                const Uint64 count = layout.TailBytes[2] / sizeof(MGPProgramStorageOverride);
                for (Uint64 i = 0; i < count; ++i) {
                    MGPProgramStorageOverride entry{};
                    std::memcpy(&entry, overrides + i * sizeof(MGPProgramStorageOverride),
                                sizeof(MGPProgramStorageOverride));
                    CheckHostSpanIsHonest(entry.Name, *m_segments);
                }
            }
            return false;
        }

        case MGPWireOp::SetGlobalConstants: {
            const auto& rec = *static_cast<const MGPGlobalConstants*>(payload);
            // The length cross-check is the applier's (PipeApply.cpp:2784, against the
            // program's GlobalUboSize, which this record does not carry). Under rule A it
            // stops being inert for the first time, which is the whole point of arming Size.
            const void* bytes = ResolveOrFatal(op, rec.Blob);
            MGPipeApplySetGlobalConstants(rec, bytes);
            return true;
        }

        case MGPWireOp::SetVertexAttribDefaults:
            // Count == popcount(Mask) was enforced by the layout above: two declarants that
            // must agree, and nothing checked it before (contract table 1 row 15).
            MGPipeApplySetVertexAttribDefaults(*static_cast<const MGPVertexAttribDefaults*>(payload),
                                               reinterpret_cast<const MGPAttribValue*>(tailAt(0)));
            return true;

        case MGPWireOp::SetPixelPackState:
            MGPipeApplySetPixelPackState(*static_cast<const MGPPixelPackState*>(payload));
            return true;

        case MGPWireOp::SetPatchState:
            MGPipeApplySetPatchState(*static_cast<const MGPPatchState*>(payload));
            return true;

        case MGPWireOp::SetDrawProgram:
            MGPipeApplySetDrawProgram(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::SetDispatchProgram:
            MGPipeApplySetDispatchProgram(*static_cast<const MGPHandleOnly*>(payload));
            return true;

        case MGPWireOp::SetResidualValueState: {
            // THE HARDEST ROW IN TABLE 1, and it is hard for a reason that does not show in
            // the payload: MGPipeApplySetResidualValueState takes `const ResidualValueBlock&`
            // - not a payload, not a const void* - and MGPResidualValueState is NEVER
            // INSTANTIATED on the live path (PipeFill.cpp:2184 passes the block straight to
            // the applier). So the encoder had to invent both the record fill and the blob
            // fill, and this is the first code in the tree that reads either.
            //
            // The block is the blob, whole, and its size only ever ratchets DOWN
            // (MGL_RESIDUAL_BLOCK_SIZE, 1248 -> 8 at P2, 0 at P13). Requiring exact equality
            // rather than ">=" is what makes a client built against an older block a loud
            // mismatch instead of a silently short read of CapabilityBits.
            const auto& rec = *static_cast<const MGPResidualValueState*>(payload);
            RequireDeclaredBlob(op, rec.Blob, *m_segments);
            if (rec.Blob.Size != sizeof(ResidualValueBlock)) {
                WireProtocolFatalAt("SetResidualValueState.Blob", rec.Blob.Size,
                                    sizeof(ResidualValueBlock));
            }
            const void* bytes = ResolveOrFatal(op, rec.Blob);
            ResidualValueBlock block{};
            std::memcpy(&block, bytes, sizeof(block));
            MGPipeApplySetResidualValueState(block);
            return true;
        }

        case MGPWireOp::SetTextureParams:
            noteAcceptance(MGPipeApplySetTextureParams(*static_cast<const MGPTextureParams*>(payload)));
            return true;

        // ---- transfer ---------------------------------------------------------------------
        case MGPWireOp::ResourceSubData: {
            const auto& rec = *static_cast<const MGPSubData*>(payload);
            // Rule A arms both halves. The buffer half already declared a real size in
            // monolith and is cross-checked at PipeApply.cpp:702; THE TEXTURE HALF DECLARED 0
            // (TextureEmit.h:1265-1267, on the grounds that the byte count was "the server's
            // to compute") and under split it must declare too - a length the reader computes
            // from the record it is checking is not a bounds check.
            const Bool namesABuffer = rec.Target == kMGPipeResourceTargetBuffer;
            // THE TEXTURE PREDICATE, TIGHTENED. It used to be "all three extents non-zero",
            // which read a 4x4x0 box as carrying nothing and let rule A's arm 2 sit out - so a
            // record could describe a real destination and declare no bytes. A box is either
            // EMPTY (every extent zero, which is how a pull that needs nothing is spelled) or
            // WHOLE; a partially-zero extent is neither, and no emitter produces one.
            const Bool boxIsEmpty =
                rec.UnionBox.W == 0 && rec.UnionBox.H == 0 && rec.UnionBox.D == 0;
            const Bool boxIsWhole =
                rec.UnionBox.W != 0 && rec.UnionBox.H != 0 && rec.UnionBox.D != 0;
            if (!namesABuffer && !boxIsEmpty && !boxIsWhole) {
                MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"ResourceSubData\"} the union box "
                        "%ux%ux%u has a zero extent on some axes and not others; a box is "
                        "either empty or whole",
                        rec.UnionBox.W, rec.UnionBox.H, rec.UnionBox.D);
                std::abort();
            }
            const Bool carriesContent = namesABuffer ? MGPipeSubDataBufferSize(rec) != 0
                                                     : (boxIsWhole || rec.RegionCount != 0);
            const void* bytes = nullptr;
            if (carriesContent) {
                bytes = ResolveOrFatal(op, rec.Blob);
            } else {
                CheckBlobIsHonest(op, rec.Blob, *m_segments);
            }
            noteAcceptance(MGPipeApplyResourceSubData(
                rec, bytes, reinterpret_cast<const MGPSubRegion*>(tailAt(0))));
            return true;
        }

        case MGPWireOp::BufferSubDataResident: {
            const auto& rec = *static_cast<const MGPSubData*>(payload);
            // kOptional is a CAPABILITY question under split, not a null-pointer one: the
            // client gates on kCapResidentSubData through the caps mirror before it emits
            // (R-8). By the time a record is here, the answer was already yes.
            const void* bytes = MGPipeSubDataBufferSize(rec) != 0 ? ResolveOrFatal(op, rec.Blob)
                                                                  : nullptr;
            if (bytes == nullptr) {
                CheckBlobIsHonest(op, rec.Blob, *m_segments);
            }
            MGPipeApplyBufferSubDataResident(rec, bytes);
            return true;
        }

        case MGPWireOp::ResourceSubDataComplete:
            // The forward terminator of a SERVER-initiated texture pull (section 7.1). There
            // is no client producer and no applier; the reverse channel is P7/P9.
            return false;

        case MGPWireOp::ResourceFlushRange:
            // R-13.2 / contract §6.3: IT CARRIES NO BYTES AT ALL under split. The ladder it
            // drives rewrites its range from the AUTHORITATIVE SHADOW, which rule C makes
            // server-owned, so resource_subdata is already the only way bytes reach it and a
            // second way would be a forgeable one. AccessFlags cross verbatim, never
            // normalised (PipeApply.h:902-903).
            MGPipeApplyResourceFlushRange(*static_cast<const MGPFlushRange*>(payload), nullptr);
            return true;

        case MGPWireOp::ResourceReadback:
            // The BYTES go back in SEG_EVENT through OnBufferWriteback (contract table 1 row
            // 22) - the destination is the client's shadow and the size is the resource's, not
            // a fixed slot's - so the reply slot carries COMPLETION only.
            MGPipeApplyResourceReadback(*static_cast<const MGPReadback*>(payload));
            PostReply(op, seq, ReplySink::kStatusOk, nullptr, 0);
            return true;

        // P5b (CONTRACT-P5B.md): the two transfer verbs with no applier reach the sink like the
        // class-B five. resource_copy_region = glCopyImageSubData (i1), generate_mipmap =
        // glGenerateMipmap (f1). The payloads are plain PODs with no blob and no tail, so the
        // bounds gate above is the whole validation; what the sink does with a renderbuffer
        // endpoint or an emulation site is the contract's, not the codec's.
        case MGPWireOp::ResourceCopyRegion:
            return m_verbs != nullptr &&
                   m_verbs->OnResourceCopyRegion(*static_cast<const MGPCopyRegion*>(payload));

        case MGPWireOp::GenerateMipmap:
            return m_verbs != nullptr &&
                   m_verbs->OnGenerateMipmap(*static_cast<const MGPMipPlan*>(payload));

        case MGPWireOp::GetTextureImage:
            // Off the reduced path (BRIEF §4) and outside P5b's measured 25: its emit-table slot
            // is Fatal{UnmigratedVerb} on the client (contract §7 class C, wave 3 / P9).
            return false;

        // ---- the five class-B verbs: no MGPipeApply* exists, so v1's sink or nothing -------
        case MGPWireOp::Blit:
            return m_verbs != nullptr && m_verbs->OnBlit(*static_cast<const MGPBlit*>(payload));

        case MGPWireOp::Clear:
            return m_verbs != nullptr && m_verbs->OnClear(*static_cast<const MGPClear*>(payload));

        case MGPWireOp::ReadPixels:
            // P5 BLOCKS on read_pixels (ROADMAP.md:21) and the pixels come back in the reply
            // slot, which is why ReplyPool::SlotBytes() is sized from the scenario's largest
            // read rather than guessed. MGPReadbackInfo has DstOffset/DstSize and no Seg on
            // purpose: in P5 the destination is ALWAYS SEG_REPLY (contract table 1 row 23).
            return m_verbs != nullptr &&
                   m_verbs->OnReadPixels(*static_cast<const MGPReadbackInfo*>(payload), seq, m_replies);

        case MGPWireOp::DrawVbo: {
            const auto& info = *static_cast<const MGPDrawInfo*>(payload);
            // The conditional second tail. P5 must produce NONE of these - the reduced path
            // draws from a VBO precisely so kDrawHasUserIndices never fires (table 0's cap-bit
            // row) - so a span arriving here is a FINDING, not merely a corruption check, and
            // CheckHostSpanIsHonest is what makes it one.
            const MGHostSpan* userIndices = nullptr;
            MGHostSpan span{};
            if ((info.Flags & kDrawHasUserIndices) != 0) {
                if (layout.TailCount != 2 || layout.TailBytes[1] != sizeof(MGHostSpan)) {
                    WireProtocolFatalAt("DrawVbo.userIndices", layout.TailBytes[1],
                                        sizeof(MGHostSpan));
                }
                std::memcpy(&span, tailAt(1), sizeof(span));
                // ALL FOUR ARMS. WireVerbSink's header promises OnDrawVbo "a DECODED,
                // VALIDATED argument list"; without the segment-range arm a span whose run
                // left SEG_STAGE reached the sink and that promise was false. P5b's d1 is what
                // arms this path (client-side index arrays staged whole, CONTRACT-P5B.md d1).
                CheckHostSpanIsHonest(span, *m_segments);
                CheckDrawUserIndices(info, reinterpret_cast<const MGPDrawRange*>(tailAt(0)), span);
                userIndices = &span;
            }
            // P5b d1: the indirect block, in the span's place. The layout already refused a
            // record that sets both flags or that declares ranges alongside it.
            const MGPDrawIndirect* indirect = nullptr;
            MGPDrawIndirect indirectBlock{};
            if ((info.Flags & kDrawIsIndirect) != 0) {
                if (layout.TailCount != 2 || layout.TailBytes[1] != sizeof(MGPDrawIndirect)) {
                    WireProtocolFatalAt("DrawVbo.indirect", layout.TailBytes[1],
                                        sizeof(MGPDrawIndirect));
                }
                std::memcpy(&indirectBlock, tailAt(1), sizeof(indirectBlock));
                indirect = &indirectBlock;
            }
            return m_verbs != nullptr &&
                   m_verbs->OnDrawVbo(info, reinterpret_cast<const MGPDrawRange*>(tailAt(0)),
                                      userIndices, indirect);
        }

        case MGPWireOp::Present:
            return m_verbs != nullptr && m_verbs->OnPresent(*static_cast<const MGPPresent*>(payload));

        // ---- compute, barriers, XFB: P5b's i1 and t2 rows, to the sink -----------------------
        //
        // Six existing rows that returned false through P5 ("off the reduced path"). None has
        // an MGPipeApply* and none gains one (CONTRACT-P5B.md): the payloads are plain PODs the
        // bounds gate has already proved, so each arm hands over and stops.
        case MGPWireOp::LaunchGrid:
            return m_verbs != nullptr &&
                   m_verbs->OnLaunchGrid(*static_cast<const MGPGridInfo*>(payload));

        case MGPWireOp::MemoryBarrier:
            return m_verbs != nullptr &&
                   m_verbs->OnMemoryBarrier(*static_cast<const MGPMemoryBarrier*>(payload));

        case MGPWireOp::BeginStreamOutput:
            return m_verbs != nullptr &&
                   m_verbs->OnBeginStreamOutput(*static_cast<const MGPStreamOutputBegin*>(payload));

        case MGPWireOp::EndStreamOutput:
            return m_verbs != nullptr &&
                   m_verbs->OnEndStreamOutput(*static_cast<const MGPXfbAccounting*>(payload));

        case MGPWireOp::PauseStreamOutput:
            return m_verbs != nullptr &&
                   m_verbs->OnPauseStreamOutput(*static_cast<const MGPStreamOutputControl*>(payload));

        case MGPWireOp::ResumeStreamOutput:
            return m_verbs != nullptr &&
                   m_verbs->OnResumeStreamOutput(*static_cast<const MGPStreamOutputControl*>(payload));

        // ---- the five P5b-appended verbs, opcodes 72..76 ---------------------------------------
        case MGPWireOp::BindShaderImage:
            return m_verbs != nullptr &&
                   m_verbs->OnBindShaderImage(*static_cast<const MGPImageBind*>(payload));

        case MGPWireOp::PatchParameter:
            return m_verbs != nullptr &&
                   m_verbs->OnPatchParameter(*static_cast<const MGPPatchParameter*>(payload));

        case MGPWireOp::BindStreamOutput:
            return m_verbs != nullptr &&
                   m_verbs->OnBindStreamOutput(*static_cast<const MGPStreamOutputBind*>(payload));

        case MGPWireOp::SetStorageBlockBinding: {
            // The block name is the ONE string on the wire (CONTRACT-P5B.md i1): a kHasBlob
            // record whose blob is the NUL-terminated name, Size = strlen + 1, staged whole in
            // SEG_STAGE like every other client -> server blob. Copied into a bounded local
            // and re-terminated, so a record whose staged bytes forgot the NUL cannot make the
            // backend read past the run, and so the pointer the sink sees dies with this call
            // (rule C).
            const auto& rec = *static_cast<const MGPStorageBlockBinding*>(payload);
            constexpr Uint64 kMaxBlockNameBytes = 4096;
            RequireDeclaredBlob(op, rec.Name, *m_segments);
            if (rec.Name.Size > kMaxBlockNameBytes) {
                WireProtocolFatalAt("SetStorageBlockBinding.Name", rec.Name.Size, kMaxBlockNameBytes);
            }
            const void* bytes = ResolveOrFatal(op, rec.Name);
            char name[kMaxBlockNameBytes + 1];
            std::memcpy(name, bytes, static_cast<SizeT>(rec.Name.Size));
            name[rec.Name.Size] = '\0';
            if (name[rec.Name.Size - 1] != '\0') {
                WireProtocolFatal("SetStorageBlockBinding.Name",
                                  "the staged block name is not NUL-terminated; Size is strlen + 1");
            }
            return m_verbs != nullptr && m_verbs->OnSetStorageBlockBinding(rec, name);
        }

        case MGPWireOp::CopyFramebufferToTexture:
            return m_verbs != nullptr &&
                   m_verbs->OnCopyFramebufferToTexture(
                       *static_cast<const MGPCopyFromFramebuffer*>(payload));

        // ---- P5c's two control records, opcodes 77..78 (MG_Remote/CONTRACT-P5C.md §5) --------
        //
        // Fixed-size PODs the bounds gate has already proved; neither carries a blob, a tail
        // or a reply, so each arm hands over and stops. ObjectDeath's null-handle refusal is
        // the sink's, not the codec's: the codec proves SHAPES, and "the client emits nothing
        // for an object that never crossed" (§5.2) is a contract fact about the peer.
        case MGPWireOp::ApplierReset:
            return m_verbs != nullptr &&
                   m_verbs->OnApplierReset(*static_cast<const MGPApplierReset*>(payload));

        case MGPWireOp::ObjectDeath:
            return m_verbs != nullptr &&
                   m_verbs->OnObjectDeath(*static_cast<const MGPHandleOnly*>(payload));

        // ---- P5c rv (CONTRACT-P5C.md §5.3): the residual-value record, opcode 79 -------------
        //
        // An ordinary set_* row, unlike the two control records beside it: a fixed-width POD
        // with no blob, no tail and no reply, so the bounds gate is the whole validation and
        // the arm is the applier entry point - the same shape as set_pixel_pack_state beside
        // it. There is no sink half and no acceptance answer: the write into gPipeInputs is
        // the whole effect.
        case MGPWireOp::SetContextValues:
            MGPipeApplySetContextValues(*static_cast<const MGPContextValues*>(payload));
            return true;

        case MGPWireOp::SetSwapInterval:
            // Class C, wave 3 (census-classC.md "static cross"); not a verb (FillPoints.def:21).
            return false;

        case MGPWireOp::Flush:
            // NOT A VERB AND NOT A NO-OP DECISION: there is no Flush slot in
            // GLFunctionsTable at all, and glFlush/glFinish are empty function bodies
            // (Definitions.cpp:111-112), so R-4's predicted minimum set naming Flush was
            // wrong (C-3). A TriangleScenario that orders its readback with glFlush orders
            // nothing; the verb barrier is what orders it.
            return false;

        case MGPWireOp::kInvalid:
        case MGPWireOp::kOpCount:
        default:
            WireProtocolFatalAt("opcode", static_cast<Uint64>(op),
                                static_cast<Uint64>(MGPWireOp::kOpCount));
        }
    }

} // namespace MobileGL::MG_Remote::Wire
