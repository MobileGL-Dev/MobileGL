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

#include "../Wire/PipeWireCodec.h"
#include "CapsMirror.h"

namespace MobileGL::MG_Remote::Client {

    class ClientSession {
    public:
        // Null until Start() succeeds; MG_Backend::Init() is the only caller of Start().
        static ClientSession* Active();

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

    private:
        Wire::PipeWireEncoder m_encoder;
        Wire::SegmentTable m_segments;
        CapsMirror* m_caps = nullptr;
        Bool m_barrierArmed = true;
    };

} // namespace MobileGL::MG_Remote::Client
