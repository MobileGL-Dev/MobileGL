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
#include "../Transport/ILink.h"
#include "../Transport/ControlInbox.h"
#include <Includes.h>

#include <atomic>  // the device-lost latch is read off the GL thread

#include <Config.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Util/Damage/Damage.h>

#include "../Server/ServerSpawn.h"
#include "../Server/SurfaceControlFrame.h"
#include "../Transport/Doorbell.h"
#include "../Transport/EventRing.h"
#include "../Transport/ITransport.h"
#include "../Transport/ReplySlot.h"
#include "../Transport/Ring.h"
#include "../Transport/RoleMemory.h"
#include "../Transport/SessionRings.h"
#include "../Transport/SocketTransport.h"
#include "../Transport/AdoptT0.h"
#include "../Wire/PipeWireCodec.h"
#include "CapsMirror.h"

#include <functional>
#include <unordered_map>

#include <memory>

namespace MobileGL::MG_Remote::Transport {
    class InProcessTransport;
}

namespace MobileGL::MG_Remote::Client {

    class ClientSession {
    public:
        // Null until Start() succeeds; MG_Backend::Init() is the only caller of Start().
        static ClientSession* Active();

        // P6 `dl` (CONTRACT-P6 5.3). THE DEVICE-LOST LATCH, and the two words that matter are
        // SESSION-SCOPED and LATCH: it is set at most once per session and never cleared while
        // that session lives. What recovers is the PROCESS, not the session: the application's
        // next eglCreateContext / eglCreate*Surface / eglInitialize ends the lost session and
        // dials a fresh one (RecoverAfterDeviceLoss), and every context built before stays lost.
        //
        // SET FROM Doorbell::PeerHungUp() OR THE SERVER'S OWN SessionFault, NEVER FROM A
        // TIMEOUT. 5.4's whole point is that slow and dead are different states and must be told
        // apart without a threshold: under P5e run-ahead a server one frame behind is the
        // INTENDED steady state, so a latch armed by a deadline would fire on a healthy session
        // under load. The descriptor (or the server, naming its fault) answers instead.
        //
        // WHAT THE CALLING THREAD SEES, which is more than the latch: the thread's GL state may
        // belong to a session that has already been replaced (a context from before a recovered
        // loss, still current on some thread), and for that thread the device is lost for good -
        // nothing it does may reach the new session. ScopedWireMute below answers true as well;
        // ScopedCurrentSessionWork lifts the thread's own staleness (not the latch).
        //
        // The static form answers false whenever no session is active, so monolith and the pull
        // build get the honest answer with no branch of their own.
        static Bool DeviceLost();
        // The latch alone: is the ACTIVE session lost. What the EGL layer asks before recovering.
        static Bool SessionLatchedLost();

        // Arms the latch and says why, once. Safe to call repeatedly and from any thread; only
        // the first call logs.
        void LatchDeviceLost(const char* why);

        // A reply-owning record came back DECLINED where only a latched server declines (a
        // readback). The server publishes its SessionFault on the control plane BEFORE it declines,
        // so the frame - or the hangup that follows it - is what tells "lost" from a refusal. Pumps
        // the control plane and asks the bell for up to `waitMs`; true when the session is lost.
        Bool ConfirmLossAfterDecline(Uint32 waitMs);

        // THE RECOVERY. Ends this lost session (Stop) and dials the configured server for a fresh
        // one (StartSpawned); the new session forgets every client-side cache of the old one and
        // advances MG_State's wire epoch, which is what makes every older GLContext lost. The
        // caller (EGLImpl) holds the EGL operation lock, which every GL entry point also takes, so
        // nothing else is emitting. Refused by name - and the session stays latched - under
        // MOBILEGL_IPC_RECOVER=0, for a transport whose server cannot be dialed again (inproc,
        // fd:), and when the server does not answer a probe. OK when a fresh session is up.
        MobileGLResult RecoverAfterDeviceLoss();
        // Sessions brought up in this process so far (the recovery's arm proof for a test).
        static Uint64 SessionsStarted();

        // While one is open on a thread, that thread's emissions are DECLINED as on a lost
        // device: the lost contexts' objects die under it, and their deaths must not reach the
        // fresh session.
        struct ScopedWireMute {
            ScopedWireMute();
            ~ScopedWireMute();
            ScopedWireMute(const ScopedWireMute&) = delete;
            ScopedWireMute& operator=(const ScopedWireMute&) = delete;
        };
        // While one is open on a thread, the thread's own GL state does not make it "lost": EGL
        // work that targets the current session on behalf of a thread whose current context is
        // a lost one (it creates the replacement context, re-creates a surface).
        struct ScopedCurrentSessionWork {
            ScopedCurrentSessionWork();
            ~ScopedCurrentSessionWork();
            ScopedCurrentSessionWork(const ScopedCurrentSessionWork&) = delete;
            ScopedCurrentSessionWork& operator=(const ScopedCurrentSessionWork&) = delete;
        };

        // The bell this session parks on, for the `dl` red-once ONLY. A test cannot otherwise
        // ask the question the latch turns on - "did the peer hang up" is a fact about a
        // descriptor, and the pair of tests that pin D5c has to be able to kill a server and
        // then watch this object, not a side effect three layers up.
        Transport::Doorbell* SelfDoorbellForTest() { return m_producer.SelfDoorbell(); }

        // PH-6's fuzz arm 3 ONLY (EventForfeitPeerTest): the control stream, so a peer that has
        // stopped draining can still put on it the frames no ClientSession path would send at that
        // moment - a malformed one, a surface op queued behind a wait it did not drain for, a
        // LogFlush every 100 ms, or a half-close. Every production path that writes control holds
        // m_remoteControlMutex and drains first; a caller of this does neither, which is the
        // point. Null unless spawned.
        Transport::SocketTransport* ControlSocketForTest() { return m_socketTransport.get(); }
        // P9 W3's fault injection F2 ONLY (EventForfeitPeerTest): the control READER, so a TCP peer
        // can stop taking anything off its control connection - the server's forwarded log lines
        // included - and later resume (ControlInbox::PauseReadingForTest). False when there is no
        // reader to pause (not a stream session).
        bool PauseControlReaderForTest(bool paused) {
            if (!m_controlInbox) return false;
            m_controlInbox->PauseReadingForTest(paused);
            return true;
        }

        // CONTRACT-P6 4.3: THE PID THE SERVER STATED IN ITS Welcome, which is not the same fact
        // as the pid this session's launcher recorded - that one is local knowledge, this one
        // CROSSED THE WIRE. Under spawn they must agree and neither may be ours; under inproc
        // the server states its own pid, which IS ours, and that is the honest answer there.
        std::uint32_t PeerServerPid() const { return m_peerServerPid; }

        ClientSession();
        ~ClientSession();
        MobileGLResult AttachStreamLink(int dataFd, const Transport::SessionSegmentSizes& sizes);
        void AttachDataLink(std::unique_ptr<Transport::ILink> link);
        Transport::ILink* DataLink() const { return m_link.get(); }
        bool ConnectDial() const { return m_connectDial; }
        void SyncPeerLog();

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
        // `cp`: the remote control sink ServerLoop calls when the server is
        // another process. Encode, send, wait for the SurfaceReply that carries
        // OUR seq back.
        static MobileGLResult RemoteControlSinkThunk(void* user, Server::SurfaceControlFrame& frame);
        MobileGLResult RunRemoteSurfaceControlFrame(Server::SurfaceControlFrame& frame);

        MobileGLResult FinishStartup(Transport::Doorbell* peerBell,
                                     Transport::Doorbell* selfBell,
                                     bool startApplyThreadHere);

        // `transport` is this process's end of a connection to an ALREADY
        // RUNNING server process. The two bells arrive over it, with the four
        // segments, as SCM_RIGHTS offers - nothing here is inherited, because
        // the two processes were started independently.
        // P6: launch a server process and connect to it. The two are
        // INDEPENDENT - the launcher hands over a rendezvous name and nothing
        // else - so this is the same code path a client would use against a
        // server that was already running when it started.
        MobileGLResult StartSpawned();

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
                                Uint64* replySizeOut = nullptr, Bool wantReply = true,
                                Bool willReadReply = true);

        // ---- P11 B2: T0 (MG_Remote/CONTRACT-P11.md B2) ------------------------------------
        //
        // Does this session adopt persistent stores through T0? Decided ONCE, lazily, at the first
        // map_persistent - the first moment it matters, and one at which the server's first native
        // bind has certainly republished its caps (a buffer store cannot exist before a
        // make-current, and the make-current adopts what the bind published): the Hello asked T0
        // (MOBILEGL_IPC_ADOPT_TIER=0 over shared segments) AND the server published kCapAdoptT0.
        // A T0 the session asked for and cannot use is T2 plus ONE named line here
        // (`Refuse{AdoptT0Unavailable, ...}`), never a Fatal.
        Bool AdoptT0();
        // T0's map_persistent: allocate the AHB, hold its lock, seed it from `seed` (the shadow),
        // queue its Offer for the record's seq BEFORE the record is published (the order the
        // server's inbox relies on), and wait for the answer. The held pointer on OK - the caller
        // adopts it as the store's mapping - or null on DECLINED (the store stays T2).
        void* AdoptPersistentT0(const MG_Pipe::MGPHandleOnly& handle, Uint64 size, const void* seed);
        // shared_image (docs/Disaggregated/notes/anland/plan-ahb-dmabuf.md): emits `op` and waits
        // for its answer. `sendFd` >= 0 (an Import) is queued on the aux socket for the record's
        // seq BEFORE the record is published; `receivedFd` non-null (an Allocate) takes the
        // descriptor the server queued before its answer. False = DECLINED (the server's log says
        // why) or the descriptor did not travel.
        Bool EmitSharedImage(const MG_Pipe::MGPSharedImageOp& op, int sendFd, MG_Pipe::MGPSharedImageReply* reply,
                             int* receivedFd);
        // The client's reference to buffer `handle`'s T0 store ends because the buffer was
        // respecified or destroyed: unlock and release it. The server's import holds its own
        // reference until its own fence for the store's last use has completed. No-op for a
        // handle with no T0 store.
        void ReleaseT0Store(MG_Pipe::MGPipeHandle handle);
        // Per-session T0 arm proof (B2): stores the server imported, their bytes, and the
        // map_persistent answers that came back DECLINED in a T0 session.
        Uint64 T0StoresImported() const { return m_t0Imported; }
        Uint64 T0BytesImported() const { return m_t0Bytes; }
        Uint64 T0Declined() const { return m_t0Declined; }
        // 1 when this session's knob asked for T0 (0, or unset) and it runs T2 instead: a stream
        // data plane, or no kCapAdoptT0 at the first map_persistent. Counted whether the fallback
        // was named (knob 0) or quiet (unset, the default).
        Uint64 T0Fallbacks() const { return m_t0Fallbacks; }
        // Test seam: 0 = queue each Offer before its record (the product order); > 0 = queue it
        // that many ms AFTER the record is published, from another thread; < 0 = never queue it.
        // What the host tests use to prove a late Offer still lands and a lost one is a named
        // DECLINED rather than a hang.
        static void SetT0OfferFaultForTest(Int32 delayMs);

        // `willReadReply` (P12) and `wantReply` ARE TWO QUESTIONS AND MUST STAY TWO.
        //
        //   wantReply      does THIS CALL read the answer (it blocks on the barrier and takes the
        //                  reply slot before returning);
        //   willReadReply  will the answer be read AT ALL - now, or later by somebody else.
        //
        // The create window is the case that makes the difference load-bearing: it emits with
        // wantReply=false and its answers are read afterwards by the window's drain, so a record
        // it emits must still be ANSWERED. Only willReadReply=false tells the server it may skip
        // the answer (Transport::kRecNoReply), and only three sites may say it: the parameter row
        // for a confirmed object, the respecify row's fire-and-forget half, and the sub-data row
        // - each of which answers its own caller locally and reads nothing from the wire.
        //
        // THE TWO ILLEGAL COMBINATIONS ARE FATAL BY NAME rather than ignored, because both would
        // fail silently in opposite directions: wantReply && !willReadReply is a caller blocking
        // on an answer it has just told the server not to send, and !willReadReply on a row with
        // no reply slot is a caller discarding an answer that never existed.

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
        // P10 (CONTRACT-P10.md §1): a polling entry point (glClientWaitSync, glGetSynciv(
        // GL_SYNC_STATUS)) is a doorbell point. Everything published so far - the polled fence's
        // own create record above all - is flushed to the server, and the reverse channel is
        // drained, so a fence the server has since reported is known before the poll answers.
        // Publishes nothing and waits for nothing.
        void PollEntry();
        // Submit published commands without waiting for application. This is
        // glFlush's contract and the frame-submit boundary for Present.
        void Flush();

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

        // eglSwapBuffersWithDamage's damage for the present about to be emitted (BackendObject_Remote
        // sets it around the swap; EmitPresent takes it into the record). Taking clears it, so a
        // plain swap after it carries none (the whole surface).
        void SetPendingPresentDamage(const MG_Util::Damage::Region& damage) {
            m_pendingDamageCount = MG_Util::Damage::PackRects(damage, m_pendingDamage, MG_Pipe::kMGPMaxDamageRects);
        }
        Uint32 TakePendingPresentDamage(Int32* out) {
            const Uint32 count = m_pendingDamageCount;
            std::memcpy(out, m_pendingDamage, sizeof(Int32) * 4 * count);
            m_pendingDamageCount = 0;
            return count;
        }

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

        // P12: HOW MANY REPLY-OWNING RECORDS THIS CLIENT HAS EMITTED, the clock a deferred
        // answer's RESIDENCY is measured against. An answer is readable only until the bounded
        // reply buffer reuses its slot, so a caller that defers an answer (the create window)
        // has to know how many other answers have gone by since it was posted.
        //
        // EVERY reply-slot row is counted, not only the rows this client waits for, and that is
        // the conservative direction on purpose: a server that ignores the no-reply bit answers
        // records the client does not read, and those answers displace a deferred one just the
        // same. A budget that counted only "answers I will read" would be wrong in exactly the
        // deployment where it matters most.
        Uint64 ReplyPostings() const;

        // P12: DOES THIS LINK KEEP AN ANSWER UNTIL ITS READER TAKES IT? The create window defers
        // answers across an unbounded amount of other traffic, which only works where the answer
        // is still there when the drain comes back - measured: a bounded buffer of the last N
        // answers never is. On a link that answers false the window must not defer at all, so
        // this is what CreateWindowEffective() is gated on.
        Bool LinkRetainsReplies() const;

        // The reply slot for `seq`, addressed seq % slots with the seq stamped back into the
        // header for self-check (R-3). `outStatus` is 0 OK / 1 DECLINED / 2 ERROR, and
        // DECLINED IS A REAL ANSWER - MapPersistent's nullptr and the four Bool acceptances.
        Bool ReadReply(Uint64 seq, void* outBytes, Uint64 outCapacity, Int32* outStatus,
                       Uint64* outSize);
        // What one answer may carry. No single answer is ever bigger than this: a ReadPixels
        // whose whole read is bigger is split by the emitter into row bands (or, for a row
        // wider than a reply, single-row pieces) that each fit (g5-readback, EmitTables.h
        // PlanReadbackBands), and the client still checks every band BEFORE it emits.
        Uint32 MaxReplyBytes() const;
        // ID-47, the fifth primitive: true exactly when an answer of `bytes` can be posted.
        Bool ReplyCanHold(Uint64 bytes) const;
        // ID-47's named refusal, forwarded verbatim to ReplySlotPool::RequireReadPixelsFits.
        // Returns when the answer fits; otherwise
        //     Fatal{ReplyTooLarge, "ReadPixels <w>x<h> <format> <bytes> > <cap>"}
        // and abort - AT THE CLIENT, BEFORE EMISSION. Package c1's OnReadPixels emitter calls
        // this immediately before each EmitAndWait(MGPWireOp::ReadPixels, ...), with that
        // band's box, its Format/Type enums and the DstSize it computed - and, when not even one
        // pixel fits a reply, once for that one pixel instead of emitting; the server's Post
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

        // P12 (on-screen server window), D1: THE GEOMETRY FLOWS BACK. `surface` (the client's EGL
        // handle) is this session's server-owned window surface, `width`x`height` the server
        // window's real extent from the CreateWindowSurface reply (0 = not known yet). The extent
        // goes into the EGL state at once - eglQuerySurface answers it before eglCreateWindowSurface
        // returns - and every later surface-changed event that carries an extent (the server window
        // resized or rotated) is applied to the same surface, beside the default-framebuffer
        // reallocation the drain already does. Every server-owned surface is tracked (they are all
        // on the server's one window, so each extent applies to all of them), and
        // ForgetServerOwnedWindowSurface drops one when the client releases it.
        void NoteServerOwnedWindowSurface(EGLSurface surface, Uint32 width, Uint32 height);
        void ForgetServerOwnedWindowSurface(EGLSurface surface);
        // Whether `surface` is one of them (the resize path asks: it resizes the server's window).
        static Bool IsServerOwnedWindowSurface(EGLSurface surface);

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
        MobileGLResult ReceiveControlFrame(std::vector<Uint8>& frame, Uint32 timeoutMs);
        std::unique_ptr<Transport::ControlInbox> m_controlInbox;
        std::unique_ptr<Transport::ILink> m_link;
        bool m_connectDial = false;
        std::uint64_t m_logFlushSeq = 0;
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
        Uint32 m_pendingDamageCount = 0;
        Int32 m_pendingDamage[MG_Pipe::kMGPMaxDamageRects * 4] = {};
        Uint64 m_presentCreditWaits = 0;

        // sm: the spawn client owns ONE end and two bells. Held here rather
        // than in the transport because which bell is "mine" is the session's
        // knowledge, not the transport's - the same ruling as inproc's
        // PeerDoorbell/SelfDoorbell split.
        // cp: one control op at a time, and the client's own seq space.
        std::mutex m_remoteControlMutex;
        Uint64 m_remoteControlSeq = 0;
        // P7 CI: false until this session's first MakeCurrent is answered ok. Until then the
        // ops the server may bring its native backend up inside wait MOBILEGL_IPC_COLD_START_MS
        // for their reply instead of MOBILEGL_IPC_CONTROL_TIMEOUT_MS (Config.h has both). Either
        // budget bounds SILENCE (p7/spawnhang): a Wire::SurfaceProgress for the op restarts it.
        // Guarded by m_remoteControlMutex, like the seq above; reset by Stop().
        Bool m_serverBackendWarm = false;

        // The server we launched, if we launched one. Reaped in Stop().
        Server::LaunchedServer m_spawned;

        std::unique_ptr<Transport::SocketTransport> m_socketTransport;
        // PH-7 (4): the `tcp://` endpoint the control connection went to, for the data
        // connection opened after Welcome. Empty on every other transport.
        std::string m_dataEndpoint;
        std::unique_ptr<Transport::Doorbell> m_socketSelfBell;
        std::unique_ptr<Transport::Doorbell> m_socketPeerBell;

        std::unique_ptr<Transport::InProcessTransport> m_clientTransport;
        std::unique_ptr<Transport::InProcessTransport> m_serverTransport;
        Transport::RingProducer* m_cmd = nullptr;
        Transport::SessionProducer m_producer;
        Transport::EventRingConsumer* m_events = nullptr;
        Transport::ITransport* m_transport = nullptr;
        Bool m_started = false;
        Uint64 m_replyPostings = 0; // P12: the residency budget's clock (ReplyPostings())
        // Written by whichever thread first notices the hangup - the GL thread in the barrier,
        // or the event pump - and read by glGetGraphicsResetStatus on the GL thread.
        std::atomic<bool> m_deviceLost{false};
        // Welcome::serverPid, kept because §9.5's arm proof wants it and because a value that is
        // only ever logged cannot be asserted on.
        std::uint32_t m_peerServerPid = 0;

        // P11 B2 (T0). The Hello's ask, settled at the handshake; the lazy decision (-1 = not
        // yet, 0 = T2, 1 = T0); the stores this client holds by handle key (Gen << 32 | Slot);
        // the counters; and the one-shot hook EmitAndWaitTails runs between EncodeRecord (the
        // seq exists) and PublishAndNotify (the record is visible) - where the Offer is queued.
        Bool m_adoptT0Asked = false;
        Bool m_adoptT0Quiet = false; // the knob is unset: fallbacks at MGLOG_D
        Int32 m_adoptT0Decision = -1;
        std::unordered_map<Uint64, Transport::AdoptT0::HeldStore> m_t0Stores;
        Uint64 m_t0Imported = 0;
        Uint64 m_t0Bytes = 0;
        Uint64 m_t0Declined = 0;
        Uint64 m_t0Released = 0;
        Uint64 m_t0Fallbacks = 0;
        std::function<void(Uint64)> m_prePublish;
        void ResetT0(Bool releaseStores);
        // Every client-side cache keyed to the session that just ended, dropped before the next
        // one serves a record (FinishStartup, on every start after the first).
        void ForgetEndedSession();
    };

    // One per process in P5, because P5 serves one context, and LEAKED AT EXIT like every other
    // MG_Remote singleton (ID-8): no frontend destructor may reach pipe or backend state from an
    // exit handler, and a session destroyed before them would be a use-after-free rather than a
    // tidy teardown. MG_Backend::Init() calls Start() on this one.
    ClientSession& ClientSessionInstance();

    // F1 (P7 wave 2). Is a session bring-up running anywhere in this process right now.
    //
    // CapsMirror asks a narrower question through its own hook (SetCapsFirstSnapshotWait) and
    // this is the half a TEST needs: a case that wants to read the mirror DURING a handshake
    // has to know when the window has opened, and inferring it from a log line or a sleep is
    // how such a case starts passing for the wrong reason. True from the first line of Start /
    // StartSpawned / StartOverSocket / StartOverTransportPair until FinishStartup returns.
    Bool BringUpInFlight();

} // namespace MobileGL::MG_Remote::Client
