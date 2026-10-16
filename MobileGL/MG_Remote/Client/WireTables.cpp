// MobileGL - MobileGL/MG_Remote/Client/WireTables.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The encode twin of gMGPipeWireRecordApply: the emitters that turn a routed MGPipe table call
// into a wire record (install order: WireTables.h). On the server role's own thread an emitter
// runs the server's code directly; otherwise it requires a session, stages its blob (the payload
// is copied because its MGPBlobRef must name the SEG_STAGE run), emits through EmitAndWait and
// posts any answer into MG_Pipe's reply mailbox. The macros keep the regular rows checkable
// against PipeTables.inc at a glance. These are state and object records, not verbs, so they
// never call PushPersistentMapsBeforeVerb / MarkGpuWritesFor*: those are pre-verb hooks
// (MG_Impl/Pipe/Verb/VerbPort.cpp) and must not run before every set_dynamic_state.

#include "WireTables.h"
#include <MG_Remote/FatalFunnel.h>

#if MOBILEGL_BUILD_DISAGGREGATED

#include "ClientSession.h"

#include "../Server/ServerLoop.h"

#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeRoute.h>
// P12: MGPipeHandleIsPublished, the latch that says an object's existence is CONFIRMED.
#include <MG_Pipe/PipeMutation.h>
// P5c (ct): EmitObjectDeathRecord resolves the dying object's handle in the CLIENT's own
// allocator - a client surface, asked on the client thread, which is the one place the lookup
// is legal under rule E (CONTRACT-P5C.md §5.2).
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_State/GLState/StateObjectDeathNotice.h>
#include <MG_Util/Debug/Log.h>
// P5e (pg): ID-87 asks this package for bytes-per-link measured rather than argued, and the
// archive's byte count exists exactly once - here, where it is serialised.
#include <MG_Util/Metrics/PipeStats.h>
// P12: kDefaultReplySlotCount, because the create window's ceiling is derived from the pool it
// shares rather than from a number written twice (see kCreateWindowMax below).
#include <MG_Remote/Transport/ReplySlot.h>
// P12: kRetainedAnswersMax, the store capacity the window's own ceiling is derived from - the
// constant itself rather than a second literal, so the two cannot drift.
#include <MG_Remote/Transport/StreamLink.h>
// P12: LinkMetricsBeginReply/ReplyApplied, so the window's blocking take is visible in by_op 2.
#include <MG_Remote/Transport/LinkMetrics.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Client {

    using MG_Pipe::MGPWireOp;

    // True on the server role's own apply thread. Under inproc the server's backend bring-up and
    // applier run in this process and reach these emitters; a record published there would wait
    // for the very thread meant to apply it (Fatal{BarrierTimeout} during bring-up). So on that
    // thread an emitter runs the server's code directly, as the decoder does. Under spawn the
    // answer is constant per process. Declared in WireTables.h because PipeFill.cpp and
    // EGLImpl.cpp need it too; kept out of line so that header includes no server header.
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
                // @Ph-declined (ID-P7-1): returns ClientSession& and runs in the CLIENT - no
                // session to hand back, no peer bytes, and the latch is a server-session idea.
                SessionFail(MGFatalFamily::NoClientSession, "MGPipe: Fatal{NoClientSession, \"%s\"} - the client wire tables are "
                        "installed but no ClientSession is active. A row may not fall through to "
                        "a driver this role does not have",
                        row);
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
                SessionFail(MGFatalFamily::BlobMissing, "MGPipe: Fatal{BlobMissing, \"%s\"} - the row's decoder requires a "
                        "declared blob and the call site handed over %llu bytes at %p. Under "
                        "monolith the companion pointer carries them; under split they have to "
                        "be staged, and there is nothing to stage",
                        row, static_cast<unsigned long long>(count), bytes);
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
        // resource_destroy is MGP_WIRE_PLAIN plus one line: P11 B2 - a destroyed buffer's T0
        // store is released by this client after the record is out (the server's import holds its
        // own reference until its fence for the store's last use; CONTRACT-P11.md B2).
        void Wire_ResourceDestroy(const MG_Pipe::MGPHandleOnly* payload) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithScreen().ResourceDestroy(payload); return; }
            ClientSession& session = RequireSession("ResourceDestroy");
            session.EmitAndWait(MGPWireOp::ResourceDestroy, payload, sizeof(*payload), nullptr, 0, nullptr, 0,
                                nullptr);
            ++g_emitted;
            if (static_cast<MG_Pipe::MGPipeKind>(payload->Kind) == MG_Pipe::MGPipeKind::Buffer)
                session.ReleaseT0Store(payload->Handle);
        }
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
        // and that bit is RESERVED (P8-F): no server publishes it.
        MGP_WIRE_TAIL(SetShaderBuffers, MGPShaderBuffers, MGPBufferRange)
        MGP_WIRE_TAIL(SetVertexAttribDefaults, MGPVertexAttribDefaults, MGPAttribValue)

#undef MGP_WIRE_PLAIN
#undef MGP_WIRE_BLOB
#undef MGP_WIRE_TAIL

        // -- the rows that fit none of the three shapes --------------------------------

        // set_dynamic_state. AN OPTIONAL BLOB, NOT A MANDATORY ONE (round-2 regression, the
        // census's Fatal{BlobMissing, "SetDynamicState"}). EmitRenderState (PipeFill.cpp) sends a
        // set_dynamic_state whenever the RenderState VERSION moved, and when the
        // chunk-level suppressor finds NO dynamic chunk changed it sends the 32-byte header with
        // ChunkMask == 0 and blobByteCount == 0 - a version-only update, which the comment there
        // calls out as deliberate. The generic MGP_WIRE_BLOB wrapper stages through StageRequired,
        // which Fatals on a zero count, so a legal header-only update aborted the process with a
        // blob-shape complaint that had nothing to do with the actual state of the pipe. The
        // DECODER already handles it: PipeWireDecoder::ApplyChecked takes the `ChunkMask == 0` arm
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

        // set_residual_value_state: RETIRED (P13 W3c). Nothing calls it; the row stays installed so
        // the table is total, and reaching it is a named death rather than a record on the wire.
        void Wire_SetResidualValueState(const MG_Pipe::MGPResidualValueState*, const void*, Uint64) {
            SessionFail(MGFatalFamily::ResidualBlockSize,
                        "MGPipe: Fatal{ResidualBlockSize} - set_residual_value_state reached the wire table, "
                        "but %s", "the residual value block was deleted in P13 W3c (op 46 is a retired row; the verify comparator owns the capability cross-check)");
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
            // Cancellation has no wire ordinal; retain the caller's nonzero local ticket.
            if (seq != Wire::kInvalidSeq) reply->Id = seq;
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

        // ---- the create window (MOBILEGL_IPC_CREATE_WINDOW) ------------------------------------
        // resource_create's answer cannot be dropped: an accepted create latches "the applier
        // holds this handle" (PublishCreate), and every converted row reads that latch. The window
        // defers the answer instead: up to that many creates (at most kCreateWindowMax) go out
        // without waiting, the caller reads a provisional accept, and answers are collected in
        // order; a declined one un-latches its object (MGPipeNoteHandleUnpublished) and marks it
        // suspect, so its later creates block. It runs only on a link that keeps declared answers
        // until read (ILink::RetainsReplies). Measurements: docs/Performance/ipc-create-window.md.
        constexpr Uint32 kCreateWindowMax = 12;
        // kCreateWindowMax is at most half of StreamLink's declared-answer store
        // (kRetainedAnswersMax): the other half is for the one blocking row in flight and for
        // answers a retired window left behind. The window never runs on the slot-pool link
        // (ShmLink retains no replies), so depth costs store entries, not reply slots. Store
        // overflow is a named Fatal, never an eviction, and the store logs its peak: argue any
        // raise from that peak (docs/Performance/ipc-create-window.md).
        static_assert(kCreateWindowMax <= Transport::StreamLink::kRetainedAnswersMax / 2,
                      "the create window may hold at most half of StreamLink's declared-answer "
                      "store: the other half belongs to the one blocking row in flight and to "
                      "whatever a retired window has stranded. The store's overflow is a named "
                      "Fatal rather than an eviction, so this bound is about losing a run, not "
                      "about a wrong picture");
        // AND THE ENVIRONMENT MAY NOT RAISE IT EITHER. The configured value is clamped to the
        // array's own size, so the fixed array below cannot be overrun by a config bound and this
        // constant having drifted apart. 1 (and anything below it) is the pre-window shape.
        Uint32 CreateWindowEffective() {
            if (MG_Config::Ipc.CreateWindow <= 1) return 1;
            return MG_Config::Ipc.CreateWindow < kCreateWindowMax ? MG_Config::Ipc.CreateWindow
                                                                  : kCreateWindowMax;
        }
        struct PendingCreate {
            Uint64 Seq = 0;
            MG_Pipe::MGPipeKind Kind = MG_Pipe::MGPipeKind::None;
            MG_Pipe::MGPipeHandle Handle{};

        };
        PendingCreate g_createWindow[kCreateWindowMax];
        Uint32 g_createWindowHead = 0;
        Uint32 g_createWindowCount = 0;
        Uint64 g_createRefusals = 0;
        // How many deferred answers the window has actually TAKEN BACK, which is the one number
        // that says the deferral worked rather than merely happened. It is not inferable from the
        // others: `pending` staying at the ceiling is consistent BOTH with a drain that collects
        // one per create and with a drain that collects nothing while a server-skip keeps the
        // answers from existing at all - the second is what a no-reply bit keyed on wantReply
        // would produce, and it would leave every other reading green.
        Uint64 g_createWindowTaken = 0;
        // P12 MEASUREMENT: WHY A CREATE DOES NOT DEFER. The window's own arithmetic says a frame's
        // blocking takes should be creates/window (4,536/6 = 756 at the shipped default), and the
        // device measures 1,155 - a CONSTANT ~400 that does not move when the window deepens (at
        // window 8 the ideal is 567 and the measurement is 859). A constant is a fixed population
        // rather than a queueing effect, and the four exits below are the only ways into it, so
        // counting them by name is what says which one it is. It matters because the frame turned
        // out to be LATENCY-bound, not throughput-bound (the server applies all 103,097 records in
        // 1.95 s of a 9.64 s frame, and the client's 1,161 blocking takes at ~4.9 ms are 5.7 s of
        // it) - so the way to make it faster is fewer blocks, not a faster apply.
        Uint64 g_blockNoWindow = 0, g_blockSuspectsFull = 0, g_blockSuspectObject = 0,
               g_blockWindowStillFull = 0, g_blockTotal = 0;
        void NoteCreateBlocked(Uint64& reason) {
            ++reason;
            ++g_blockTotal;
            // Once per 256 blocks: a handful of lines per load frame, and the composition is
            // readable from any one of them.
            if (g_blockTotal % 256 == 0) {
                MGLOG_I("P12 create blocks: total=%llu no-window=%llu suspects-full=%llu "
                        "suspect-object=%llu window-still-full=%llu",
                        static_cast<unsigned long long>(g_blockTotal),
                        static_cast<unsigned long long>(g_blockNoWindow),
                        static_cast<unsigned long long>(g_blockSuspectsFull),
                        static_cast<unsigned long long>(g_blockSuspectObject),
                        static_cast<unsigned long long>(g_blockWindowStillFull));
            }
        }

        // A HANDLE WHOSE CREATE CAME BACK DECLINED IS NEVER WINDOWED AGAIN. Without this the window
        // would UNDO the repair it exists to keep: every later use of the object sends another create,
        // reads another provisional accept, and latches the object as published again - so a server
        // that keeps refusing (the consumer-gate asymmetry the audit found, or any bug) would be fed
        // fire-and-forget records for ever with the client believing all of them landed. With it, a
        // refused object leaves the window and its next create is BLOCKING, so the refusal reaches the
        // caller, the latch stays clear, and every converted row for that object is back on the loud
        // blocking path. The window is for objects that have never been refused.
        //
        // BOUNDED, AND THE BOUND IS THE SAFE DIRECTION: past kCreateSuspectMax suspects the window
        // switches itself off entirely, which is the pre-window shape.
        constexpr SizeT kCreateSuspectMax = 256;
        Vector<MG_Pipe::MGPipeHandle> g_createSuspects;

        Bool CreateIsSuspect(MG_Pipe::MGPipeHandle handle) {
            for (const MG_Pipe::MGPipeHandle& suspect : g_createSuspects) {
                if (suspect.Slot == handle.Slot && suspect.Gen == handle.Gen) return true;
            }
            return false;
        }
        void NoteCreateSuspect(MG_Pipe::MGPipeHandle handle) {
            if (CreateIsSuspect(handle) || g_createSuspects.size() >= kCreateSuspectMax) return;
            g_createSuspects.push_back(handle);
        }
        void ClearCreateSuspect(MG_Pipe::MGPipeHandle handle) {
            for (SizeT i = 0; i < g_createSuspects.size(); ++i) {
                if (g_createSuspects[i].Slot != handle.Slot || g_createSuspects[i].Gen != handle.Gen)
                    continue;
                for (SizeT j = i + 1; j < g_createSuspects.size(); ++j)
                    g_createSuspects[j - 1] = g_createSuspects[j];
                g_createSuspects.pop_back();
                return;
            }
        }

        // Which publication latch an object's create owns. The three resource kinds are spelled out
        // rather than defaulted, so a target added later cannot silently borrow the texture latch.
        MG_Pipe::MGPipeKind CreateRecordKind(const MG_Pipe::MGPResourceDesc& desc) {
            switch (static_cast<MG_Pipe::MGPipeResourceTarget>(desc.Target)) {
            case MG_Pipe::MGPipeResourceTarget::Buffer: return MG_Pipe::MGPipeKind::Buffer;
            case MG_Pipe::MGPipeResourceTarget::Renderbuffer: return MG_Pipe::MGPipeKind::Renderbuffer;
            default: return MG_Pipe::MGPipeKind::Texture;
            }
        }

        // Collect every answer that is already available. `blockForOldest` is the window being full,
        // which is the one moment an answer is worth waiting for - appliedSeq is monotone, so taking
        // the oldest is what frees the slot the next create needs. FIFO throughout: an unapplied
        // oldest means none of the rest are ready either.
        void DrainCreateWindow(ClientSession& session, Bool blockForOldest) {
            Bool blocking = blockForOldest;
            while (g_createWindowCount != 0) {
                const PendingCreate head = g_createWindow[g_createWindowHead];
                // THE WAIT THIS ROW PAYS IS PAID HERE (P12), so the metric has to see it HERE.
                // Without this, by_op 2 counts only the fallback emissions and a WORKING window
                // reads as "create stopped waiting" when the row had merely moved the waiting
                // into this loop - the false green the design review named. Counted for the
                // blocking take only: the poll is not a round trip, and it is the blocking take
                // that is the thing MOBILEGL_IPC_CREATE_WINDOW divides (4,536 -> creates/window).
                const Uint64 metricStart =
                    blocking ? Transport::LinkMetricsBeginReply(
                                   true, static_cast<std::uint32_t>(MGPWireOp::ResourceCreate))
                             : 0;
                const Transport::SessionWait wait =
                    session.WaitForApplied(head.Seq, blocking ? 120000u : 0u);
                if (blocking) Transport::LinkMetricsReplyApplied(metricStart);
                if (wait != Transport::SessionWait::Reached) return;
                Int32 status = Wire::ReplySink::kStatusError;
                Uint64 size = 0;
                // AN UNREADABLE SLOT IS NOT AN ERROR: ReadReply refuses when the stamp does not
                // match yet (or when the answer does not fit the scratch), and treating that as
                // ERROR would invent a transport fault out of a reader that arrived early. The
                // entry stays in the window and is taken at the next drain.
                if (!session.ReadReply(head.Seq, nullptr, 0, &status, &size)) {
                    // REACHED BUT UNREADABLE IS AN INVARIANT, NOT A RETRY (P12). The link declared
                    // it retains this answer, and on the stream arm the reply frame strictly
                    // precedes the progress frame that published appliedSeq - so by the time the
                    // wait above returns, the answer is in hand or something is broken. Retrying
                    // (what this used to do) wedges the window for the rest of the process and
                    // leaves every deferred object latched on a provisional accept; recovering by
                    // re-latching would lose texels. So it names itself instead.
                    SessionFail(MGFatalFamily::ReplyMissing,
                                "MGPipe: Fatal{ReplyMissing, \"create window\"} - seq %llu was "
                                "declared and APPLIED, and its answer is no longer in the link's "
                                "store; the window cannot advance past it and nothing recovers a "
                                "lost answer",
                                static_cast<unsigned long long>(head.Seq));
                }
                // ERROR IS NOT A DECLINE (M4 / R-5), and a deferred answer does not change that:
                // the same named Fatal the blocking take raises.
                if (status == Wire::ReplySink::kStatusError) {
                    SessionFail(MGFatalFamily::ReplyError, "MGPipe: Fatal{ReplyError, \"resource_create\"} - the row answered "
                            "ERROR, which is not an acceptance answer; folding it into accepted or "
                            "refused would make a transport fault look like a resource decision");
                }
                if (status == Wire::ReplySink::kStatusDeclined) {
                    MG_Pipe::MGPipeNoteHandleUnpublished(head.Kind, head.Handle);
                    NoteCreateSuspect(head.Handle);
                    ++g_createRefusals;
                } else {
                    // It landed: the object is published for real, so it is no longer suspect.
                    ClearCreateSuspect(head.Handle);
                }
                g_createWindowHead = (g_createWindowHead + 1) % kCreateWindowMax;
                --g_createWindowCount;
                ++g_createWindowTaken;
                blocking = false;
            }
        }
        // THE PRE-WINDOW SHAPE, AND THE HEAL BESIDE IT. Every path that cannot defer takes this
        // one: the window off, the object already refused, the suspect list full, and the window
        // still full after a blocking drain. The record waits and its caller reads the REAL
        // answer.
        //
        // AND AN ACCEPTED CREATE TAKES ITS OBJECT BACK OUT OF THE SUSPECT LIST. "Never windowed
        // again" is the answer to a REFUSAL, and a blocking create that landed means the refusal
        // is over - the object is published, by the caller's own latch, on the answer this call
        // just read. Leaving the entry standing would retire the object from the window for the
        // rest of the process, and kCreateSuspectMax such recoveries would retire the WINDOW
        // itself, which is the one way this row's speed-up can be lost with nothing going wrong.
        void EmitCreateBlocking(ClientSession& session, const MG_Pipe::MGPResourceDesc* payload,
                                MG_Pipe::MGPReplySlot* reply) {
            Int32 status = Wire::ReplySink::kStatusError;
            const Uint64 seq = session.EmitAndWait(MGPWireOp::ResourceCreate, payload,
                                                   sizeof(*payload), nullptr, 0, nullptr, 0,
                                                   &status);
            // Cancellation has no wire ordinal; retain the caller's nonzero local ticket.
            if (seq != Wire::kInvalidSeq) reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, status, status == 0 ? 1u : 0u);
            ++g_emitted;
            if (status == 1) ++g_declined;
            if (status == 0) ClearCreateSuspect(payload->Resource);
        }
        // The window keeps no client-side residency budget. On a link that reuses reply slots,
        // what displaces a deferred answer is the server's reply traffic, not this client's
        // emissions, so a budget counted here fires too late. The window defers only where the
        // link retains answers, and the store bounds it (docs/Performance/ipc-create-window.md).
        void Wire_ResourceCreate(const MG_Pipe::MGPResourceDesc* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithScreen().ResourceCreate(payload, reply); return; }
            ClientSession& session = RequireSession("ResourceCreate");
            // THE WINDOW MAY ONLY DEFER WHERE THE LINK RETAINS ANSWERS (P12). On one that does
            // not - the shared pool, whose slot is seq % slotCount and is reused - the answer the
            // drain comes back for may already be gone, and there is NO recovery: retrying wedges
            // the row for the rest of the process, and re-latching the object by sending another
            // create wipes the server's record along with its PendingUploads (MGPipeApplyResourceCreate,
            // MGPipeResourceRecord::PendingUploads) while the client cleared its dirty flags at emission
            // (PipeApply.h's PendingUpload comment), so nobody re-sends those texels. So it takes the blocking
            // path instead, which is the pre-window shape exactly.
            const Uint32 window = session.LinkRetainsReplies() ? CreateWindowEffective() : 1u;
            // P15: UNDER RUN-AHEAD A CREATE IS FIRE-AND-FORGET. The handle is the client's own
            // allocation, and the server's accept is derivable: it declines only a reserved or
            // out-of-table slot or a target with no table (all protocol corruption, trip-wired on
            // the server) or a texture family with no consumer, which the client already knows
            // and never emits for (P4aFamilyHasItsConsumer). Waiting for that answer was a
            // blocking round trip per new object (FCL + BSL: 2-3 per frame).
            // A link that retains answers (the stream arm) keeps its create window, which already
            // defers the answer and reads it later; this is the arm without one (the shared pool).
            if (window <= 1 && session.RunAheadArmed() && !CreateIsSuspect(payload->Resource)) {
                Int32 status = Wire::ReplySink::kStatusError;
                const Uint64 seq = session.EmitAndWaitTails(
                    MGPWireOp::ResourceCreate, payload, sizeof(*payload), nullptr, 0, nullptr, 0,
                    &status, nullptr, /*wantReply=*/false, /*willReadReply=*/false);
                if (seq != Wire::kInvalidSeq) reply->Id = seq;
                MG_Pipe::MGPipePostReply(*reply,
                                         seq == Wire::kInvalidSeq ? Wire::ReplySink::kStatusDeclined
                                                                  : Wire::ReplySink::kStatusOk,
                                         seq == Wire::kInvalidSeq ? 0u : 1u);
                ++g_emitted;
                if (seq == Wire::kInvalidSeq) ++g_declined;
                return;
            }
            // THE WINDOW OFF RESTORES THE OLD SHAPE EXACTLY, and is the negative control for it.
            // THE WINDOW OFF, THE SUSPECT LIST FULL, OR AN OBJECT THAT HAS ALREADY BEEN REFUSED:
            // all four take the blocking branch, which is the pre-window shape - and on the device
            // this is the branch the whole row takes, because the deferral above it cannot be
            // collected. See the section comment for the measurement and the arithmetic.
            if (window <= 1) {
                NoteCreateBlocked(g_blockNoWindow);
            } else if (g_createSuspects.size() >= kCreateSuspectMax) {
                NoteCreateBlocked(g_blockSuspectsFull);
            } else if (CreateIsSuspect(payload->Resource)) {
                NoteCreateBlocked(g_blockSuspectObject);
            }
            if (window <= 1 || g_createSuspects.size() >= kCreateSuspectMax ||
                CreateIsSuspect(payload->Resource)) {
                // THE WINDOW RETIRES HERE, AND IT MAY BE HOLDING ANSWERS (P12). This branch is
                // taken when the window is off, when the suspect list is full, or for a refused
                // object - and in the last two cases entries already emitted are still in flight.
                // Without this drain they arrive, are retained, and are never read, WHILE their
                // objects stay latched on a provisional accept: the silent divergence the
                // suspect list exists to prevent. One blocking pass settles them.
                if (g_createWindowCount != 0) DrainCreateWindow(session, /*blockForOldest=*/true);
                EmitCreateBlocking(session, payload, reply);
                return;
            }
            // Take what is already answerable, and if the window is full take the oldest for real.
            // The poll is not politeness: it is what keeps a burst shorter than the window from
            // leaving its answers unread until the next burst.
            DrainCreateWindow(session, /*blockForOldest=*/false);
            if (g_createWindowCount >= window) {
                DrainCreateWindow(session, /*blockForOldest=*/true);
                // IF IT IS STILL FULL the oldest answer is not readable yet, and the window is a
                // fixed array: pushing would silently OVERWRITE a confirmation. This record takes
                // the blocking path instead - slower for this one call, never lossy. Reaching it
                // at all means the oldest answer did not arrive inside the drain's own budget, so
                // the session is on its way out; it is a net, not a hot path. The comparison is
                // against the effective window, which is what the array below is sized for, so
                // this line IS the overwrite guard.
                if (g_createWindowCount >= window) {
                    NoteCreateBlocked(g_blockWindowStillFull);
                    EmitCreateBlocking(session, payload, reply);
                    return;
                }
            }
            Int32 status = Wire::ReplySink::kStatusError;
            // willReadReply IS TRUE AND THAT IS THE WHOLE POINT OF THE WINDOW: this record does
            // not wait for its answer, and the drain above reads it later. A P12 no-reply bit
            // keyed on wantReply would tell the server to skip the very answer this row comes
            // back for (docs/.../p12/CREATE-WINDOW-MEASURED.md section 7).
            const Uint64 seq = session.EmitAndWaitTails(
                MGPWireOp::ResourceCreate, payload, sizeof(*payload), nullptr, 0, nullptr, 0,
                &status, nullptr, /*wantReply=*/false, /*willReadReply=*/true);
            if (seq == Wire::kInvalidSeq) {
                // Cancelled: there is no ordinal to read an answer from, and the caller must NOT
                // latch an object the record never described.
                MG_Pipe::MGPipePostReply(*reply, Wire::ReplySink::kStatusDeclined, 0u);
                ++g_emitted;
                ++g_declined;
                return;
            }
            g_createWindow[(g_createWindowHead + g_createWindowCount) % kCreateWindowMax] =
                PendingCreate{seq, CreateRecordKind(*payload), payload->Resource};
            ++g_createWindowCount;
            reply->Id = seq;
            MG_Pipe::MGPipePostReply(*reply, Wire::ReplySink::kStatusOk, 1u);
            ++g_emitted;
        }

        // set_texture_params owes no answer once the texture is published. MGPipeHandleIsPublished
        // is the client's copy of "the applier holds this handle": set when the create is accepted
        // and kept while the record lives (MGPipeApplierReset keeps texture records). A published
        // texture's record goes fire-and-forget with a provisional accept. An unpublished one still
        // waits, so a refusal reaches the caller as a refusal. EmitTextureParams pays the
        // confirming create at the texture's first use (HealUnpublishedRecord).
        void Wire_SetTextureParams(const MG_Pipe::MGPTextureParams* payload, MG_Pipe::MGPReplySlot* reply) {
            if (RunsAsTheServerRole()) { MG_Pipe::MGPipeMonolithContext().SetTextureParams(payload, reply); return; }
            ClientSession& session = RequireSession("SetTextureParams");
            const Bool confirmed = MG_Pipe::MGPipeHandleIsPublished(MG_Pipe::MGPipeKind::Texture,
                                                                   payload->Res);
            Int32 status = Wire::ReplySink::kStatusError;
            Uint64 seq = Wire::kInvalidSeq;
            if (confirmed) {
                seq = session.EmitAndWaitTails(MGPWireOp::SetTextureParams, payload, sizeof(*payload),
                                               nullptr, 0, nullptr, 0, &status, nullptr,
                                               /*wantReply=*/false, /*willReadReply=*/false);
                status = seq == Wire::kInvalidSeq ? Wire::ReplySink::kStatusDeclined
                                                  : Wire::ReplySink::kStatusOk;
            } else {
                seq = session.EmitAndWait(MGPWireOp::SetTextureParams, payload, sizeof(*payload),
                                          nullptr, 0, nullptr, 0, &status);
            }
            // Cancellation has no wire ordinal; retain the caller's nonzero local ticket.
            if (seq != Wire::kInvalidSeq) reply->Id = seq;
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
            // it, which ReplySlotPool's seq-stamped slots make legal. A row whose acceptance a caller USES
            // may never take this path - R-5 has not moved.
            const Bool wantReply = MG_Pipe::MGPipeSubDataWantsItsReply(record);
            Int32 status = 0;
            const Wire::WireTail tail{varTail, static_cast<Uint64>(varTailCount) *
                                                   sizeof(MG_Pipe::MGPSubRegion)};
            const Uint64 seq = session.EmitAndWaitTails(
                MGPWireOp::ResourceSubData, &record, sizeof(record),
                varTail != nullptr ? &tail : nullptr, varTail != nullptr ? 1u : 0u, nullptr, 0,
                &status, nullptr, wantReply,
                // ONE ANSWER TO BOTH QUESTIONS, and on this row that is not a shortcut: the
                // wire arm's MGPipeSubDataWantsItsReply is false for the whole texture half
                // (PipeRoute.h), the caller discards the Bool either way, and the record's own
                // comment above already says "nothing reads it". A buffer-half caller that DID
                // read it would pass wantReply=true and get its answer.
                wantReply);
            // Cancellation has no wire ordinal; retain the caller's nonzero local ticket.
            if (seq != Wire::kInvalidSeq) reply->Id = seq;
            const Int32 postedStatus = (seq == Wire::kInvalidSeq || status == Wire::ReplySink::kStatusDeclined)
                                           ? Wire::ReplySink::kStatusDeclined
                                           : wantReply ? status : Wire::ReplySink::kStatusOk;
            MG_Pipe::MGPipePostReply(*reply, postedStatus, postedStatus == Wire::ReplySink::kStatusOk ? 1u : 0u);
            if (seq != Wire::kInvalidSeq) ++g_emitted;
            if (postedStatus == Wire::ReplySink::kStatusDeclined) ++g_declined;
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
                SessionFail(MGFatalFamily::UncarriedInitialBytes, "MGPipe: Fatal{UncarriedInitialBytes, \"resource_respecify\"} - a call "
                        "site handed initial content to a split respecify. R-13.3 rules that "
                        "initialBytes never crosses and that the content follows as "
                        "resource_subdata; a caller that still passes it has bytes nothing will "
                        "carry");
            }
            // Scope and exact mutable mip extent travel together through the descriptor helper.
            // It temporarily reuses BufOffset/BufSize on non-buffer image targets; the server
            // checks and clears that carrier before persisting the resource descriptor.
            MG_Pipe::MGPResourceDesc record = *desc;
            if (level != nullptr) {
#if MOBILEGL_BUILD_DISAGGREGATED
                MG_Pipe::MGPipeSetRespecifiedLevel(record, level->UploadTarget, level->Level,
                                                   level->Width, level->Height, level->Depth);
#else
                MG_Pipe::MGPipeSetRespecifiedLevel(record, level->UploadTarget, level->Level);
#endif
            } else {
                MG_Pipe::MGPipeClearRespecifiedLevel(record);
            }
            // Whether this record waits is MGPipeResourceRespecifyWantsItsReply, shared with the
            // route (ARCHITECTURE.md §13.1.1). On the texture half a provisional accept advances
            // the emitter's acceptance mirror (entry.LastDesc, TextureEmit.h) without a verdict.
            // That is safe only because the texture is published: the server accepted its create
            // and keeps its record. The one exception, the server dropping a live record
            // (MGPipeApplierReleaseObjectRecords), has no production caller.
            const Bool wantsReply = MG_Pipe::MGPipeResourceRespecifyWantsItsReply(record);
            Int32 status = Wire::ReplySink::kStatusError;
            if (!wantsReply) {
                const Uint64 seq = session.EmitAndWaitTails(
                    MGPWireOp::ResourceRespecify, &record, sizeof(record), nullptr, 0, nullptr, 0,
                    &status, nullptr, /*wantReply=*/false, /*willReadReply=*/false);
                status = seq == Wire::kInvalidSeq ? Wire::ReplySink::kStatusDeclined
                                                  : Wire::ReplySink::kStatusOk;
            } else {
                (void)session.EmitAndWait(MGPWireOp::ResourceRespecify, &record, sizeof(record),
                                          nullptr, 0, nullptr, 0, &status);
            }
            ++g_emitted;
            // P11 B2: A RESPECIFIED BUFFER'S T0 STORE IS DONE FOR THIS CLIENT. The frontend has
            // already dropped its adopted pointer (BufferObject::RedefineStorage) and the store's
            // replacement is minted by the next map_persistent; the server keeps its own
            // reference until its fence for the store's last use has completed, so releasing ours
            // here cannot pull pages from under a frame still in flight (the fire-and-forget arm
            // above included).
            if (desc->Target == MG_Pipe::kMGPipeResourceTargetBuffer) session.ReleaseT0Store(desc->Resource);
            // ERROR IS NOT A DECLINE (M4 / codex 5 / R-5). status is 0 OK / 1 DECLINED / 2 ERROR;
            // an escape may not fold 2 into `false`, which is what a bare `return status == 0`
            // did - a transport fault then read as "the server said no". Every reply-owning row
            // - the generated acceptance rows through MGPipeTakeReplyBool, and now these two
            // escapes - answers ERROR with the same named Fatal.
            if (status == 2) {
                SessionFail(MGFatalFamily::ReplyError, "MGPipe: Fatal{ReplyError, \"resource_respecify\"} - the row answered "
                        "ERROR, which is not an acceptance answer; folding it into accepted or "
                        "refused would make a transport fault look like a resource decision");
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
            // P11 B2 (CONTRACT-P11.md B2): A T0 SESSION ADOPTS FOR REAL. The store is an
            // AHardwareBuffer this client allocates, holds locked and seeds from the shadow
            // (`seedBytes`, `size` of them); the server imports it and answers OK, and the held
            // pointer is the mapping. Everything below is the T2 arm, unchanged byte for byte.
            if (session.AdoptT0()) {
                void* adopted = session.AdoptPersistentT0(*handle, size, seedBytes);
                ++g_emitted;
                if (adopted == nullptr) ++g_declined;
                return adopted;
            }
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
                SessionFail(MGFatalFamily::ReplyError, "MGPipe: Fatal{ReplyError, \"map_persistent\"} - the row answered ERROR, "
                        "which is not an acceptance answer; a transport fault is not a resource "
                        "decision and may not be folded into the decline R-6 predicts");
            }
            if (status == 1) ++g_declined;
            // NOT "always nullptr": the answer is READ. R-6 says the server declines, and the
            // day it stops declining this returns what it actually said rather than what the
            // ruling predicted.
            if (status == 0) {
                SessionFail(MGFatalFamily::UnexpectedMapAccept, "MGPipe: Fatal{UnexpectedMapAccept, \"map_persistent\"} - the server "
                        "accepted a persistent map under split. R-6 makes the split answer a "
                        "constant decline because there is no way to hand a host pointer across "
                        "a process boundary in P5; a pointer arriving here is one this client "
                        "cannot dereference");
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
                SessionFail(MGFatalFamily::ArtefactsMissing, "MGPipe: Fatal{ArtefactsMissing, \"create_shader_state\"} - the record's "
                        "two typed companions are null. Under monolith the applier reads the "
                        "modules out of spirv->generatedSpirv; under split there is nothing to "
                        "serialise, and emitting the record anyway would create a CSO with no "
                        "code");
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
                SessionFail(MGFatalFamily::ArchiveEmpty, "MGPipe: Fatal{ArchiveEmpty, \"create_shader_state\"} - the program's "
                        "artefacts serialised to nothing");
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

    // P14 S1: the EGL context lifecycle's two frames. NO SESSION IS AN ANSWER, not a Fatal:
    // eglCreateContext/eglDestroyContext can legally run before ClientSession::Start (the
    // bring-up window) and on the server role's own thread, and in both there is no wire for the
    // frame to cross - the caller falls back to what it did before this slice, which is to keep
    // the client's own EGL books and nothing else.
    namespace {
        // The window test both senders below open with, and the reason it is a function rather
        // than two copies of the same if: a sender that answered Refused for "no wire" would make
        // eglCreateContext return EGL_NO_CONTEXT during the bring-up, which is the exact failure
        // the pre-Start exception exists to prevent.
        Bool ContextFramesCanCross() {
            ClientSession* session = ClientSession::Active();
            return session != nullptr && session->Started() &&
                   !g_clientTablesUninstalled.load(std::memory_order_acquire);
        }
    } // namespace

    ContextFrameEmit SendCreateContextFrame(Uint64 clientContextToken, Uint64 shareGroupToken, Uint32 flags) {
        if (!ContextFramesCanCross()) return ContextFrameEmit::NoWire;
        return Server::ServerCreateEGLContext(clientContextToken, shareGroupToken, flags) ? ContextFrameEmit::Sent
                                                                                         : ContextFrameEmit::Refused;
    }

    ContextFrameEmit SendDestroyContextFrame(Uint64 clientContextToken) {
        if (!ContextFramesCanCross()) return ContextFrameEmit::NoWire;
        return Server::ServerDestroyEGLContext(clientContextToken) ? ContextFrameEmit::Sent
                                                                   : ContextFrameEmit::Refused;
    }

    // P14 S1: bind_context. The same four windows as EmitApplierResetRecord below answer false,
    // and for the same reason: a binding that cannot cross leaves the session on the default it
    // already has (one context per session, every record its own), which is what the server
    // answers with no record at all. The token is minted by EGLState and passed in rather than
    // read here, so this table holds no second opinion about which context is current.
    Bool EmitBindContextRecord(Uint64 clientContextToken) {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started()) return false;
        if (g_clientTablesUninstalled.load(std::memory_order_acquire)) return false;
        MG_Pipe::MGPBindContext record{};
        record.ClientContextToken = clientContextToken;
        const Uint64 seq = session->EmitAndWait(MGPWireOp::BindContext, &record, sizeof(record), nullptr, 0,
                                                nullptr, 0, nullptr);
        ++g_emitted;
        // A DECLINED binding moved nothing on the server (a lost session, or a thread whose
        // context is from an ended one), so the caller must not believe the session is bound to
        // it: the next thread that writes re-binds its own context.
        return seq != Wire::kInvalidSeq;
    }

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
        record.ContextSerial = g_applierResetContextSerial;
        const Uint64 seq = session->EmitAndWait(MGPWireOp::ApplierReset, &record, sizeof(record), nullptr, 0,
                                                nullptr, 0, nullptr);
        // THE SERIAL COUNTS RESETS THE SERVER WAS SENT, not resets this client primed: a declined
        // record (a lost session, a thread on an ended session's context) never reached the
        // server's count, and moving ours past it would latch the next session that hears one.
        if (seq != Wire::kInvalidSeq) ++g_applierResetContextSerial;
        ++g_emitted;
        return true;
    }

    void ForgetWireTablesOfEndedSession() {
        // The ended session's deferred create answers are not coming: their seqs name a ring
        // that is gone, and a drain on the new session would wait for them (or read another
        // record's answer). The objects behind them are the ended session's too.
        g_createWindowHead = 0;
        g_createWindowCount = 0;
        g_createSuspects.clear();
        // A fresh session's server counts applier resets from 0.
        g_applierResetContextSerial = 0;
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
            SessionFail(MGFatalFamily::ClientTablesUninstalled, "MGPipe: Fatal{ClientTablesUninstalled, \"%s\"} - the client tables are "
                    "being torn down; refusing before session or ring access", row);
        }
    }

    namespace {
        void WireStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId,
                                      MG_Pipe::MGPipeHandle /*handle*/) {
            (void)EmitObjectDeathRecord(kind, lifetimeId);
        }

        const MG_State::GLState::StateObjectDeathOps kClientStateObjectDeathOps = {
            .OnDestroyed = &WireStateObjectDestroyed,
        };
    }

    void InstallClientWireTables() {
        using namespace MG_Pipe;

        // A remote client has no backend, so nothing else installs a state-object death notice
        // and NotifyAndFree would free slots without emitting object_death. Install the wire
        // emitter only under Transport == Spawn (inproc keeps its backend's dispatcher) and only
        // when no ops are installed (the first installer keeps the notice). Magma's own ops
        // (DirectVulkan.cpp) cannot collide with these through the last-writer-wins global: Magma
        // installs only in a process that owns a DirectVulkan backend, and a spawn client owns
        // none (InitSplitRoles builds a BackendObject_Remote). CtWireScenario's two death cases
        // cover both installs.
        if (MG_Config::Transport == MG_Config::TransportMode::Spawn &&
            MG_State::GLState::GetStateObjectDeathOps() == nullptr) {
            MG_State::GLState::SetStateObjectDeathOps(&kClientStateObjectDeathOps);
        }

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
        if (MG_State::GLState::GetStateObjectDeathOps() == &kClientStateObjectDeathOps) {
            MG_State::GLState::SetStateObjectDeathOps(nullptr);
        }
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

    // ---- P12: the CREATE WINDOW, as numbers a test can read ---------------------------------
    //
    // The window is a DEFERRAL, so the property that has to be checkable is not "is it faster"
    // but "was every deferred answer eventually taken, and did the window ever hold more than it
    // is sized for". Pending is that second question, and it is the one a silent overwrite would
    // answer wrongly: the ring wraps, so an unguarded push would not grow the count past the
    // ceiling - it would REPLACE the oldest confirmation and leave the count where it was. Read it
    // against the effective ceiling and the array size, which are the two numbers the guard itself
    // is written in terms of, and not against a literal repeated in the test.
    Uint32 ClientCreateWindowPending() { return g_createWindowCount; }
    // Taken + Pending is every create the window ever accepted: the answers it has read back and
    // the ones still outstanding. A create the window accepted and then LOST would break that
    // sum, which is the failure a provisional accept makes possible and a count cannot show.
    Uint64 ClientCreateWindowTaken() { return g_createWindowTaken; }
    // P12: RetainedOutstanding / SkippedUnwanted / UnansweredWanted live on the LINK, where the
    // store is (StreamLink::RetainedOutstanding and friends). They are what says whether the
    // declared-answer store is bounded by the CLIENT (SkippedUnwanted grows, outstanding does
    // not) rather than by the server's answer rate - which is the property the capacity rests on.
    Uint32 ClientCreateWindowEffective() { return CreateWindowEffective(); }
    Uint32 ClientCreateWindowArraySize() { return kCreateWindowMax; }
    // Objects whose create came back DECLINED: out of the window, next create blocking, and back
    // in the moment a blocking create lands.
    Uint32 ClientCreateWindowSuspects() { return static_cast<Uint32>(g_createSuspects.size()); }
    Uint64 ClientCreateWindowRefusals() { return g_createRefusals; }

} // namespace MobileGL::MG_Remote::Client

#endif // MOBILEGL_BUILD_DISAGGREGATED
