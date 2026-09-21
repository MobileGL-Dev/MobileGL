// MobileGL - MobileGL/MG_Remote/Client/ClientSession.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client half of a session: the rings, the handshake, the verb barrier. Owner: package s1
// (construction and handshake) with c1 (the barrier and the reply read). Signatures by c0.
//
// INPROC USES ShmSegment AND THE RING, NOT new[] AND NOT InProcessTransport's deques. That is
// half of what "inproc runs the same G3 codec as spawn" means: InProcessTransport
// (InProcessTransport.cpp:38-97) is two deque<vector<uint8_t>> plus two condvar doorbells, it
// touches neither a ring nor a codec, and building the session on top of it instead of on top
// of the ring would make the whole phase unfalsifiable. The transport supplies the two
// DOORBELLS and the control plane; the records go through SEG_CMD.
//
// THE VERB BARRIER (R-1). After emitting a verb the client blocks until
// RingControl::appliedSeq >= the seq it just got back from the encoder. It is not caution: 31
// of the 63 PipeInputs fields are still filled by the client's residual pass out of a live
// GLContext, so an unbarriered queue lets the server read a FUTURE value of them. Two
// consequences that must be stated because both are load-bearing:
//   - while the barrier holds, at most one of {GL thread, apply thread} is runnable, which is
//     what makes a single process-wide gPipeInputs legal (table 3);
//   - the barrier is a RETIRING object, not a design. It opens family by family as table 2's
//     BARRIER-PULLED column empties, and each later package reports how many rows it left.
//
// THE BARRIER'S WAIT IS ALSO THE REPLY'S WAIT (R-3/R-5). The reply slot id IS the record seq,
// so "wait for appliedSeq >= mySeq" and "wait for my answer" are one wait and the four Bool
// acceptance returns, ReadPixels' pixels and MapPersistent's decline cost ZERO extra round
// trips. The client MUST NOT re-derive any of those four answers locally - that is the c0f/c0g
// defect P4a paid two contract corrections for, and "always accept" is ID-39's 66 lost uploads.

#pragma once
#include <Includes.h>

#include <Config.h>
#include <MG_Pipe/MGPipe.h>

#include "../Transport/Doorbell.h"
#include "../Transport/EventRing.h"
#include "../Transport/ITransport.h"
#include "../Transport/ReplySlot.h"
#include "../Transport/Ring.h"
#include "../Transport/RoleMemory.h"
#include "../Transport/SessionRings.h"
#include "../Transport/SocketTransport.h"
#include "../Wire/PipeWireCodec.h"
#include "CapsMirror.h"

#include <memory>

namespace MobileGL::MG_Remote::Transport {
    class InProcessTransport;
}

namespace MobileGL::MG_Remote::Client {

    class ClientSession {
    public:
        // Null until Start() succeeds; MG_Backend::Init() is the only caller of Start().
        static ClientSession* Active();

        ~ClientSession();

        // Builds the four segments, performs Hello/Welcome, takes the first CapsSnapshot, and
        // - for TransportMode::InProcess - starts the server role's apply thread. Returns a
        // named error rather than falling back to monolith: a fallback here is the "split lane
        // ran monolith and went green" failure, and it must be loud.
        MobileGLResult Start(MG_Config::TransportMode mode, const String& endpoint);

        // Start()'s second half: the handshake and everything after it, over a transport pair
        // the CALLER made with InProcessTransport::CreatePair. Start() refuses every mode but
        // `inproc`, makes the pair, and calls this; it is public for exactly one reason. The
        // Welcome guard below (envelope->msg_as_Welcome() == nullptr, ID-46 finding 7) can only
        // be reached by a frame that arrives on the server->client direction BEFORE the
        // server's own Welcome, and Start() builds that pair itself, so no control could put
        // one there. SessionHandshakeTest does it through here, and the null-union Welcome must
        // come back as MOBILEGL_ERR_PROTOCOL_MISMATCH with the guard's own line. Not a second
        // way to start a session: MG_Backend::Init() calls Start(), and nothing else may call
        // this with a pair it did not just create.
        // P6 `sm`: the spawn client's half. `transport` is this process's end of
        // the control socket to an already-running server process; the two bell
        // fds are this side's ends of the two bell pairs (ServerSpawn.h).
        //
        // It is the SAME handshake StartOverTransportPair runs, with three
        // differences and no others: the server half happens in another process
        // so there is no local Accept to call, the four segments arrive as
        // descriptors over SCM_RIGHTS instead of being dup'ed from a local
        // owner, and the bells are sockets rather than condvars. Everything
        // below the attach - producer, reply pool, event ring, segment table,
        // encoder - is byte for byte the inproc path, which is the property
        // that makes "P6 is a transport swap" true of THIS function even though
        // it is not true of the phase.
        MobileGLResult FinishStartup(Transport::Doorbell* peerBell,
                                     Transport::Doorbell* selfBell,
                                     bool startApplyThreadHere);

        // `transport` is this process's end of a connection to an ALREADY
        // RUNNING server process. The two bells arrive over it, with the four
        // segments, as SCM_RIGHTS offers - nothing here is inherited, because
        // the two processes were started independently.
        MobileGLResult StartOverSocket(std::unique_ptr<Transport::SocketTransport> transport);

        MobileGLResult StartOverTransportPair(std::unique_ptr<Transport::InProcessTransport> clientEnd,
                                              std::unique_ptr<Transport::InProcessTransport> serverEnd);

        // Teardown order matters and is table 3's fourth column: publish and let the server
        // drain, Doorbell::Kill() (the ONLY thing that wakes an apply thread parked on
        // kWaitForever, CondVarDoorbell::Kill), then join, and only then release anything an
        // emitter owns - a tail still referenced by an unapplied record is a use-after-free
        // the join is what prevents.
        void Stop();

        Wire::PipeWireEncoder& Encoder();
        CapsMirror& Caps();

        // Emit one record and, if the barrier is armed, wait for it. `replyOut`/`replyBytes`
        // name where a kReplySlot answer lands; pass {nullptr, 0} for a call that has none.
        // Returns the record's seq, which is also its reply-slot id.
        //
        // `replySizeOut` (optional) receives the answer's OWN byte count as the server stamped
        // it - which is not always `replyBytes`: a short OK reply stamps fewer, and a DECLINE or
        // ERROR stamps 0. ReadPixels is the one caller that must know, because scattering a
        // reply that arrived short would spray stale bytes as pixels (M2 / codex 11); it reads
        // this and refuses `replySize != DstSize` by name rather than trust the copy.
        //
        // Waiting is spin(MOBILEGL_IPC_SPIN_US) then park, through Doorbell::Wait, with
        // producerParked set before blocking - the shape Doorbell::Wait already implements.
        Uint64 EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                           const void* varTail, Uint64 varTailBytes, void* replyOut,
                           Uint64 replyBytes, Int32* statusOut, Uint64* replySizeOut = nullptr);

        // The same call for a record that carries TWO tails (P5b d1: draw_vbo's user-index span
        // or its MGPDrawIndirect block behind the range array). The one-tail form above is this
        // one with a single WireTail; the barrier policy lives in exactly one body. A distinct
        // name rather than an overload, because `nullptr, 0` would match both.
        // P5e (ra, CONTRACT-P5E §2.5): `wantReply` is the ONE caller-side relaxation of the
        // catalogue's reply rule, and it exists for exactly one row - resource_subdata's
        // BUFFER half, whose Bool the only caller discards (PipeFill.cpp's
        // MGPipeEmitResourceSubData). The row keeps kReplySlot on the wire (the server still
        // posts; an unread slot is harmless, ReplySlot.h:16, 105), so nothing about the
        // protocol moves - what moves is that under run-ahead this client neither waits for
        // the answer nor reads it, which is what makes the 64 KB persistent-map push
        // fire-and-forget instead of a hidden round trip per block. It is NOT a licence to
        // skip an answer a caller uses: R-5 still forbids re-deriving one locally, and every
        // other kReplySlot row passes true (the default) and waits.
        Uint64 EmitAndWaitTails(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                                const Wire::WireTail* tails, Uint32 tailCount, void* replyOut,
                                Uint64 replyBytes, Int32* statusOut,
                                Uint64* replySizeOut = nullptr, Bool wantReply = true);

        // MOBILEGL_IPC_VERB_BARRIER. False is the R-1 negative control and is EXPECTED to be
        // red; it must be run once and the way it goes red recorded.
        Bool BarrierArmed() const;

        // ---- P5e (ra): the wait rule (CONTRACT-P5E §1, §2.3, §2.4, §2.5) -------------------

        // A CONJUNCTION OF THREE, LATCHED ONCE: `m_barrierArmed && Ipc.RunAhead &&
        // Caps().HasCap(kCapRunAheadApply)`. The third conjunct is the server's own statement
        // that it applies an unbarriered record without reading client memory, so run-ahead is
        // never something the client can turn on by itself - on Magma, and on Espryt until the
        // integration commit publishes bit 10, this answers false and EmitAndWaitTails runs
        // today's lockstep path byte for byte.
        //
        // LATCHED AT THE FIRST CAPS ADOPTION AFTER Start, AND A LATER SNAPSHOT MAY ONLY TURN IT
        // OFF. A caps re-run cannot promote a lockstep server to a run-ahead one mid-frame:
        // records already published without a wait would have been published against the wrong
        // rule, and there is no way to un-publish them. Demotion is safe in the other
        // direction - it only adds waits - so it is allowed.
        Bool RunAheadArmed() const;

        // §2.3: the four O-class SharedPtr rows of gPipeInputs (PipeInputs.h:801-809 - the
        // bound VAO and the three programs), dropped on the GL thread once a barriered apply
        // has returned. Under lockstep the next fill overwrites them within a verb; under
        // run-ahead the next fill may be a frame away, and a pinned VAO whose last owner ends
        // up being the apply thread is exactly the deferred-destroy hazard §2.7 keeps a queue
        // for. Releasing them at the one instant the applier is provably idle costs four
        // refcount drops per barriered record and removes the pin entirely.
        void ReleaseFillPins();

        // §2.5's forced wait, as one named entry point: publish nothing, wait for the apply
        // thread to reach LastPublishedSeq, drain the reverse channel. `why` names the caller
        // in the timeout Fatal, because "which forced wait wedged" is the whole diagnostic.
        // A no-op when run-ahead is not armed (the client is already in step by construction).
        void WaitForApplyToCatchUp(const char* why);

        // glFinish. Flush and Finish stay no-ops ON THE WIRE (ARCHITECTURE §11, :415) - there
        // is no record to emit - but "the GL commands issued so far have completed" cannot be
        // true while a run-ahead queue still holds them, so under run-ahead glFinish becomes
        // exactly WaitForApplyToCatchUp + the drain that comes with it.
        void Finish();

        // The last seq this session published, whether or not it waited for it. The forced
        // waits above are defined against it.
        Uint64 LastPublishedSeq() const;

        // §2.4's counter, published on the stats line as `credit-waits`: how often the client
        // actually blocked for a swap to come back. Zero with a credit of 1 means the server
        // is never the frame's critical path; a number close to the frame count means the
        // credit is what paces this workload, which is what the device exit measures.
        Uint64 PresentCreditWaits() const;

        // §2.4, called by EmitPresent immediately before it encodes: pay the credit (parking
        // if this client already has `MOBILEGL_IPC_PRESENT_CREDIT` presents in flight), then
        // mint and return this frame's 1-based MGPPresent::FrameSerial. The credit wait is
        // BEFORE the encode on purpose: a client that encoded first would hold a SEG_CMD
        // reservation across the park.
        Uint64 AcquirePresentCredit();

        // R-1's invariant made checkable rather than only written down: true while this
        // thread is inside a barrier wait. The apply thread sets its own flag on entry to the
        // applier; a debug/verify build asserts the two are never both true, and that the
        // client never touches gPipeInputs while the server is inside the applier.
        static Bool InBarrierWait();
        static Bool ApplyThreadIsInsideApplier();

        // P5c (gt, CONTRACT-P5C §6 layer 2 / audit A1): the sentence above's second half,
        // wired. A GL-thread touch of gPipeInputs while the apply thread is inside the
        // applier and THIS thread is not in a barrier wait is Fatal{RoleViolation,
        // "gPipeInputs"} - the barrier is the only thing that makes one process-wide
        // gPipeInputs legal (CONTRACT-P5 table 3), and a touch in that window races the
        // applier's own reads of it. The residual fill and the verify harness are legal by
        // timing, not by exemption: they run before the record is published, which under an
        // armed barrier is a moment the flag is provably down (the applier drops it before
        // appliedSeq advances past the record the client last waited on). No-op when no armed
        // split session is live, when the caller IS the apply thread, and when the barrier is
        // disarmed - MOBILEGL_IPC_VERB_BARRIER=0 is R-1's negative control and its red belongs
        // to Fatal{BarrierViolation}, not to this check.
        //
        // P5e (ra, CONTRACT-P5E §3.5) RE-DERIVES IT UNDER RUN-AHEAD, and `isBarrieredFill` is
        // what the caller says about ITSELF: "this touch is the residual fill of a record this
        // thread is about to park behind". With run-ahead armed the block is server-role
        // memory for every other touch. The MOBILEGL_IPC_BATCH_WAITS early return goes with
        // it: batching is about how many round trips a lockstep client pays, and it says
        // nothing about a client that pays none.
        //
        // P5e (ra2) KEEPS THE ARGUMENT AND STOPS TAKING IT ON TRUST. "About to park behind it"
        // is a claim about the future; the write is now. Between the fill and the park the
        // apply thread is still draining the unbarriered records the client ran ahead of, so an
        // unconditional exemption made this guard unfireable on exactly the class it was
        // written for - measured as the apply thread aborting on a field whose stamp the GL
        // thread had just withdrawn. So the rule is "not while the applier is inside" again,
        // for barriered fills too; what changed is that the fill sites MAKE it true first, with
        // §2.5's forced wait (PipeFill.cpp's QuiesceApplierBeforeFill), instead of asserting it.
        static void RefusePipeInputsTouchWhileApplierOwnsIt(const char* surface,
                                                            Bool isBarrieredFill = false);

        // ---- c1's additions ---------------------------------------------------------------

        // The apply thread's half of R-1's invariant. v1's apply loop brackets its
        // DecodeAndApply with these; the client checks the flag before it publishes, so
        // "at most one of {GL thread, apply thread} is runnable" is a runtime assertion rather
        // than a sentence in a brief. A raw pair rather than an RAII type in this header
        // because the server side owns its own scoping and must not have to include a client
        // header to get it - ScopedApplierEntry below is the convenience, not the contract.
        static void NoteApplyThreadEnteredApplier();
        static void NoteApplyThreadLeftApplier();
        struct ScopedApplierEntry {
            ScopedApplierEntry() { NoteApplyThreadEnteredApplier(); }
            ~ScopedApplierEntry() { NoteApplyThreadLeftApplier(); }
            ScopedApplierEntry(const ScopedApplierEntry&) = delete;
            ScopedApplierEntry& operator=(const ScopedApplierEntry&) = delete;
        };

        // R-12's INVALIDATION EDGE. Drains whatever the server has queued on the control plane
        // and adopts every CapsSnapshot in it - and a SECOND snapshot IS the invalidation,
        // which is why there is no Invalidate(). Non-blocking: it peeks and returns.
        //
        // Called at the handshake, from BackendObject_Remote's Initialize/InitCapabilities, and
        // once per Present. Present is the boundary every one of P5's three targets crosses,
        // and a caps re-run can only follow a surface event, so once a frame is both sufficient
        // and the cheapest place that is.
        //
        // Returns how many snapshots it adopted, so a case can assert the edge fired rather
        // than assert that a number downstream of it happened to change.
        Uint32 PumpControlPlane();

        // ---- s1's additions: the four primitives c1's EmitAndWait composes ---------------
        //
        // s1 owns construction and lifetime; c1 owns the barrier POLICY. So the plumbing is
        // here and the composition is c1's: encode (w1) -> PublishAndNotify -> WaitForApplied
        // -> ReadReply. Splitting it the other way round is how a package ends up
        // re-deriving an acceptance answer locally, which is the c0f/c0g defect P4a paid two
        // contract corrections for.

        Bool Started() const;

        // Publish the ring head, record submittedSeq, then ring the server IF IT IS PARKED -
        // in that order. RingTest.cpp:446 pins the order; SessionProducer is where it lives.
        Transport::SessionProducer& Producer();

        // Wait for RingControl::appliedSeq >= seq. Returns ShutDown when the doorbell died,
        // which is the only thing that returns from a kWaitForever park and therefore the
        // only way a client blocked in the barrier survives a server that went away.
        Transport::SessionWait WaitForApplied(Uint64 seq, Uint32 timeoutMs);

        // The reply slot for `seq`, addressed seq % slots with the seq stamped back into the
        // header for self-check (R-3). `outStatus` is 0 OK / 1 DECLINED / 2 ERROR, and
        // DECLINED IS A REAL ANSWER - MapPersistent's nullptr and the four Bool acceptances.
        Bool ReadReply(Uint64 seq, void* outBytes, Uint64 outCapacity, Int32* outStatus,
                       Uint64* outSize);
        // What one answer may carry. A ReadPixels bigger than this is Fatal rather than
        // chunked, so the client checks BEFORE it emits.
        Uint32 MaxReplyBytes() const;
        // ID-47, the fifth primitive: true exactly when an answer of `bytes` can be posted.
        Bool ReplyCanHold(Uint64 bytes) const;
        // ID-47's named refusal, forwarded verbatim to ReplySlotPool::RequireReadPixelsFits.
        // Returns when the answer fits; otherwise
        //     Fatal{ReplyTooLarge, "ReadPixels <w>x<h> <format> <bytes> > <cap>"}
        // and abort - AT THE CLIENT, BEFORE EMISSION. Package c1's OnReadPixels emitter calls
        // this once, immediately before EmitAndWait(MGPWireOp::ReadPixels, ...), with the
        // record's box, its Format/Type enums and the DstSize it computed; the server's Post
        // keeps its own refusal as the last line of defence, but that one fires on the apply
        // thread with the record already on the wire, where all the client sees is a hang.
        void RequireReadPixelsReplyFits(Uint32 width, Uint32 height, Uint32 format, Uint32 type,
                                        Uint64 bytes) const;

        // The reverse channel's reading end: OnBufferWriteback / OnGpuWritten /
        // OnSurfaceChanged. Drained by the GL thread between verbs.
        Transport::EventRingConsumer& Events();

        // The event ring's SECOND drain point, for the blocking EGL lifecycle RPCs
        // (BackendObject_Remote's surface creation and make-current). Their return is the
        // same kind of instant as EmitAndWait's post-barrier one - the apply thread is
        // known idle because the RPC it was serving has completed and this thread has
        // published nothing since - so the consumers' frontend writes are as legal here as
        // there. A surface-changed event posted during bring-up must be applied BEFORE the
        // first frontend query of the default framebuffer's depth/stencil format: waiting
        // for the first verb's drain answered GL_DEPTH32F_STENCIL8 (the placeholder) for a
        // depth24+stencil8 surface, and every buffer allocated from that answer was
        // blit-incompatible with the real thing.
        Uint32 DrainPublishedEvents();

        // SEG_EVENT's ring capacity, exposed so the readback path can slice a writeback
        // request into records that always fit (RingProducer::MaxRecordBytes ==
        // capacity/2, Ring.h:288).
        Uint64 EventRingCapacityBytes() const;

        // SEG_STAGE's capacity, exposed so a content emitter can cut a range into records that
        // always stage whole: the staging area is a linear arena of exactly these bytes (w1),
        // and one blob larger than it is Fatal{RingOverrun, "SEG_STAGE"} at the encoder rather
        // than a split (PipeWireCodec.cpp:856-864).
        Uint64 StageCapacityBytes() const;

        // The CLIENT's own segment table (P5c ev, CONTRACT-P5C §4.3): the writeback
        // consumer resolves a SEG_EVENT blobref through it. Never the process resolver -
        // table 3 installs that one on the server role only.
        Wire::SegmentTable& Segments();

        Transport::RingControl* Control();
        Transport::SessionSegments& Shm();
        Transport::ITransport* Control_Plane();

        // Peak-RSS accounting for t1 (RoleMemory.h).
        Transport::RoleMemorySample SampleMemory() const;
        void LogMemory(const char* phase) const;
        // R-10's maximum record bytes and R-9's wrap/wait counts, at teardown, in whatever log
        // this process writes. See the definition for why it is not only on the stats line.
        void LogWireLedger() const;

    private:
        // P5e (ra): the latch, taken from PumpControlPlane after every adoption. Private
        // because "when may run-ahead start" is this class's own rule and not a caller's.
        void LatchRunAheadFromCaps();

        Wire::PipeWireEncoder m_encoder;
        Wire::SegmentTable m_segments;
        Bool m_barrierArmed = true;
        // P5e (ra): the latch of RunAheadArmed() and the fact that it has been taken once.
        Bool m_runAheadArmed = false;
        Bool m_runAheadLatched = false;
        // §2.4: presents published (the 1-based FrameSerial space) and how often the credit
        // wait really blocked.
        Uint64 m_presentsSent = 0;
        Uint64 m_presentCreditWaits = 0;

        // sm: the spawn client owns ONE end and two bells. Held here rather
        // than in the transport because which bell is "mine" is the session's
        // knowledge, not the transport's - the same ruling as inproc's
        // PeerDoorbell/SelfDoorbell split.
        std::unique_ptr<Transport::SocketTransport> m_socketTransport;
        std::unique_ptr<Transport::Doorbell> m_socketSelfBell;
        std::unique_ptr<Transport::Doorbell> m_socketPeerBell;

        std::unique_ptr<Transport::InProcessTransport> m_clientTransport;
        std::unique_ptr<Transport::InProcessTransport> m_serverTransport;
        Transport::SessionSegments m_shm;
        Transport::RingProducer m_cmd;
        Transport::SessionProducer m_producer;
        Transport::EventRingConsumer m_events;
        Transport::ReplySlotPool m_replies;
        Transport::ITransport* m_transport = nullptr;
        Bool m_started = false;
    };

    // One per process in P5, because P5 serves one context, and LEAKED AT EXIT like every other
    // MG_Remote singleton (ID-8): no frontend destructor may reach pipe or backend state from an
    // exit handler, and a session destroyed before them would be a use-after-free rather than a
    // tidy teardown. MG_Backend::Init() calls Start() on this one.
    ClientSession& ClientSessionInstance();

} // namespace MobileGL::MG_Remote::Client
