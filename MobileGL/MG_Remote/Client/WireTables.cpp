// MobileGL - MobileGL/MG_Remote/Client/WireTables.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The ENCODE TWIN of gMGPipeWireRecordApply: thirty-eight emitters that turn a table call into
// a wire record. Owner: package c1 (P5 ruling R-17). See WireTables.h for the install order
// and MG_Pipe/PipeRoute.h for what R-17 actually cost.
//
// EVERY EMITTER IS THE SAME FOUR STEPS, and the macros below exist so that a reader can check
// thirty-eight rows against PipeTables.inc in one pass instead of reading thirty-eight bodies:
//
//     1. require a session - a slot that fell through to a driver this role does not have is
//        the failure R-4 exists to prevent, and there is no fall-through here either;
//     2. stage the blob, if the row has one, and name the SEG_STAGE run in the payload's own
//        MGPBlobRef - which is why the payload is COPIED: the table hands it over const, and
//        the blobref is the one field the client must write after the caller is done with it;
//     3. EmitAndWait, which is the barrier's wait and the reply's wait at once (R-3/R-5);
//     4. post the answer, for the rows that have one, into MG_Pipe's reply mailbox.
//
// WHAT IS DELIBERATELY NOT HERE. b1's `PushPersistentMapsBeforeVerb` / `MarkGpuWritesFor*` are
// NOT called from these thirty-eight. They are pre-VERB hooks and these are not verbs: they
// are the resource, CSO and state records that a verb is later drawn against. The five class-B
// verbs in EmitTables.cpp call them, once each, immediately before their record, which is the
// ordering b1's B-1 fix depends on. Calling them here as well would push a persistent map
// before every `set_dynamic_state` - hundreds of times a frame, and each one a real record.

#include "WireTables.h"

#if MOBILEGL_BUILD_DISAGGREGATED

#include "ClientSession.h"

#include "../Server/ServerLoop.h"

#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeRoute.h>
// P5c (ct): EmitObjectDeathRecord resolves the dying object's handle in the CLIENT's own
// allocator - a client surface, asked on the client thread, which is the one place the lookup
// is legal under rule E (CONTRACT-P5C.md §5.2).
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_Util/Debug/Log.h>
// P5e (pg): ID-87 asks this package for bytes-per-link measured rather than argued, and the
// archive's byte count exists exactly once - here, where it is serialised.
#include <MG_Util/Metrics/PipeStats.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Client {

    using MG_Pipe::MGPWireOp;

    // TABLE 3's ROLE SPLIT, AS A RUNTIME CHECK. gMGPipeScreen / gMGPipeContext are PROCESS
    // globals and under `inproc` the server role is a thread in this same process, so the
    // apply thread running the server's own backend - the EGL bring-up, InitCapabilities,
    // the applier - reaches these very emitters. A record published there would be waited
    // for by the thread that is supposed to apply it: `Fatal{BarrierTimeout,
    // "ResourceRespecify"}` from `mgl-srv-apply`, thirty seconds into bring-up, which is
    // exactly how this was found.
    //
    // THE ANSWER IS NOT "SUPPRESS THE RECORD" - it is "run the server's own code", because
    // on that thread this process IS the server and the applier is one call away. It is
    // the same thing PipeWireCodec does on the decode side, where every arm calls
    // MGPipeApply* directly and never goes through a table.
    //
    // Under `spawn` (P6) the predicate is constantly false in the client process and
    // constantly true in the server's, so this costs one atomic load and changes nothing.
    //
    // NOT in the anonymous namespace, because M5 needs it from MG_Impl/Pipe/PipeFill.cpp too
    // (its split-only respecify/flush branches must not run on the apply thread). Declared in
    // WireTables.h.
    //
    // AND IT STAYS OUT OF LINE (P5d round 3, package D). OnApplyThread() became an inline
    // relaxed load plus a thread-pointer compare in ServerLoop.h, so the body this forwards to
    // is now smaller than the call that reaches it - but inlining THIS would mean including a
    // server header from WireTables.h, and the declaration above exists precisely so PipeFill
    // does not have to. One PLT hop per verb is the price of that boundary; the four-part
    // predicate behind it, which was the measured cost (2.66% self on the client thread at
    // head 56a77348), is gone either way. The "one atomic load" the paragraph above promised
    // is now literally what it costs.
    Bool RunsAsTheServerRole() { return Server::ServerLoop::OnApplyThread(); }

    namespace {

        Uint64 g_emitted = 0;
        Uint64 g_declined = 0;

        // TEARDOWN REFUSAL (codex 4). ClientSession::Stop marks the routed tables uninstalled
        // BEFORE it frees the rings, the segments and the transport - so the window between then
        // and the moment the monolith adapters go back is one in which a routed GL-thread call
        // must not run the applier on the caller (the forbidden path table 3 draws) and must not
        // reach a half-freed ring. Round 2 reinstalled the monolith adapters at the top of Stop,
        // which is exactly running the applier on the caller; a routed mutation after
        // UninstallClientWireTables() moved no wire ordinal and never refused (the cross-family
        // verifier reproduced it). So Uninstall now RAISES THIS FLAG and leaves the wire rows in
        // place; RequireSession below reads it and refuses by name before it touches anything.
        // Atomic because the apply thread's own role check races a GL-thread teardown.
        std::atomic<Bool> g_clientTablesUninstalled{false};

        ClientSession& RequireSession(const char* row) {
            // BEFORE the session lookup and before any ring access. A routed call that arrives
            // once Stop has begun tearing the session down is refused by name rather than run
            // on the caller's thread - the applier is server-exclusive (table 3), and a wire
            // emit into a ring being freed is a use-after-free. The monolith adapters are put
            // back only as the LAST step of teardown, for the at-exit ~BufferObject deletes that
            // legitimately reach a process with no session (see ReinstallMonolithAfterTeardown).
            RequireClientTablesInstalled(row);
            ClientSession* session = ClientSession::Active();
            if (session == nullptr) {
                MGLOG_F("MGPipe: Fatal{NoClientSession, \"%s\"} - the client wire tables are "
                        "installed but no ClientSession is active. A row may not fall through to "
                        "a driver this role does not have",
                        row);
                std::abort();
            }
            return *session;
        }

        // Stages a mandatory blob. `StageBytes` Fatals on a zero size by design (R-2.2: "the
        // record declared no blob" and "the record declared an empty blob" must not be spelled
        // the same way on a wire), so a row whose decoder calls RequireDeclaredBlob or
        // ResolveOrFatal is checked HERE, on the producing side, where the row has a name.
        MG_Pipe::MGPBlobRef StageRequired(ClientSession& session, const char* row,
                                          const void* bytes, Uint64 count) {
            if (bytes == nullptr || count == 0) {
                MGLOG_F("MGPipe: Fatal{BlobMissing, \"%s\"} - the row's decoder requires a "
                        "declared blob and the call site handed over %llu bytes at %p. Under "
                        "monolith the companion pointer carries them; under split they have to "
                        "be staged, and there is nothing to stage",
                        row, static_cast<unsigned long long>(count), bytes);
                std::abort();
            }
            return session.Encoder().StageBytes(bytes, count);
        }

        // Stages an OPTIONAL blob: the two sub-data rows, whose decoders resolve only when the
        // record's own size field says there are bytes. All three fields zero is the wire's
        // "no blob declared", and CheckBlobIsHonest refuses any other spelling of it.
        MG_Pipe::MGPBlobRef StageOptional(ClientSession& session, const void* bytes, Uint64 count) {
            if (bytes == nullptr || count == 0) return MG_Pipe::MGPBlobRef{};
            return session.Encoder().StageBytes(bytes, count);
        }

        // ---------------------------------------------------------------------------------
        // The three regular shapes
        // ---------------------------------------------------------------------------------

#define MGP_WIRE_PLAIN(Name, Payload, Table)                                                       \
    void Wire_##Name(const MG_Pipe::Payload* payload) {                                            \
        if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolith##Table().Name(payload); return; }      \
        ClientSession& session = RequireSession(#Name);                                            \
        session.EmitAndWait(MGPWireOp::Name, payload, sizeof(*payload), nullptr, 0, nullptr, 0,    \
                            nullptr);                                                              \
        ++g_emitted;                                                                               \
    }

#define MGP_WIRE_BLOB(Name, Payload, BlobMember)                                                   \
    void Wire_##Name(const MG_Pipe::Payload* payload, const void* blobBytes,                       \
                     Uint64 blobByteCount) {                                                       \
        if (RunsAsTheServerRole()) {                                                               \
            MG_Pipe::MGPipeMonolithContext().Name(payload, blobBytes, blobByteCount);              \
            return;                                                                                \
        }                                                                                          \
        ClientSession& session = RequireSession(#Name);                                            \
        MG_Pipe::Payload record = *payload;                                                        \
        record.BlobMember = StageRequired(session, #Name, blobBytes, blobByteCount);               \
        session.EmitAndWait(MGPWireOp::Name, &record, sizeof(record), nullptr, 0, nullptr, 0,      \
                            nullptr);                                                              \
        ++g_emitted;                                                                               \
    }

#define MGP_WIRE_TAIL(Name, Payload, TailType)                                                     \
    void Wire_##Name(const MG_Pipe::Payload* payload, const void* varTail, Uint32 varTailCount) {   \
        if (RunsAsTheServerRole()) {                                                               \
            MG_Pipe::MGPipeMonolithContext().Name(payload, varTail, varTailCount);                 \
            return;                                                                                \
        }                                                                                          \
        ClientSession& session = RequireSession(#Name);                                            \
        session.EmitAndWait(MGPWireOp::Name, payload, sizeof(*payload), varTail,                   \
                            static_cast<Uint64>(varTailCount) * sizeof(MG_Pipe::TailType),         \
                            nullptr, 0, nullptr);                                                  \
        ++g_emitted;                                                                               \
    }

        // -- screen -------------------------------------------------------------------
        MGP_WIRE_PLAIN(ResourceDestroy, MGPHandleOnly, Screen)
        MGP_WIRE_PLAIN(UnmapPersistent, MGPHandleOnly, Screen)

        // -- context, plain -----------------------------------------------------------
        MGP_WIRE_PLAIN(BindRenderState, MGPBindRenderState, Context)
        MGP_WIRE_PLAIN(DeleteRenderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(BindVertexElements, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteVertexElements, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteSamplerState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(CreateSamplerView, MGPSamplerView, Context)
        MGP_WIRE_PLAIN(DeleteSamplerView, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(BindShaderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(DeleteShaderState, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetDrawProgram, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetDispatchProgram, MGPHandleOnly, Context)
        MGP_WIRE_PLAIN(SetFramebufferState, MGPFramebufferState, Context)
        MGP_WIRE_PLAIN(SetIndexBuffer, MGPIndexBuffer, Context)
        MGP_WIRE_PLAIN(SetPixelPackState, MGPPixelPackState, Context)
        MGP_WIRE_PLAIN(SetPatchState, MGPPatchState, Context)
        // P5c (rv), CONTRACT-P5C.md §5.3: the residual-value record is an ordinary routed
        // set_* row - a fixed POD, no blob, no tail, no reply. The PRODUCER is transport-gated
        // (PipeFill.cpp's EmitContextValues), so under monolith this wrapper is never reached;
        // the server-role arm forwards to the monolith adapter exactly like its siblings.
        MGP_WIRE_PLAIN(SetContextValues, MGPContextValues, Context)

        // -- context, mandatory blob --------------------------------------------------
        MGP_WIRE_BLOB(CreateRenderState, MGPRenderStateDesc, Blob)
        MGP_WIRE_BLOB(CreateVertexElements, MGPVertexElements, Blob)
        MGP_WIRE_BLOB(CreateSamplerState, MGPSamplerDesc, Parameters)
        MGP_WIRE_BLOB(SetGlobalConstants, MGPGlobalConstants, Blob)

        // -- context, variable tail ---------------------------------------------------
        MGP_WIRE_TAIL(SetVertexBuffers, MGPVertexBuffers, MGPVertexBuffer)
        MGP_WIRE_TAIL(SetSamplerViews, MGPSamplerViews, MGPBoundView)
        MGP_WIRE_TAIL(BindSamplerStates, MGPSamplerStates, MGPipeHandle)
        MGP_WIRE_TAIL(SetShaderImages, MGPShaderImages, MGPImageView)
        // P5e (sb, CONTRACT-P5E.md §5.6): the indexed buffer binding points, one record per
        // class. The generic tail wrapper carries the FIRST tail only, which is the whole of
        // the record on Espryt - the optional MGHostSpan tail exists for kCapNeedsHostUboBytes
        // and that bit is 0 for the whole of P5.
        MGP_WIRE_TAIL(SetShaderBuffers, MGPShaderBuffers, MGPBufferRange)
        MGP_WIRE_TAIL(SetVertexAttribDefaults, MGPVertexAttribDefaults, MGPAttribValue)

#undef MGP_WIRE_PLAIN
#undef MGP_WIRE_BLOB
#undef MGP_WIRE_TAIL

        // -- the rows that fit none of the three shapes --------------------------------

        // set_dynamic_state. AN OPTIONAL BLOB, NOT A MANDATORY ONE (round-2 regression, the
        // census's Fatal{BlobMissing, "SetDynamicState"}). EmitRenderState (PipeFill.cpp:2405-
        // 2421) sends a set_dynamic_state whenever the RenderState VERSION moved, and when the
        // chunk-level suppressor finds NO dynamic chunk changed it sends the 32-byte header with
        // ChunkMask == 0 and blobByteCount == 0 - a version-only update, which the comment there
        // calls out as deliberate. The generic MGP_WIRE_BLOB wrapper stages through StageRequired,
        // which Fatals on a zero count, so a legal header-only update aborted the process with a
        // blob-shape complaint that had nothing to do with the actual state of the pipe. The
        // DECODER already handles it: PipeWireCodec.cpp:1571-1587 takes the `ChunkMask == 0` arm
        // (CheckBlobIsHonest, no blob resolved) and applies the header alone. So the emitter is
        // the only side that was wrong; it stages OPTIONALLY, exactly like the two sub-data rows.
        // A non-empty mask still stages (blobByteCount > 0), so the reduced-path scenarios, whose
        // first draw is freshly primed with every chunk, are byte-for-byte unchanged.
        void Wire_SetDynamicState(const MG_Pipe::MGPDynamicState* payload, const void* blobBytes,
                                  Uint64 blobByteCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().SetDynamicState(payload, blobBytes, blobByteCount);
                return;
            }
            ClientSession& session = RequireSession("SetDynamicState");
            MG_Pipe::MGPDynamicState record = *payload;
            record.Blob = StageOptional(session, blobBytes, blobByteCount);
            session.EmitAndWait(MGPWireOp::SetDynamicState, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
            ++g_emitted;
        }

        // set_residual_value_state. CONTRACT-P5 table 1 row 6: the applier takes a frontend
        // `ResidualValueBlock&` and `MGPResidualValueState` is never instantiated on the live
        // path, so the encoder invents BOTH the record fill and the blob fill. The block IS
        // the blob, whole - the decoder requires exactly sizeof(ResidualValueBlock) and says
        // why ("a size that only ever ratchets down makes a short read silently lose
        // CapabilityBits"), so the two sides state the same number from the same header.
        void Wire_SetResidualValueState(const MG_Pipe::MGPResidualValueState* payload,
                                        const void* blobBytes, Uint64 blobByteCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().SetResidualValueState(payload, blobBytes,
                                                                       blobByteCount);
                return;
            }
            ClientSession& session = RequireSession("SetResidualValueState");
            MG_Pipe::MGPResidualValueState record = *payload;
            record.Blob = StageRequired(session, "SetResidualValueState", blobBytes, blobByteCount);
            session.EmitAndWait(MGPWireOp::SetResidualValueState, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
            ++g_emitted;
        }

        // resource_readback. kReplySlot, and the answer is COMPLETION only: the bytes travel
        // server -> client in SEG_EVENT through OnBufferWriteback, because the destination is
        // the client's shadow and its size is the resource's, not a slot's (CONTRACT-P5 table 1
        // row 22). So the reply buffer is deliberately {nullptr, 0} and the wait is what makes
        // the writeback already drained by the time this returns.
        void Wire_ResourceReadback(const MG_Pipe::MGPReadback* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithContext().ResourceReadback(payload, reply); return; }
            ClientSession& session = RequireSession("ResourceReadback");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::ResourceReadback, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, 0);
            ++g_emitted;
        }

        // ---- the three acceptance rows that fit a generated signature ----------------
        //
        // THE ANSWER IS THE SERVER'S AND NOTHING ELSE. `EmitAndWait` returns the record's seq
        // and fills `status` from the reply slot the server stamped; DECLINED is `false` and OK
        // is `true`, and neither is derived from anything this side knows. R-5 exists because
        // "always accept" is ID-39's 66 lost DirectVulkan uploads and "accept if we emitted" is
        // the same bug wearing a counter.

        void Wire_ResourceCreate(const MG_Pipe::MGPResourceDesc* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithScreen().ResourceCreate(payload, reply); return; }
            ClientSession& session = RequireSession("ResourceCreate");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::ResourceCreate, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
        }

        void Wire_SetTextureParams(const MG_Pipe::MGPTextureParams* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithContext().SetTextureParams(payload, reply); return; }
            ClientSession& session = RequireSession("SetTextureParams");
            Int32 status = 0;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::SetTextureParams, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
        }

        void Wire_ResourceSubData(const MG_Pipe::MGPSubData* payload, const void* blobBytes,
                                  Uint64 blobByteCount, const void* varTail, Uint32 varTailCount,
                                  MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().ResourceSubData(payload, blobBytes, blobByteCount,
                                                                 varTail, varTailCount, reply);
                return;
            }
            ClientSession& session = RequireSession("ResourceSubData");
            MG_Pipe::MGPSubData record = *payload;
            record.Blob = StageOptional(session, blobBytes, blobByteCount);
            // P5e (ra, CONTRACT-P5E §2.5 / ruling 15): the BUFFER half does not want its
            // answer, and MGPipeSubDataWantsItsReply is the one place that decides - the same
            // function the route reads, so the two halves of this call cannot disagree about
            // whether a wait is owed. Under run-ahead the record is published and this thread
            // returns; the persistent-map push (PersistentMapTracker's 64 KB blocks) stops
            // being a hidden round trip per block, which is the single biggest wait left on
            // the steady path that is not a draw.
            //
            // THE ANSWER IS ACCEPT-BY-CONSTRUCTION, AND THAT IS HONEST HERE AND NOWHERE ELSE:
            // the only caller discards it (PipeFill.cpp's MGPipeEmitResourceSubData), so
            // "accepted" is not a re-derivation of a server decision - it is the absence of a
            // question. The server still posts the real answer into the slot; nothing reads
            // it, which ReplySlot.h:16, 105 makes legal. A row whose acceptance a caller USES
            // may never take this path - R-5 has not moved.
            const Bool wantReply = MG_Pipe::MGPipeSubDataWantsItsReply(record);
            Int32 status = 0;
            const Wire::WireTail tail{varTail, static_cast<Uint64>(varTailCount) *
                                                   sizeof(MG_Pipe::MGPSubRegion)};
            const Uint64 seq = session.EmitAndWaitTails(
                MGPWireOp::ResourceSubData, &record, sizeof(record),
                varTail != nullptr ? &tail : nullptr, varTail != nullptr ? 1u : 0u, nullptr, 0,
                &status, nullptr, wantReply);
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, wantReply ? status : 0,
                                     (wantReply ? status == 0 : true) ? 1u : 0u);
            ++g_emitted;
            if (wantReply && status == 1) ++g_declined;
        }

        void Wire_BufferSubDataResident(const MG_Pipe::MGPSubData* payload, const void* blobBytes,
                                        Uint64 blobByteCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithContext().BufferSubDataResident(payload, blobBytes,
                                                                       blobByteCount);
                return;
            }
            ClientSession& session = RequireSession("BufferSubDataResident");
            MG_Pipe::MGPSubData record = *payload;
            record.Blob = StageOptional(session, blobBytes, blobByteCount);
            session.EmitAndWait(MGPWireOp::BufferSubDataResident, &record, sizeof(record), nullptr,
                                0, nullptr, 0, nullptr);
            ++g_emitted;
        }

        // ---- the four escapes --------------------------------------------------------

        // resource_respecify. R-13.3: `initialBytes` is ALWAYS nullptr under split and the
        // initial content arrives as resource_subdata records immediately after this one. The
        // caller's bytes are therefore not dropped - they are re-expressed - and PipeFill.cpp's
        // emitter is where that happens, because the chunking walk that has to size them
        // (MGPipeForEachSubDataRecordRange) lives there. What this emitter owes is the REFUSAL:
        // a non-null pointer arriving here means the call site was not converted, and silently
        // ignoring it would lose exactly the bytes R-13.3 promised would follow.
        Bool Wire_Escape_ResourceRespecify(const MG_Pipe::MGPResourceDesc* desc,
                                           const void* initialBytes,
                                           const MG_Pipe::MGPRespecifiedLevel* level) {
            if (RunsAsTheServerRole()) {
                return MG_Pipe::MGPipeMonolithEscapes().ResourceRespecify(desc, initialBytes, level);
            }
            ClientSession& session = RequireSession("ResourceRespecify");
            if (initialBytes != nullptr) {
                MGLOG_F("MGPipe: Fatal{UncarriedInitialBytes, \"resource_respecify\"} - a call "
                        "site handed initial content to a split respecify. R-13.3 rules that "
                        "initialBytes never crosses and that the content follows as "
                        "resource_subdata; a caller that still passes it has bytes nothing will "
                        "carry");
                std::abort();
            }
            // The scope rides in the descriptor's own pads (CONTRACT-P5 table 1 row 19b, LANDED)
            // and is written only through MGPipeSetRespecifiedLevel - three fields are one
            // value, and an open-coded writer that forgets the presence byte says "level 0 of
            // upload target 0" where it meant "the whole resource".
            MG_Pipe::MGPResourceDesc record = *desc;
            if (level != nullptr) {
                MG_Pipe::MGPipeSetRespecifiedLevel(record, level->UploadTarget, level->Level);
            } else {
                MG_Pipe::MGPipeClearRespecifiedLevel(record);
            }
            Int32 status = 0;
            const Uint64 seq =
                session.EmitAndWait(MGPWireOp::ResourceRespecify, &record, sizeof(record), nullptr,
                                    0, nullptr, 0, &status);
            (void)seq;
            ++g_emitted;
            // ERROR IS NOT A DECLINE (M4 / codex 5 / R-5). status is 0 OK / 1 DECLINED / 2 ERROR;
            // an escape may not fold 2 into `false`, which is what a bare `return status == 0`
            // did - a transport fault then read as "the server said no". Every reply-owning row
            // - the generated acceptance rows through MGPipeTakeReplyBool, and now these two
            // escapes - answers ERROR with the same named Fatal.
            if (status == 2) {
                MGLOG_F("MGPipe: Fatal{ReplyError, \"resource_respecify\"} - the row answered "
                        "ERROR, which is not an acceptance answer; folding it into accepted or "
                        "refused would make a transport fault look like a resource decision");
                std::abort();
            }
            if (status == 1) ++g_declined;
            return status == 0;
        }

        // resource_flush_range. R-13.2: it carries NO bytes under split - it is a
        // {range, AccessFlags} control record and the bytes of exactly that range arrive ahead
        // of it as resource_subdata. Same refusal as above, for the same reason: a blobref here
        // would be "a second, forgeable way to say the same thing".
        void Wire_Escape_ResourceFlushRange(const MG_Pipe::MGPFlushRange* record,
                                            const void* bytes) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithEscapes().ResourceFlushRange(record, bytes);
                return;
            }
            ClientSession& session = RequireSession("ResourceFlushRange");
            (void)bytes; // ruled uncarried; the emitter in PipeFill.cpp sends the range first
            session.EmitAndWait(MGPWireOp::ResourceFlushRange, record, sizeof(*record), nullptr, 0,
                                nullptr, 0, nullptr);
            ++g_emitted;
        }

        // map_persistent. R-6/R-2.4: the split answer is a CONSTANT DECLINE, and it still costs
        // a record, because the server has to know the client asked - the applier's
        // MapPersistentRoundtrips counter is defined as "one per storage definition in both
        // modes" and a client that answered locally would zero it. `size` and `seedBytes` have
        // no carrier (MGPHandleOnly is {Handle, Kind}) and need none: nothing is minted.
        void* Wire_Escape_MapPersistent(const MG_Pipe::MGPHandleOnly* handle, Uint64 size,
                                        const void* seedBytes) {
            if (RunsAsTheServerRole()) {
                return MG_Pipe::MGPipeMonolithEscapes().MapPersistent(handle, size, seedBytes);
            }
            ClientSession& session = RequireSession("MapPersistent");
            (void)size;
            (void)seedBytes;
            Int32 status = 0;
            session.EmitAndWait(MGPWireOp::MapPersistent, handle, sizeof(*handle), nullptr, 0,
                                nullptr, 0, &status);
            ++g_emitted;
            // ERROR IS NOT A DECLINE (M4 / codex 5 / R-5), the same rule the respecify escape and
            // the generated acceptance rows obey: status 2 is a transport fault, and returning
            // nullptr for it would make it indistinguishable from R-6's legitimate decline.
            if (status == 2) {
                MGLOG_F("MGPipe: Fatal{ReplyError, \"map_persistent\"} - the row answered ERROR, "
                        "which is not an acceptance answer; a transport fault is not a resource "
                        "decision and may not be folded into the decline R-6 predicts");
                std::abort();
            }
            if (status == 1) ++g_declined;
            // NOT "always nullptr": the answer is READ. R-6 says the server declines, and the
            // day it stops declining this returns what it actually said rather than what the
            // ruling predicted.
            if (status == 0) {
                MGLOG_F("MGPipe: Fatal{UnexpectedMapAccept, \"map_persistent\"} - the server "
                        "accepted a persistent map under split. R-6 makes the split answer a "
                        "constant decline because there is no way to hand a host pointer across "
                        "a process boundary in P5; a pointer arriving here is one this client "
                        "cannot dereference");
                std::abort();
            }
            return nullptr;
        }

        // create_shader_state. SEVEN blobrefs and TWO typed frontend pointers; one blobBytes
        // pair cannot express seven runs. The serializer already exists and no package may
        // write a second one (CONTRACT-P5 table 1 row 3): EncodeProgramArtifacts produces one
        // archive, the decoder's DecodeProgramArtifacts consumes it, and the six per-stage runs
        // stay UNDECLARED because the modules already travel inside the archive - a declared
        // Spirv[i] is Fatal on the far side rather than ignored.
        void Wire_Escape_CreateShaderState(const MG_Pipe::MGPProgramDesc* desc,
                                           const MG_State::GLState::LinkArtifacts* link,
                                           const MG_State::GLState::SpirvArtifacts* spirv,
                                           const Uint32* linkedStages, Uint32 linkedStageCount) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithEscapes().CreateShaderState(desc, link, spirv, linkedStages,
                                                                   linkedStageCount);
                return;
            }
            ClientSession& session = RequireSession("CreateShaderState");
            if (link == nullptr || spirv == nullptr) {
                MGLOG_F("MGPipe: Fatal{ArtefactsMissing, \"create_shader_state\"} - the record's "
                        "two typed companions are null. Under monolith the applier reads the "
                        "modules out of spirv->generatedSpirv; under split there is nothing to "
                        "serialise, and emitting the record anyway would create a CSO with no "
                        "code");
                std::abort();
            }
            // EncodeProgramArchive APPENDS and never fails - everything it walks is owned
            // plain data - so an empty archive means the two structs themselves were empty,
            // which is a linked program with no artefacts and is not a codec question.
            //
            // P5e (pg): THE FRAMED form, so the record's own copy on the far side can pair each
            // module with its stage. The bare EncodeProgramArtifacts is still what the verify
            // build's round-trip pin uses; this is the one that crosses.
            Vector<Uint8> archive;
            Vector<Uint32> stages(linkedStages, linkedStages + linkedStageCount);
            MG_State::GLState::EncodeProgramArchive(*link, *spirv, stages, archive);
            if (MG_Util::PipeStats::Enabled()) {
                // ID-87's measurement, taken where the bytes actually exist rather than
                // estimated: one sample per link, so the steady-state cost is (links this
                // frame) x (bytes per link) and both halves are countable from the stats line.
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::CsoBlobBytes,
                                             static_cast<Uint64>(archive.size()));
            }
            if (archive.empty()) {
                MGLOG_F("MGPipe: Fatal{ArchiveEmpty, \"create_shader_state\"} - the program's "
                        "artefacts serialised to nothing");
                std::abort();
            }
            MG_Pipe::MGPProgramDesc record = *desc;
            for (Uint32 i = 0; i < 6; ++i) record.Spirv[i] = MG_Pipe::MGPBlobRef{};
            record.Reflection = session.Encoder().StageBytes(archive.data(), archive.size());
            session.EmitAndWait(MGPWireOp::CreateShaderState, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);
            ++g_emitted;
        }

        // set_program_bindings (P5e, pg). THE FIFTH ESCAPE, for PipeRoute.h's reason: three
        // tails in three index spaces plus a parallel name array, which no generated
        // (payload, varTail, varTailCount) row can express.
        //
        // THE NAMES ARE STAGED ONE BY ONE and each element's own MGHostSpan names its run, so
        // the third tail is self-describing the way set_storage_block_binding's single name
        // already is. Staged rather than packed into one run with offsets, because the honesty
        // pass the decoder runs is per span and a packed run would have to be re-split there
        // against arithmetic nothing on the wire declares.
        void Wire_Escape_SetProgramBindings(const MG_Pipe::MGPProgramBindings* hdr,
                                            const Int32* blockBindings,
                                            const MG_Pipe::MGPProgramSamplerUnit* samplerUnits,
                                            const MG_Pipe::MGPProgramStorageOverride* storageOverrides,
                                            const char* const* storageOverrideNames) {
            if (RunsAsTheServerRole()) {
                MG_Pipe::MGPipeMonolithEscapes().SetProgramBindings(hdr, blockBindings, samplerUnits,
                                                                    storageOverrides,
                                                                    storageOverrideNames);
                return;
            }
            ClientSession& session = RequireSession("SetProgramBindings");
            MG_Pipe::MGPProgramBindings record = *hdr;

            // The override tail is COPIED before it is emitted, because the span is the one
            // field the client must write after the caller is done with it - the same reason
            // MGP_WIRE_BLOB copies its payload.
            Vector<MG_Pipe::MGPProgramStorageOverride> overrides;
            overrides.reserve(record.StorageOverrideCount);
            for (Uint32 i = 0; i < record.StorageOverrideCount; ++i) {
                MG_Pipe::MGPProgramStorageOverride entry = storageOverrides[i];
                const char* const name = storageOverrideNames[i];
                // Size = strlen + 1: THE NUL TRAVELS, exactly as set_storage_block_binding's
                // name does (contract table 0's block-name row), and the decoder refuses a run
                // whose last byte is not NUL.
                const Uint64 nameBytes = static_cast<Uint64>(std::strlen(name)) + 1ull;
                const MG_Pipe::MGPBlobRef staged = session.Encoder().StageBytes(name, nameBytes);
                // A HOST SPAN AND NOT A BLOBREF, which is what the row's kHostSpan flag means:
                // the same run, described in the shape the decoder's honesty pass reads. Ptr
                // stays null - rule B, "the encoder writes nullptr and names SEG_STAGE" - so
                // the name is unreachable on a second process except through the segment table,
                // which is the whole point of the flag.
                entry.Name = MG_Pipe::MGHostSpan{};
                entry.Name.Ptr = nullptr;
                entry.Name.Seg = staged.Seg;
                entry.Name.Offset = staged.Offset;
                entry.Name.Size = staged.Size;
                overrides.push_back(entry);
            }

            const Wire::WireTail tails[3] = {
                {blockBindings, static_cast<Uint64>(record.BlockBindingCount) * sizeof(Int32)},
                {samplerUnits, static_cast<Uint64>(record.SamplerUnitCount) *
                                   sizeof(MG_Pipe::MGPProgramSamplerUnit)},
                {overrides.empty() ? nullptr : overrides.data(),
                 static_cast<Uint64>(record.StorageOverrideCount) *
                     sizeof(MG_Pipe::MGPProgramStorageOverride)},
            };
            // ALWAYS THREE, even when a count is 0 - the decoder derives the third tail's
            // offset from the first two, so declaring fewer would put the same bytes somewhere
            // else (PipeWireCodec.cpp's layout arm says this from the other side).
            session.EmitAndWaitTails(MGPWireOp::SetProgramBindings, &record, sizeof(record), tails, 3,
                                     nullptr, 0, nullptr);
            ++g_emitted;
        }

    } // namespace

    // =====================================================================================
    // P5c (ct), CONTRACT-P5C.md §5: the two control records' producers
    // =====================================================================================

    namespace {
        // The ContextSerial of the NEXT applier_reset record. NOWHERE IN THE FRONTEND NUMBERS
        // A MAKE-CURRENT: the tracker knows the EDGE (FreshlyPrimed) and no session or context
        // carries a serial for it (checked at the contract commit), so the serial is this
        // client's own count of applier_reset emissions - 0 for the first make-current, one
        // more per primed edge. That is the only value the server can hold the client to in a
        // one-context session: the sink keeps the same count and ASSERTS equality
        // (CONTRACT-P5C.md §1: the value is asserted, never dispatched on; P6's multi-context
        // shape is what will read it for real). GL-thread only, like every emitter here.
        Uint64 g_applierResetContextSerial = 0;
    } // namespace

    Bool EmitApplierResetRecord() {
        // NO SESSION, NO RECORD - and false rather than the RequireSession Fatal, because a
        // validate can legitimately prime with a transport CONFIGURED but no live session:
        // the bring-up window before ClientSession::Start() (§6 layer 2's documented
        // exception), and the server-role-only shape a ServerLoop fixture drives with
        // Transport=InProcess and no client at all. In both this process has no wire for the
        // reset to cross, and the caller answers with the direct call the record replaced.
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started()) return false;
        if (g_clientTablesUninstalled.load(std::memory_order_acquire)) return false;
        MG_Pipe::MGPApplierReset record{};
        record.ContextSerial = g_applierResetContextSerial++;
        session->EmitAndWait(MGPWireOp::ApplierReset, &record, sizeof(record), nullptr, 0, nullptr,
                             0, nullptr);
        ++g_emitted;
        return true;
    }

    // P5c (rv), CONTRACT-P5C.md §5.3. Whether set_context_values can cross RIGHT NOW: a live,
    // started session whose tables are not being torn down. PipeFill.cpp gates BOTH halves of
    // the row on this - the emission and the residual-fill skip - so a configured-but-wireless
    // transport (the bring-up window, a server-role-only fixture) keeps the pull, and the two
    // can never disagree about who supplies the eight fields.
    Bool ContextValuesWireLive() {
        ClientSession* session = ClientSession::Active();
        return session != nullptr && session->Started() &&
               !g_clientTablesUninstalled.load(std::memory_order_acquire);
    }

    ObjectDeathEmit EmitObjectDeathRecord(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
        using MG_Pipe::MGPipeHandle;

        // NO SESSION, NO RECORD - and that is NOT the RequireSession shape on purpose. A death
        // notice can outlive the session by construction: frontend objects die at context
        // teardown and at process exit, after Stop has joined the apply thread and freed the
        // rings, and the twins this record would kill died with the server. Emitting into that
        // window is a use-after-free; Fatal-ing on it is an abort at exit() for a legal death.
        // NoSession is its own answer (not folded into NoHandle) because the caller's fallback
        // for "no wire exists" is delivery to the server loop this process still has, while
        // "no handle" means there is nothing to deliver at all.
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started()) return ObjectDeathEmit::NoSession;
        // The same window, one step earlier: Stop has raised the teardown refusal but not yet
        // joined the apply thread. A death landing here is destructor-driven, not an app bug,
        // and the twin it would kill dies with the backend Stop is destroying - so it is
        // skipped, where a routed call in the same window is Fatal{ClientTablesUninstalled}.
        if (g_clientTablesUninstalled.load(std::memory_order_acquire)) {
            return ObjectDeathEmit::NoSession;
        }

        // THE CLIENT'S OWN ALLOCATOR, ON THE CLIENT'S OWN THREAD (§5.2 step 1). A lifetime id
        // the allocator cannot resolve names an object that never crossed - no create record
        // ever carried its handle, so the server has no twin to kill and NOTHING is emitted.
        // This replaces the mailbox's unconditional delivery, which hopped every death to the
        // apply thread whether or not the server had ever seen the object.
        const MGPipeHandle handle = MG_Pipe::MGPipeSlots().FindByLifetimeId(kind, lifetimeId);
        if (MG_Pipe::MGPipeHandleIsNull(handle)) return ObjectDeathEmit::NoHandle;

        MG_Pipe::MGPHandleOnly record{};
        record.Handle = handle;
        record.Kind = static_cast<Uint32>(kind);
        // The WAIT is the only property the blocking mailbox hop provided and the only one P5c
        // keeps (§5.2 step 3): it orders the death against in-flight verbs that name the
        // handle, so the server's twin cannot be released underneath a verb that is using it.
        session->EmitAndWait(MGPWireOp::ObjectDeath, &record, sizeof(record), nullptr, 0, nullptr,
                             0, nullptr);
        ++g_emitted;
        return ObjectDeathEmit::Emitted;
    }

    void RequireClientTablesInstalled(const char* row) {
        if (g_clientTablesUninstalled.load(std::memory_order_acquire)) {
            MGLOG_F("MGPipe: Fatal{ClientTablesUninstalled, \"%s\"} - the client tables are "
                    "being torn down; refusing before session or ring access", row);
            std::abort();
        }
    }

    void InstallClientWireTables() {
        using namespace MG_Pipe;

        // A fresh install means the routed tables are live again: a Start after a previous
        // session's Stop clears the teardown-refusal flag so its own routed calls are not
        // refused. (Stop already puts the monolith adapters back and clears the flag; this is
        // the belt to that braces.)
        g_clientTablesUninstalled.store(false, std::memory_order_release);

        gMGPipeScreen.ResourceCreate = &Wire_ResourceCreate;
        gMGPipeScreen.ResourceDestroy = &Wire_ResourceDestroy;
        gMGPipeScreen.UnmapPersistent = &Wire_UnmapPersistent;

        gMGPipeContext.CreateRenderState = &Wire_CreateRenderState;
        gMGPipeContext.BindRenderState = &Wire_BindRenderState;
        gMGPipeContext.DeleteRenderState = &Wire_DeleteRenderState;
        gMGPipeContext.CreateVertexElements = &Wire_CreateVertexElements;
        gMGPipeContext.BindVertexElements = &Wire_BindVertexElements;
        gMGPipeContext.DeleteVertexElements = &Wire_DeleteVertexElements;
        gMGPipeContext.CreateSamplerState = &Wire_CreateSamplerState;
        gMGPipeContext.DeleteSamplerState = &Wire_DeleteSamplerState;
        gMGPipeContext.CreateSamplerView = &Wire_CreateSamplerView;
        gMGPipeContext.DeleteSamplerView = &Wire_DeleteSamplerView;
        gMGPipeContext.BindShaderState = &Wire_BindShaderState;
        gMGPipeContext.DeleteShaderState = &Wire_DeleteShaderState;
        gMGPipeContext.SetDrawProgram = &Wire_SetDrawProgram;
        gMGPipeContext.SetDispatchProgram = &Wire_SetDispatchProgram;
        gMGPipeContext.SetDynamicState = &Wire_SetDynamicState;
        gMGPipeContext.SetFramebufferState = &Wire_SetFramebufferState;
        gMGPipeContext.SetVertexBuffers = &Wire_SetVertexBuffers;
        gMGPipeContext.SetIndexBuffer = &Wire_SetIndexBuffer;
        gMGPipeContext.SetSamplerViews = &Wire_SetSamplerViews;
        gMGPipeContext.BindSamplerStates = &Wire_BindSamplerStates;
        gMGPipeContext.SetShaderImages = &Wire_SetShaderImages;
        gMGPipeContext.SetShaderBuffers = &Wire_SetShaderBuffers;
        gMGPipeContext.SetGlobalConstants = &Wire_SetGlobalConstants;
        gMGPipeContext.SetVertexAttribDefaults = &Wire_SetVertexAttribDefaults;
        gMGPipeContext.SetPixelPackState = &Wire_SetPixelPackState;
        gMGPipeContext.SetPatchState = &Wire_SetPatchState;
        gMGPipeContext.SetContextValues = &Wire_SetContextValues;
        gMGPipeContext.SetResidualValueState = &Wire_SetResidualValueState;
        gMGPipeContext.SetTextureParams = &Wire_SetTextureParams;
        gMGPipeContext.ResourceSubData = &Wire_ResourceSubData;
        gMGPipeContext.BufferSubDataResident = &Wire_BufferSubDataResident;
        gMGPipeContext.ResourceReadback = &Wire_ResourceReadback;

        gMGPipeRouteEscapes.ResourceRespecify = &Wire_Escape_ResourceRespecify;
        gMGPipeRouteEscapes.ResourceFlushRange = &Wire_Escape_ResourceFlushRange;
        gMGPipeRouteEscapes.MapPersistent = &Wire_Escape_MapPersistent;
        gMGPipeRouteEscapes.CreateShaderState = &Wire_Escape_CreateShaderState;
        gMGPipeRouteEscapes.SetProgramBindings = &Wire_Escape_SetProgramBindings;

        MGPipeNoteInstalledArm(MGPipeRouteArm::kClientWire);
    }

    void UninstallClientWireTables() {
        // MARK THE ROUTED TABLES UNINSTALLED (codex 4). It does NOT reinstall the monolith
        // adapters, and that is the whole fix: round 2 reinstalled them here, at the TOP of
        // Stop, so a routed GL-thread call arriving during teardown ran the applier on the
        // caller - the forbidden path - and moved no wire ordinal, with no refusal. Raising the
        // flag leaves the Wire_* rows in place; the next routed call reaches RequireSession,
        // reads the flag and aborts by name (Fatal{ClientTablesUninstalled}) before it touches a
        // ring that Stop is about to free. The monolith adapters are put back only once teardown
        // is complete, by ReinstallMonolithAfterTeardown, for the at-exit deletes that reach a
        // process with no session at all. Idempotent: safe to call when nothing was installed.
        g_clientTablesUninstalled.store(true, std::memory_order_release);
    }

    void ReinstallMonolithAfterTeardown() {
        // THE LAST STEP OF ClientSession::Stop, after the rings, segments and transport are gone
        // and the apply thread has joined. Now a routed call can only be a process that no
        // longer has a session - the canonical case is ~BufferObject running from an exit
        // handler (ID-8) - and the monolith adapter, which runs the applier synchronously, is
        // the correct answer for it, exactly as it is in a pure-monolith build. Clearing the
        // flag re-enables the (now monolith) rows. A null row here would be an undiagnosed crash
        // at whatever GL call an exit handler makes; the applier is a defined no-op-or-apply.
        MG_Pipe::MGPipeInstallMonolithTables();
        g_clientTablesUninstalled.store(false, std::memory_order_release);
    }

    Uint64 ClientWireRecordsEmitted() { return g_emitted; }
    Uint64 ClientWireRecordsDeclined() { return g_declined; }

} // namespace MobileGL::MG_Remote::Client

#endif // MOBILEGL_BUILD_DISAGGREGATED
