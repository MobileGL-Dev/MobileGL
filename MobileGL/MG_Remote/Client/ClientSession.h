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

        // Teardown order matters and is table 3's fourth column: publish and let the server
        // drain, Doorbell::Kill() (the ONLY thing that wakes an apply thread parked on
        // kWaitForever, Doorbell.h:211-221), then join, and only then release anything an
        // emitter owns - a tail still referenced by an unapplied record is a use-after-free
        // the join is what prevents.
        void Stop();

        Wire::PipeWireEncoder& Encoder();
        CapsMirror& Caps();

        // Emit one record and, if the barrier is armed, wait for it. `replyOut`/`replyBytes`
        // name where a kReplySlot answer lands; pass {nullptr, 0} for a call that has none.
        // Returns the record's seq, which is also its reply-slot id.
        //
        // Waiting is spin(MOBILEGL_IPC_SPIN_US) then park, through Doorbell::Wait, with
        // producerParked set before blocking - the shape Doorbell.h:121 already implements.
        Uint64 EmitAndWait(MG_Pipe::MGPWireOp op, const void* payload, Uint64 payloadBytes,
                           const void* varTail, Uint64 varTailBytes, void* replyOut,
                           Uint64 replyBytes, Int32* statusOut);

        // MOBILEGL_IPC_VERB_BARRIER. False is the R-1 negative control and is EXPECTED to be
        // red; it must be run once and the way it goes red recorded.
        Bool BarrierArmed() const;

        // R-1's invariant made checkable rather than only written down: true while this
        // thread is inside a barrier wait. The apply thread sets its own flag on entry to the
        // applier; a debug/verify build asserts the two are never both true, and that the
        // client never touches gPipeInputs while the server is inside the applier.
        static Bool InBarrierWait();
        static Bool ApplyThreadIsInsideApplier();

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

        // The reverse channel's reading end: OnBufferWriteback / OnGpuWritten /
        // OnSurfaceChanged. Drained by the GL thread between verbs.
        Transport::EventRingConsumer& Events();

        Transport::RingControl* Control();
        Transport::SessionSegments& Shm();
        Transport::ITransport* Control_Plane();

        // Peak-RSS accounting for t1 (RoleMemory.h).
        Transport::RoleMemorySample SampleMemory() const;
        void LogMemory(const char* phase) const;

    private:
        Wire::PipeWireEncoder m_encoder;
        Wire::SegmentTable m_segments;
        Bool m_barrierArmed = true;

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
