// MobileGL - MobileGL/MG_Remote/Client/EmitTables.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 package c1: the 71-slot emit table.
//
// THE PARTITION IS CONTRACT-P5.md §7's AND IS NOT RE-DERIVED HERE (R-15, ID-12):
//   class A  2 slots  answered locally from the caps mirror, never emitted, never Fatal
//   class B 54 slots emitted; class C 15 slots name their unmigrated verb.
// The three counts are static_asserted to sum to kRemoteEmitSlotCount below, so a slot that
// changes class without changing the arithmetic is a build break rather than a behaviour
// change nobody reviewed.
//
// P5b MOVES SLOTS FROM C TO B, ONE PACKAGE AT A TIME (MG_Remote/CONTRACT-P5B.md §7). The three
// numbers above are the partition AT THE P5b CONTRACT COMMIT and they are the ones the contract
// states; the arithmetic below is what the tree currently has, and the per-package ownership
// assertions say which package moved which slot. On this head t2 has landed: class B is 5 + 6
// and class C is 58.
//
// THE PRE-VERB HOOKS RUN BEFORE THE RECORD, NEVER AFTER (b1, ID-18). PushPersistentMapsBeforeVerb
// publishes the bytes an application wrote through a coherent map with no API call at all, and
// MarkGpuWritesForDraw builds the conservative GPU-write set the client now owns. Both describe
// the work the record is ABOUT TO START, so a hook deferred past its own record is the C-1
// regression re-committed at the transport layer.

#include "EmitTables.h"
#include <MG_Remote/FatalFunnel.h>

#include "ClientSession.h"
#include "GpuWritePending.h"
#include <MG_Impl/Pipe/Verb/VerbPort.h>
#include <MG_State/GLState/BufferState/PersistentMapTracker.h>

#include "../Server/ServerLoop.h"
#include <MG_Backend/BackendObjects.h>

#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <MG_Util/Metrics/TextureMetrics.h>

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
// P5b i1: the emitters below name a texture's, a buffer's and a program's HANDLE beside the GL
// arguments (rule D). The handle comes from the client's own slot allocator, which is
// MG_Impl/Pipe's - the same table MG_Impl/Pipe/ImageEmit.h's set_shader_images reads, so the
// two records name one identity rather than two.
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>

// P5b d1: the handle a bound buffer already has (never minted here - the validate-time
// set_index_buffer / the buffer's own constructor did that), for MGPDrawInfo::IndexResource and
// the two indirect-buffer handles.
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/GLImpl/Texture/MipmapGenerationPlan.h>
#include <MG_Impl/Pipe/OwnedDrawInputs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "WireTables.h"
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/TextureEmit.h>
#include <MG_Impl/Pipe/ProgramEmit.h>
#include <MG_Pipe/PipeMutation.h>

namespace MobileGL::MG_Remote::Client {

    // P13 W5: the pre-verb hook and the E2 evidence line are the verb port's (VerbPort.h).
    using MG_Record::BeforeReadOnlyVerb;
    using MG_Record::LogE2ControlLine;

    // The slot arithmetic, asserted rather than commented. GlobalBackendFunctionsTable is
    // GLFunctionsTable plus Present plus SetSwapInterval; GLFunctionsTable is 69 function
    // pointers plus one Bool (PrefersCpuXfbPrimitiveAccounting, BackendObject.h:274). A slot
    // added to either without a decision here is a build break, which is the point: R-4 forbids
    // a null slot, so a new slot needs an owner on the day it appears.
    static_assert(sizeof(MG_Backend::GlobalBackendFunctionsTable) ==
                      sizeof(MG_Backend::GLFunctionsTable) + 2 * sizeof(void (*)()),
                  "GlobalBackendFunctionsTable is no longer GLFunctionsTable + Present + SetSwapInterval");
    static_assert(sizeof(MG_Backend::GlobalBackendFunctionsTable) ==
                      kRemoteEmitSlotCount * sizeof(void (*)()) + sizeof(void (*)()),
                  "the emit table's 71 slots plus the packed Bool no longer describe the table");

    namespace {

        Uint64 g_presentOrdinal = 0;
        Uint64 g_publishedMaxRecordBytes = 0;

        // ---- the session, demanded rather than assumed --------------------------------
        //
        // Every class-B slot needs one. A null session here is NOT the monolith answer - the
        // monolith answer is that this table was never installed at all, because
        // MG_Backend::Init() only reaches BackendObject_Remote when the transport resolved. So
        // a null one is a Fatal by name and not a fall-through to the driver: a pass-through
        // slot is the "split lane ran monolith and went green" shape that every gate in this
        // phase exists to prevent (R-4).
        ClientSession& RequireSession(const char* slot) {
            RequireClientTablesInstalled(slot);
            ClientSession* session = ClientSession::Active();
            if (session == nullptr) {
                // @Ph-declined (ID-P7-1): returns ClientSession& and runs in the CLIENT - no
                // session to hand back, no peer bytes, and the latch is a server-session idea.
                SessionFail(MGFatalFamily::NoClientSession, "MGPipe: Fatal{NoClientSession, \"%s\"} - the remote emit table is "
                        "installed but no ClientSession is active. A slot may not fall through "
                        "to a driver this role does not have",
                        slot);
            }
            return *session;
        }

        // P13 W5: the session as the record arm's VerbSession. The emitters live in MG_Record
        // (MG_Impl/Pipe/Verb/VerbPort.cpp) and run without a transport too; under one they reach
        // the session through this adapter, which RequireSession has just proved is there.
        class ClientVerbSession final : public MG_Record::VerbSession {
        public:
            Uint64 EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes, const void* varTail,
                               Uint64 varTailBytes, void* replyOut, Uint64 replyBytes, Int32* statusOut,
                               Uint64* replySizeOut) override {
                return ClientSession::Active()->EmitAndWait(op, payload, payloadBytes, varTail, varTailBytes, replyOut,
                                                            replyBytes, statusOut, replySizeOut);
            }
            Uint64 EmitAndWaitTails(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                    const MG_Pipe::MGPipeVerbTail* tails, Uint32 tailCount, void* replyOut,
                                    Uint64 replyBytes, Int32* statusOut, Uint64* replySizeOut) override {
                return ClientSession::Active()->EmitAndWaitTails(op, payload, payloadBytes, tails, tailCount, replyOut,
                                                                 replyBytes, statusOut, replySizeOut);
            }
            Uint32 MaxReplyBytes() const override { return ClientSession::Active()->MaxReplyBytes(); }
            void RequireReadPixelsReplyFits(Uint32 width, Uint32 height, Uint32 format, Uint32 type,
                                            Uint64 bytes) const override {
                ClientSession::Active()->RequireReadPixelsReplyFits(width, height, format, type, bytes);
            }
            MG_Pipe::MGPBlobRef StageBytes(const void* bytes, Uint64 size) override {
                return ClientSession::Active()->Encoder().StageBytes(bytes, size);
            }
        };

        MG_Record::VerbSession& ResolveVerbSession(const char* slot) {
            (void)RequireSession(slot);
            static ClientVerbSession adapter;
            return adapter;
        }

        // Installed at static init, like the record seam's hooks (FatalFunnel.cpp): a library with
        // MG_Remote linked resolves the emitters' session here before any verb can run.
        [[maybe_unused]] const bool g_verbSessionResolverInstalled = [] {
            MG_Record::SetVerbSessionResolver(&ResolveVerbSession);
            return true;
        }();

        void EmitPresent() {
            ClientSession& session = RequireSession("Present");
            BeforeReadOnlyVerb();

            // ---- R-10's and R-9's readings, published BEFORE the present record ------------
            //
            // THE FRAME BOUNDARY IS THE RIGHT PLACE and the per-record path is the wrong one:
            // the encoder keeps all five as run totals, so this is five relaxed stores per
            // frame rather than five per record. Guarded by Enabled() like every other counting
            // site in the tree, so the cost with MOBILEGL_PIPE_STATS unset is a global load and
            // a predicted branch.
            //
            // AND IT IS BEFORE THE EmitAndWait BELOW, WHICH IS NOT A DETAIL. PipeStats::OnPresent
            // is called by the SERVER's Present - i.e. from inside the apply of the very record
            // this function is about to emit - so a publish placed after it lands one frame
            // late, and the FIRST summary line of every run then reads `maxrec=0 maxcap=0`.
            // Measured that way once: a zero that means "not published yet" is printed in the
            // same shape as a zero that means "nothing crossed", and the second one is a real
            // defect (an emit table that fell through to the driver). The Present record is 24
            // bytes and cannot be the maximum, so nothing is lost by reading one record early.
            if (MG_Util::PipeStats::Enabled()) {
                const Wire::PipeWireEncoder& encoder = session.Encoder();
                using MG_Util::PipeStats::Gauge;
                MG_Util::PipeStats::PublishGauge(Gauge::MaxRecordBytes, encoder.MaxRecordBytesSeen());
                MG_Util::PipeStats::PublishGauge(Gauge::MaxRecordBytesCap, encoder.MaxRecordBytesCap());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWraps, encoder.CmdWraps());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWrapPads, encoder.CmdWrapPads());
                MG_Util::PipeStats::PublishGauge(Gauge::RingWaits, encoder.StageReclaimWaits());

                // P5d round 3's wait ledger, CLIENT HALF, and it rides this frame boundary for
                // the same two reasons the five above it do: the producer keeps both as run
                // totals, so it is two relaxed stores per frame rather than two per wait, and
                // publishing before the present record means the first summary line of a run
                // carries real numbers instead of two zeroes that mean "not published yet".
                // The SERVER half is published by the apply thread from its own loop - rule E
                // does not let this thread read ServerLoop's counters.
                const Transport::SessionProducer& producer = session.Producer();
                MG_Util::PipeStats::PublishGauge(Gauge::ClientWaits, producer.Waits());
                MG_Util::PipeStats::PublishGauge(Gauge::ClientParks, producer.Parks());

                // AND THE ROW, WHENEVER THE MAXIMUM MOVES. The summary line can carry the
                // number but not the name - MG_Util is below MG_Remote and has no WireOpName -
                // and the name is the actionable half: R-10 makes the integrator choose between
                // a cut for that row and a bigger ring, and that is a decision about a record
                // FAMILY. ClientSession::Stop prints the same pair at teardown, but a trace
                // replay never reaches it (measured: the OpenRA lane's library log ends mid-run
                // with no teardown line at all), so a stats-enabled run would otherwise publish
                // a size with no row. Emitted only when the maximum actually grows, so it is
                // bounded by the number of distinct maxima - five or six in a whole replay.
                if (encoder.MaxRecordBytesSeen() > g_publishedMaxRecordBytes) {
                    g_publishedMaxRecordBytes = encoder.MaxRecordBytesSeen();
                    MGLOG_I("MGPipe: wire ledger: new maximum record - maxrec=%llu "
                            "maxrecop=%s cap=%llu (R-10's proof obligation; blobs are cut, a record is not)",
                            static_cast<unsigned long long>(g_publishedMaxRecordBytes),
                            encoder.MaxRecordOpName(),
                            static_cast<unsigned long long>(encoder.MaxRecordBytesCap()));
                }
            }

            MG_Pipe::MGPPresent record{};
            // P5e (ra, CONTRACT-P5E §1, §2.4). FrameSerial USED TO BE 0 - "the server stamps
            // its own" - and that was honest while nothing paced on it. It is now minted here,
            // 1-based, by AcquirePresentCredit, which also PAYS the credit: if this client
            // already has MOBILEGL_IPC_PRESENT_CREDIT presents in flight it parks until the
            // server's OnPresent returns one, and only then does it mint.
            //
            // THE CREDIT WAIT IS BEFORE THE ENCODE, WHICH IS NOT A DETAIL: EmitAndWait
            // reserves SEG_CMD bytes as its first act, so a client that encoded and then
            // parked would hold a ring reservation across a whole frame of server time. It
            // also drains SEG_EVENT on its way out, like every other wait (§2.6).
            //
            // With run-ahead disarmed this is a counter and nothing else, and the record below
            // travels exactly as it did - the present row's own barrier is the pacing there.
            record.FrameSerial = session.AcquirePresentCredit();
            record.DamageCount = session.TakePendingPresentDamage(record.Damage);
            session.EmitAndWait(MG_Pipe::MGPWireOp::Present, &record, sizeof(record), nullptr, 0,
                                nullptr, 0, nullptr);

            // R-12's invalidation edge, drained at the one boundary every target crosses. A
            // second CapsSnapshot IS the invalidation; nothing else on the client can see that
            // the server re-ran InitCapabilities, because GLContext::GetCompileEnv()'s memo is
            // keyed on pActiveBackendObject.get() (Core.cpp:34) and that pointer never changes
            // under split.
            session.PumpControlPlane();

            ++g_presentOrdinal;
            LogE2ControlLine(g_presentOrdinal);
        }

        // =============================================================================
        // CLASS A - answered locally from the caps mirror (R-15). NO RECORD, EVER.
        // =============================================================================

        void AnswerGetIntegeri_v(GLenum target, GLuint index, GLint* data) {
            if (data == nullptr) return;
            const MG_Backend::DynamicBackendParameters& dynamic = CapsMirrorInstance().Dynamic();
            // The ONLY two indexed pnames the device owns; every other indexed pname names
            // frontend state and is answered in GL_Getter::GetIntegeri_v before any table is
            // consulted (BackendObject.h:196-205). The existing gate is
            // AdvertisedLimitsScenario.ComputeWorkGroupLimitsAreTheCapsBlocksAnswer, which pins
            // that this answer and the caps copy are ONE number.
            switch (target) {
            case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
                if (index < 3) *data = static_cast<GLint>(dynamic.MaxComputeWorkGroupCount[index]);
                return;
            case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
                if (index < 3) *data = static_cast<GLint>(dynamic.MaxComputeWorkGroupSize[index]);
                return;
            default:
                // Not a Fatal: the slot's own contract is "whatever pname the frontend has no
                // case for at all", and the monolith backends answer such a pname by leaving
                // the driver's own default in place. Answering a wrong number would be worse
                // than answering none.
                MGLOG_W_ONCE("MG_Remote client: GetIntegeri_v(0x%04x, %u) is not one of the two "
                             "device-owned indexed pnames and has no caps-mirror answer",
                             static_cast<unsigned>(target), static_cast<unsigned>(index));
                return;
            }
        }

        Bool AnswerIsTimerQuerySupported() {
            // A capability predicate, not a call. Today a null slot means COUNTER_BITS == 0
            // (GL_Query.cpp:792) - which is precisely the null check R-4 forbids, so it moves
            // here, to the bit the server published.
            return CapsMirrorInstance().HasCap(MG_Pipe::kCapTimerQuery);
        }

        // =============================================================================
        // The frontend sees a local opaque token; neither this address nor the driver's
        // BackendSyncHandle is serialized. The Fence-kind slot allocator supplies wire identity.
        struct RemoteFenceProxy {
            MG_Pipe::MGPipeHandle Handle;
            // P10: zero-timeout polls answered "not yet" in a row since the last escalation.
            Uint32 Unanswered = 0;
            // The session the fence was created on. A fence of an ENDED session (a recovered
            // device loss) names nothing on the current server: it is answered as the no-op a
            // lost device gives (signaled), and deleting it puts nothing on the wire.
            Uint64 Epoch = MG_State::CurrentWireEpoch();
            Bool FromEndedSession() const { return Epoch != MG_State::CurrentWireEpoch(); }
        };
        std::mutex g_fenceMutex;
        UnorderedMap<MG_Backend::BackendSyncHandle, UniquePtr<RemoteFenceProxy>> g_fenceProxies;

        RemoteFenceProxy& FenceProxy(MG_Backend::BackendSyncHandle proxy) {
            const auto it = g_fenceProxies.find(proxy);
            if (it == g_fenceProxies.end())
                Wire::WireProtocolFatal("Fence.proxy", "unknown client fence proxy");
            return *it->second;
        }

        MG_Pipe::MGPipeHandle FenceHandle(MG_Backend::BackendSyncHandle proxy) { return FenceProxy(proxy).Handle; }

        // ---- P10 (CONTRACT-P10.md §1): WHAT THE SERVER HAS SAID IS SIGNALED -----------------
        //
        // {slot -> gen} of every fence a kEventFenceSignaled named, or a round trip answered
        // signaled. ITS OWN MUTEX, NOT g_fenceMutex: the report arrives in the event drain, and
        // the drain runs INSIDE the emitters below while they hold g_fenceMutex (EmitAndWait and
        // PollEntry both drain), so taking g_fenceMutex there would deadlock the GL thread on
        // itself. A report for a fence already deleted leaves a {slot, old gen} entry that no live
        // handle matches; the slot's next occupant overwrites it.
        std::mutex g_signaledFenceMutex;
        UnorderedMap<Uint32, Uint32> g_signaledFences;
        std::atomic<Uint64> g_fenceLocalAnswers{0};
        std::atomic<Uint64> g_fenceEscalations{0};
        std::atomic<Uint64> g_fenceRoundTrips{0};
        std::atomic<Uint64> g_fenceServerReports{0};

        Bool FenceKnownSignaled(MG_Pipe::MGPipeHandle handle) {
            const std::lock_guard<std::mutex> lock(g_signaledFenceMutex);
            const auto it = g_signaledFences.find(handle.Slot);
            return it != g_signaledFences.end() && it->second == handle.Gen;
        }

        void RememberFenceSignaled(MG_Pipe::MGPipeHandle handle) {
            const std::lock_guard<std::mutex> lock(g_signaledFenceMutex);
            g_signaledFences[handle.Slot] = handle.Gen;
        }

        void ForgetFence(MG_Pipe::MGPipeHandle handle) {
            const std::lock_guard<std::mutex> lock(g_signaledFenceMutex);
            const auto it = g_signaledFences.find(handle.Slot);
            if (it != g_signaledFences.end() && it->second == handle.Gen) g_signaledFences.erase(it);
        }

        // MOBILEGL_IPC_POLL_ESCALATE=0 is the A/B control: every poll crosses, as before P10. A lost
        // device takes the round-trip arm too, whose DECLINED answer is the no-op a dead fence gets.
        Bool FencePollsAnswerLocally() {
            return MG_Config::Ipc.PollEscalate != 0 && !ClientSession::DeviceLost();
        }

        // THE LOCAL ANSWER, OR "CROSS". A poll is a doorbell point first (PollEntry: flush what is
        // published, drain the reverse channel), so the fence's own create record has reached the
        // server and every report it has posted is in. Known signaled -> answered here. Not known
        // and the call may not wait (a zero timeout, glGetSynciv) -> answered "not yet" here,
        // unless this fence has had POLL_ESCALATE such answers in a row, in which case the poll
        // takes one real round trip: the server's own reports are what make a local "not yet"
        // eventually true, and the escalation is the bound that holds even if one is late. A call
        // that may wait (a non-zero timeout) is never answered "not yet" here - it crosses and
        // waits on the server, as the GL call asks.
        enum class FencePollAnswer { Signaled, NotYet, Cross };
        FencePollAnswer AnswerFencePollLocally(ClientSession& session, RemoteFenceProxy& fence, Bool mayWait) {
            if (!FencePollsAnswerLocally()) return FencePollAnswer::Cross;
            session.PollEntry();
            if (FenceKnownSignaled(fence.Handle)) {
                fence.Unanswered = 0;
                g_fenceLocalAnswers.fetch_add(1, std::memory_order_relaxed);
                return FencePollAnswer::Signaled;
            }
            if (mayWait) return FencePollAnswer::Cross;
            if (++fence.Unanswered < MG_Config::Ipc.PollEscalate) {
                g_fenceLocalAnswers.fetch_add(1, std::memory_order_relaxed);
                return FencePollAnswer::NotYet;
            }
            fence.Unanswered = 0;
            g_fenceEscalations.fetch_add(1, std::memory_order_relaxed);
            return FencePollAnswer::Cross;
        }

        MG_Backend::BackendSyncHandle EmitFenceSync() {
            ClientSession& session = RequireSession("FenceSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            BeforeReadOnlyVerb();
            auto proxy = MakeUnique<RemoteFenceProxy>();
            proxy->Handle = MG_Pipe::MGPipeSlots().Allocate(MG_Pipe::MGPipeKind::Fence);
            const MG_Pipe::MGPHandleOnly desc{proxy->Handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
            session.EmitAndWait(MG_Pipe::MGPWireOp::FenceCreate, &desc, sizeof(desc),
                                nullptr, 0, nullptr, 0, nullptr);
            auto* local = proxy.get();
            g_fenceProxies.emplace(local, std::move(proxy));
            return local;
        }

        Uint32 ReadFenceReply(ClientSession& session, MG_Pipe::MGPWireOp op,
                             const void* payload, Uint64 bytes) {
            Uint32 result = 0;
            Int32 status = Wire::ReplySink::kStatusError;
            Uint64 replyBytes = 0;
            session.EmitAndWait(op, payload, bytes, nullptr, 0, &result, sizeof(result),
                                &status, &replyBytes);
            // A DECLINED SYNC REPLY IS AN ANSWER, NOT A CORRUPTION (P12). ReplySink opens the
            // three-value space with "DECLINED IS A REAL ANSWER, not a failure"
            // (PipeWireCodec.h:457-460) and the escape rows are held to it the other way round -
            // "ERROR IS NOT A DECLINE", an escape may not fold 2 into false
            // (WireTables.cpp:446-450). This site folded 1 AND 2 into one abort.
            //
            // BOTH VALUES OF 1 ARE REACHABLE WITHOUT A BYTE OF CORRUPTION:
            //   - the SERVER declines these two rows by design whenever it cannot answer them -
            //     PostReply(op, seq, ok ? kStatusOk : kStatusDeclined, ok ? &result : nullptr,
            //     ok ? sizeof(result) : 0) at PipeWireCodec.cpp:1820-1830, which is a legitimate
            //     zero-length answer to a kWaitReply row (R-5);
            //   - the CLIENT produces the same status itself once the session stops carrying
            //     records: the device-lost latch (ClientSession.cpp:1837) and a doorbell that died
            //     during teardown (ClientSession.cpp:2025, where the comment says outright that
            //     "the verb did not happen" is reported as DECLINED and not as ERROR).
            //
            // AND THE NO-OP IS THE ONE THIS FRONTEND ALREADY GIVES for a fence it cannot wait on:
            // GL_Sync.cpp:103-106 answers GL_ALREADY_SIGNALED for a sync with no backend handle,
            // PipeApplier.cpp:175-177 answers GL_ALREADY_SIGNALED for a fence with no native
            // object (a fence that provably did no work), and DirectGLES.cpp:16489 does the same
            // for an absent sync. A lost device is that same class of fact.
            //
            // WHAT ABORTING HERE COSTS, MEASURED: after a clean device loss Minecraft 26.2's next
            // glClientWaitSync - the chunk-upload fences - reached this line with DECLINED and died
            // of Fatal{ProtocolCorruption, "Fence.reply"}, rc=134, instead of the no-op the design
            // promises ("GL calls become no-ops", design/08-runtime-and-platform.md:33).
            if (status == Wire::ReplySink::kStatusDeclined) {
                MGLOG_E_ONCE("MG_Remote client: the peer DECLINED a %s reply - the verb did not "
                             "happen (a lost device, or a teardown); answering the no-op the "
                             "frontend uses for a fence it cannot wait on",
                             Wire::WireOpName(op));
                return op == MG_Pipe::MGPWireOp::FenceWait ? static_cast<Uint32>(GL_ALREADY_SIGNALED)
                                                           : 1u;
            }
            // ERROR AND A SHORT OK ARE STILL THE PROTOCOL CORRUPTIONS THEY ALWAYS WERE: a
            // transport fault is not the server saying no, and an OK answer that arrived empty is
            // a reply nobody can read.
            if (status != Wire::ReplySink::kStatusOk || replyBytes != sizeof(result))
                Wire::WireProtocolFatal("Fence.reply", "missing or malformed sync result");
            return result;
        }

        GLenum EmitClientWaitSync(MG_Backend::BackendSyncHandle proxy, GLbitfield flags, GLuint64 timeout) {
            ClientSession& session = RequireSession("ClientWaitSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            RemoteFenceProxy& fence = FenceProxy(proxy);
            if (fence.FromEndedSession()) return GL_ALREADY_SIGNALED;
            switch (AnswerFencePollLocally(session, fence, /*mayWait=*/timeout != 0)) {
            case FencePollAnswer::Signaled: return GL_ALREADY_SIGNALED;
            case FencePollAnswer::NotYet: return GL_TIMEOUT_EXPIRED;
            case FencePollAnswer::Cross: break;
            }
            g_fenceRoundTrips.fetch_add(1, std::memory_order_relaxed);
            const MG_Pipe::MGPFenceWait request{fence.Handle, timeout, flags, 0};
            const Uint32 result = ReadFenceReply(session, MG_Pipe::MGPWireOp::FenceWait, &request, sizeof(request));
            if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) RememberFenceSignaled(fence.Handle);
            return result;
        }

        Bool EmitGetSyncStatus(MG_Backend::BackendSyncHandle proxy) {
            ClientSession& session = RequireSession("GetSyncStatus");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            RemoteFenceProxy& fence = FenceProxy(proxy);
            if (fence.FromEndedSession()) return true;
            switch (AnswerFencePollLocally(session, fence, /*mayWait=*/false)) {
            case FencePollAnswer::Signaled: return true;
            case FencePollAnswer::NotYet: return false;
            case FencePollAnswer::Cross: break;
            }
            g_fenceRoundTrips.fetch_add(1, std::memory_order_relaxed);
            const MG_Pipe::MGPHandleOnly desc{fence.Handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
            const Uint32 result = ReadFenceReply(session, MG_Pipe::MGPWireOp::FenceStatus, &desc, sizeof(desc));
            if (result > 1) Wire::WireProtocolFatal("FenceStatus.reply", "status must be boolean");
            if (result != 0) RememberFenceSignaled(fence.Handle);
            return result != 0;
        }

        void EmitWaitSync(MG_Backend::BackendSyncHandle proxy, GLbitfield flags, GLuint64 timeout) {
            ClientSession& session = RequireSession("WaitSync");
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            if (FenceProxy(proxy).FromEndedSession()) return;
            const MG_Pipe::MGPFenceWait request{FenceHandle(proxy), timeout, flags, 0};
            session.EmitAndWait(MG_Pipe::MGPWireOp::FenceWaitServer, &request, sizeof(request),
                                nullptr, 0, nullptr, 0, nullptr);
        }

        void EmitDeleteSync(MG_Backend::BackendSyncHandle proxy) {
            const std::lock_guard<std::mutex> lock(g_fenceMutex);
            const auto handle = FenceHandle(proxy);
            // MobileGL::Destroy stops the server BEFORE DestroyAllSyncObjects. Detach already
            // released those native objects on the apply thread; only the local proxy remains.
            // A fence of an ended session likewise: its server is gone.
            if (auto* session = ClientSession::Active();
                session != nullptr && session->Started() && !FenceProxy(proxy).FromEndedSession()) {
                const MG_Pipe::MGPHandleOnly desc{handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
                session->EmitAndWait(MG_Pipe::MGPWireOp::FenceDestroy, &desc, sizeof(desc),
                                     nullptr, 0, nullptr, 0, nullptr);
            }
            MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Fence, handle);
            ForgetFence(handle);
            g_fenceProxies.erase(proxy);
        }

#include "QueryEmit.inc"

        // CLASS C - each remaining slot names its first blocker.
        // =============================================================================
        //
        // PARTITIONED BY THE P5b PACKAGE THAT OWNS THE FLIP (MG_Remote/CONTRACT-P5B.md,
        // ~/w7/notes/p5b/BRIEF-P5B.md), so that four packages migrating in parallel edit four
        // DISJOINT lists and four DISJOINT counts rather than one list and one number. To flip a
        // slot a package (1) removes its X row from ITS list, (2) assigns the real emitter in
        // BuildRemoteEmitTable's class-B block, (3) raises ITS kEmittedSlots* by one. The
        // per-package ownership assertions below then still hold, the totals stay arithmetic,
        // and a slot that changes class without changing the arithmetic is a build break. The
        // X-macro shape is kept so the DEFINITION and the ASSIGNMENT cannot drift apart. Three
        // slots carried a body the macro cannot (DispatchCompute, DispatchComputeIndirect,
        // SetSwapInterval) and were written out by hand below; all three are class B now.
        //
        //   d1   indexed / instanced / multi-draw / indirect draws -> draw_vbo (59), its
        //        kDrawIsIndirect tail and its kDrawHasUserIndices span
        //   i1   image bind, compute, barriers, copy-image, storage block -> bind_shader_image
        //        (72), launch_grid (60), memory_barrier (61), resource_copy_region (53),
        //        set_storage_block_binding (75)
        //   t2   the XFB spans and object bind, the patch parameter -> begin/end/pause/resume_
        //        stream_output (62..65), bind_stream_output (74), patch_parameter (73)
        //   f1   the clear family, the framebuffer-sourced copies, mips -> clear (57),
        //        copy_framebuffer_to_texture (76), generate_mipmap (54)
        //   tail the wave-3 remainder nothing measured: queries, syncs, the texture readbacks,
        //        the DSA blit, the swap interval (census-classC.md "static cross")

        // d1 v1 flipped all nineteen (DrawElements, DrawElementsBaseVertex, MultiDrawArrays,
        // MultiDrawElements, MultiDrawElementsBaseVertex, MultiDrawElementsIndirect,
        // MultiDrawArraysIndirect, MultiDrawElementsIndirectCount, MultiDrawArraysIndirectCount,
        // DrawRangeElementsBaseVertex, DrawRangeElements, the four DrawElementsInstanced*,
        // DrawArraysInstancedBaseInstance, DrawArraysInstanced, DrawElementsIndirect,
        // DrawArraysIndirect) to class B; the list is kept, empty, so the ownership assertion
        // below still reads 0 + 19 = 19 and a slot that fell back in would have to be added here.
#define MGR_UNMIGRATED_D1_SLOTS(X)

        // i1 HAS LANDED: the list is EMPTY and all seven slots are class B (the five emitters
        // above plus the two compute ones). It is kept as an empty macro rather than deleted so
        // that MGR_UNMIGRATED_GL_SLOTS' union, kUnmigratedI1's arithmetic and the ownership
        // static_assert below all keep their shape - and so the next package to need a row here
        // (a P5b wave-3 image/compute slot) has the partition to put it in.
#define MGR_UNMIGRATED_I1_SLOTS(X)

        // t2 LANDED (CONTRACT-P5B.md §2 t2): the three measured slots - BeginTransformFeedback
        // (95 lane entries), PatchParameteri (43), BindTransformFeedback (2) - and the three
        // companions that share their rows are class B now and live in the block above.
        // DeleteTransformFeedback is the one that stays: it has NO ROW in P5b, by ruling and
        // not by omission (unmeasured; the driver object leaks on the server until P9's XFB
        // namespace work, and a bind of name 0 is what the backend does on delete of the bound
        // one, DirectGLES.cpp:1422). It therefore still aborts by its own name.
#define MGR_UNMIGRATED_T2_SLOTS(X)

#define MGR_UNMIGRATED_F1_SLOTS(X)

        // The wave-3 tail. Empty since P10 B retired SetSwapInterval (it was not a GL.* slot and
        // was never a row here).
#define MGR_UNMIGRATED_TAIL_SLOTS(X)

        // The non-void ones, kept apart only because the macro body differs: a [[noreturn]]
        // call is a complete body for a void slot and for a value-returning one alike, but a
        // compiler that does not see UnmigratedVerbFatal's attribute through the macro would
        // warn on the second. It does see it; they are split for readability. All ten are the
        // wave-3 tail.
#define MGR_UNMIGRATED_TAIL_VALUE_SLOTS(X)

        // The union, for the places that want every class-C row at once (the definitions and
        // the assignments). A package never edits THIS; it edits its own list above.
#define MGR_UNMIGRATED_GL_SLOTS(X)                                                                 \
    MGR_UNMIGRATED_D1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_I1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_T2_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_F1_SLOTS(X)                                                                     \
    MGR_UNMIGRATED_TAIL_SLOTS(X)
#define MGR_UNMIGRATED_GL_VALUE_SLOTS(X) MGR_UNMIGRATED_TAIL_VALUE_SLOTS(X)

#define MGR_DEFINE_UNMIGRATED(Name, Ret, Sig)                                                      \
    Ret Name##_Unmigrated Sig { UnmigratedVerbFatal(#Name); }

        MGR_UNMIGRATED_GL_SLOTS(MGR_DEFINE_UNMIGRATED)
        MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_DEFINE_UNMIGRATED)
#undef MGR_DEFINE_UNMIGRATED

        // THE TWO COMPUTE SLOTS' class-C stubs are GONE (P5b i1): they carried b1's dispatch
        // hook before their Fatal so that the package flipping them would inherit a call site
        // that was already correct, and EmitDispatchCompute / EmitDispatchComputeIndirect above
        // are that inheritance - same two calls, same order, the record where the Fatal was.
        //
        // THE LAST HAND-WRITTEN STUB, SetSwapInterval_Unmigrated, IS GONE TOO (P10 B), and class C
        // is empty. It was never reachable: eglSwapInterval reaches the table only through
        // BackendObject::SetEGLSwapInterval, and BackendObject_Remote overrode that to call
        // Server::ServerSetEGLSwapInterval - a SurfaceControlFrame on the control channel, which
        // crosses processes. The slot now IS that forwarder (assigned in the class-B block), the
        // override is deleted, and there is one route from eglSwapInterval to the server.

        // The counts, as arithmetic. MGR_COUNT_ONE expands to `+ 1` per row.
#define MGR_COUNT_ONE(Name, Ret, Sig) +1
        constexpr Uint32 kUnmigratedD1 = 0 MGR_UNMIGRATED_D1_SLOTS(MGR_COUNT_ONE);
        // i1 landed: the list is empty and the two hand-written compute stubs are gone with it.
        constexpr Uint32 kUnmigratedI1 = 0 MGR_UNMIGRATED_I1_SLOTS(MGR_COUNT_ONE);
        constexpr Uint32 kUnmigratedT2 = 0 MGR_UNMIGRATED_T2_SLOTS(MGR_COUNT_ONE);
        constexpr Uint32 kUnmigratedF1 = 0 MGR_UNMIGRATED_F1_SLOTS(MGR_COUNT_ONE);
        constexpr Uint32 kUnmigratedTail =
            0 MGR_UNMIGRATED_TAIL_SLOTS(MGR_COUNT_ONE) MGR_UNMIGRATED_TAIL_VALUE_SLOTS(MGR_COUNT_ONE);
#undef MGR_COUNT_ONE
        constexpr Uint32 kUnmigratedSlots =
            kUnmigratedD1 + kUnmigratedI1 + kUnmigratedT2 + kUnmigratedF1 + kUnmigratedTail;

        // The emitted counts, PER OWNER. P5's five are c1's; each P5b package raises its own.
        constexpr Uint32 kEmittedSlotsP5 = 5; // Clear, DrawArrays, ReadPixels, Blit, Present
        constexpr Uint32 kEmittedSlotsD1 = 19;
        constexpr Uint32 kEmittedSlotsI1 = 7;
        constexpr Uint32 kEmittedSlotsT2 = 7;
        constexpr Uint32 kEmittedSlotsF1 = 11;
        // BlitNamedFramebuffer, both texture readbacks, and SetSwapInterval (P10 B) - the one
        // class-B slot that crosses as a surface control frame rather than a SEG_CMD record.
        constexpr Uint32 kEmittedSlotsTail = 4;
        constexpr Uint32 kEmittedSlotsSync = 5;
        constexpr Uint32 kEmittedSlotsQueries = 11;
        constexpr Uint32 kEmittedSlots =
            kEmittedSlotsP5 + kEmittedSlotsD1 + kEmittedSlotsI1 + kEmittedSlotsT2 + kEmittedSlotsF1 +
            kEmittedSlotsTail + kEmittedSlotsSync + kEmittedSlotsQueries;
        constexpr Uint32 kLocallyAnsweredSlots = 2; // GetIntegeri_v, IsTimerQuerySupported

        // EACH PACKAGE'S OWNERSHIP, PINNED. A package that flips a slot removes one row and
        // adds one to its emitted count; a package that touches another's list breaks the
        // other's line, not its own. The four numbers are the census's package tables plus the
        // unmeasured companions that share a wire row (BRIEF-P5B.md file-ownership table).
        static_assert(kUnmigratedD1 + kEmittedSlotsD1 == 19, "d1 owns the 19 draw slots");
        static_assert(kUnmigratedI1 + kEmittedSlotsI1 == 7, "i1 owns the 7 image/compute/barrier/copy/SSBO slots");
        static_assert(kUnmigratedT2 + kEmittedSlotsT2 == 7, "t2 owns the 7 XFB/tessellation slots");
        static_assert(kUnmigratedF1 + kEmittedSlotsF1 == 11, "f1 owns the 11 clear/copy/mip slots");
        static_assert(kUnmigratedTail + kEmittedSlotsTail + kEmittedSlotsSync + kEmittedSlotsQueries == 20,
                      "the original wave-3 tail owns 20 slots");
        static_assert(kUnmigratedSlots + kEmittedSlots == 69, "class B and C own 69 slots");
        static_assert(kLocallyAnsweredSlots + kEmittedSlots + kUnmigratedSlots == kRemoteEmitSlotCount,
                      "the three classes no longer partition the 71 slots");

        MG_Backend::GlobalBackendFunctionsTable BuildRemoteEmitTable() {
            MG_Record::ArmVerbControlKnobs();
            MG_Backend::GlobalBackendFunctionsTable table{};

            // ---- class C first, so that a slot forgotten below stays Fatal rather than null.
            // Order matters for exactly this reason: if class B's assignment were first, a
            // typo in class C would leave a NULL slot, and a null slot is 91 potential null
            // calls with no diagnostic. This way the worst a mistake can do is name a verb
            // that was supposed to be emitted, loudly.
#define MGR_ASSIGN_UNMIGRATED(Name, Ret, Sig) table.GL.Name = &Name##_Unmigrated;
            MGR_UNMIGRATED_GL_SLOTS(MGR_ASSIGN_UNMIGRATED)
            MGR_UNMIGRATED_GL_VALUE_SLOTS(MGR_ASSIGN_UNMIGRATED)
#undef MGR_ASSIGN_UNMIGRATED

            table.GL.FenceSync = &EmitFenceSync;
            table.GL.ClientWaitSync = &EmitClientWaitSync;
            table.GL.GetSyncStatus = &EmitGetSyncStatus;
            table.GL.WaitSync = &EmitWaitSync;
            table.GL.DeleteSync = &EmitDeleteSync;
            table.GL.BeginTimeElapsedQuery = &EmitBeginTimeElapsedQuery;
            table.GL.EndTimeElapsedQuery = &EmitEndQuery;
            table.GL.QueryCounterTimestamp = &EmitQueryCounterTimestamp;
            table.GL.BeginOcclusionQuery = &EmitBeginOcclusionQuery;
            table.GL.EndOcclusionQuery = &EmitEndQuery;
            table.GL.BeginXfbPrimitivesQuery = &EmitBeginXfbPrimitivesQuery;
            table.GL.EndXfbPrimitivesQuery = &EmitEndQuery;
            table.GL.IsQueryResultAvailable = &EmitIsQueryResultAvailable;
            table.GL.GetQueryResult64 = &EmitGetQueryResult64;
            table.GL.DeleteBackendQuery = &EmitDeleteBackendQuery;
            table.GL.GetGpuTimestampNs = &EmitGetGpuTimestampNs;

            // ---- class A
            table.GL.GetIntegeri_v = &AnswerGetIntegeri_v;
            table.GL.IsTimerQuerySupported = &AnswerIsTimerQuerySupported;
            // NOT A SLOT and not a verb: a Bool member of the table, whose one non-test client
            // reader is GL_Query.cpp:221. It does NOT ride inside MGPCaps::Dynamic - it is a
            // member of GLFunctionsTable, which is exactly the thing a split client never
            // receives - so it is answered from kCapCpuXfbPrimitiveAccounting.
            table.GL.PrefersCpuXfbPrimitiveAccounting =
                CapsMirrorInstance().PrefersCpuXfbPrimitiveAccounting();

            // ---- class B. P13 W5: every slot a VerbChannel emitter serves (draws, clears,
            // blits, readbacks, compute, copies, image binds, XFB, mipmaps) is MG_Record's
            // (MG_Impl/Pipe/Verb/VerbPort.cpp); the session's own slots stay below.
            MG_Record::AssignVerbEmitters(table);
            // ---- P5b d1: the nineteen draw slots, all on draw_vbo (CONTRACT-P5B.md §2 d1)
            table.Present = &EmitPresent;
            // P10 B: the swap interval is the server's presentation-path question, so the slot
            // forwards it there as a SurfaceControlFrame (ServerLoop.cpp's SetSwapInterval arm),
            // across processes on spawn and tcp - the forwarder BackendObject_Remote's deleted
            // SetEGLSwapInterval override used to call.
            table.SetSwapInterval = &Server::ServerSetEGLSwapInterval;
            // ---- class B, P5b t2. Assigned AFTER the class-C block above, which is what makes
            // the flip a single-line change per slot: the Fatal thunk is overwritten, and a slot
            // whose row is removed from MGR_UNMIGRATED_T2_SLOTS but not assigned here would be
            // NULL and caught by RemoteEmitTable.NoSlotIsNull rather than silently skipped.

            // ---- f1 ----

            // ---- class B, P5b package i1 (kEmittedSlotsI1 = 7)

            return table;
        }

    } // namespace

    // glFinish's GPU half: a server fence created after everything the client issued, waited
    // with GL_SYNC_FLUSH_COMMANDS_BIT, then destroyed. Waiting for the apply thread alone left
    // the GPU queue unbounded behind a "finished" glFinish.
    void EmitFinishWait() {
        ClientSession* session = ClientSession::Active();
        if (session == nullptr || !session->Started() || session->DeviceLost()) return;
        const std::lock_guard<std::mutex> lock(g_fenceMutex);
        const MG_Pipe::MGPipeHandle handle = MG_Pipe::MGPipeSlots().Allocate(MG_Pipe::MGPipeKind::Fence);
        const MG_Pipe::MGPHandleOnly desc{handle, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
        session->EmitAndWait(MG_Pipe::MGPWireOp::FenceCreate, &desc, sizeof(desc), nullptr, 0, nullptr, 0, nullptr);
        constexpr GLuint64 kFinishTimeoutNs = 10ull * 1000 * 1000 * 1000;
        const MG_Pipe::MGPFenceWait request{handle, kFinishTimeoutNs, GL_SYNC_FLUSH_COMMANDS_BIT, 0};
        (void)ReadFenceReply(*session, MG_Pipe::MGPWireOp::FenceWait, &request, sizeof(request));
        session->EmitAndWait(MG_Pipe::MGPWireOp::FenceDestroy, &desc, sizeof(desc), nullptr, 0, nullptr, 0, nullptr);
        MG_Pipe::MGPipeSlots().Free(MG_Pipe::MGPipeKind::Fence, handle);
    }

    void NoteFenceSignaledByServer(MG_Pipe::MGPipeHandle fence) {
        RememberFenceSignaled(fence);
        g_fenceServerReports.fetch_add(1, std::memory_order_relaxed);
    }

    FencePollCounters ReadFencePollCounters() {
        FencePollCounters counters;
        counters.LocalAnswers = g_fenceLocalAnswers.load(std::memory_order_relaxed);
        counters.Escalations = g_fenceEscalations.load(std::memory_order_relaxed);
        counters.RoundTrips = g_fenceRoundTrips.load(std::memory_order_relaxed);
        counters.ServerReports = g_fenceServerReports.load(std::memory_order_relaxed);
        return counters;
    }

    const MG_Backend::GlobalBackendFunctionsTable& RemoteEmitTable() {
        // Leaked at exit like every other MG_Remote singleton (ID-8): MG_Backend::Init()
        // copies it into gBackendFunctionsTable and MobileGL::Destroy() clears that copy from
        // an exit handler, by which point a static destructor here would already have run.
        static const MG_Backend::GlobalBackendFunctionsTable& table =
            *new MG_Backend::GlobalBackendFunctionsTable{BuildRemoteEmitTable()};
        return table;
    }

    Uint32 ImplementedVerbCount() { return kEmittedSlots; }
    Uint32 LocallyAnsweredSlotCount() { return kLocallyAnsweredSlots; }
    Uint32 UnmigratedSlotCount() { return kUnmigratedSlots; }

} // namespace MobileGL::MG_Remote::Client
