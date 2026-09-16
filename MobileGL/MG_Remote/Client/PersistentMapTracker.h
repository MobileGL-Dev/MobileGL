// MobileGL - MobileGL/MG_Remote/Client/PersistentMapTracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// THE BLOCK-GRANULARITY PERSISTENT-MAP PUSH (P5 R-6, CONTRACT-P5.md section 3, table 2's
// second set). Owner: package b1.
//
// WHY THIS EXISTS AT ALL. A coherent persistent map is the one buffer shape with no per-write
// API call: the application memcpys through the pointer and neither a serial, an epoch nor a
// record moves. In monolith that is free, because the pointer IS the backend's GPU storage -
// MEASUREMENTS.md:87 prices the adoption at p99 163 -> 21 ms and ~400 MB saved, and
// ARCHITECTURE.md:481 requires it to hold unmoved for the whole monolith track. Across a
// process boundary the adopted address is meaningless, so P5 runs at tier T2 (emulate): the
// client keeps the shadow, MGPipeApplyMapPersistent declines, and the bytes the application
// wrote with no call have to be SHIPPED. `persistent-map-push` (PipeStats `pmap`) is exactly
// those bytes, and it is structurally zero while adoption survives - which is why forcing T2
// and wiring the counter are one deliverable and not two.
//
// THE SET. `m_livePersistentMaps` is SyncPersistentMappedRange's own early-out chain
// (BufferObject.cpp:341-353) read as a membership test - mapped, NOT GPU-resident, Persistent,
// Write, NOT FlushExplicit, non-empty mapped range - and nothing else. Reading it as a
// predicate rather than re-deriving one is deliberate: the day that chain grows a sixth
// early-out, a re-derived predicate silently keeps pushing a buffer the monolith path stopped
// pushing, and the two arms diverge with no test able to say so. IsLivePersistentMap() is the
// single spelling; BufferObject::SyncPersistentMappedRange is held to it by a unit case.
//
// THE GRANULARITY. MOBILEGL_IPC_PERSISTENT_BLOCK_KB (default 64) blocks, keyed
// {MGPipeHandle, blockIndex} - the key is implicit, because a block ships as an ORDINARY
// resource_subdata record whose destination range is [blockIndex * blockBytes, + blockBytes).
// NO NEW RECORD KIND: the existing record is already chunked by
// MGPipeForEachSubDataRecordRange and already acceptance-gated, and a second way to say
// "these bytes go there" is a second way to get it wrong. A block size of 0 is E3(a)'s
// NEGATIVE CONTROL and means "push nothing" - not "one unlimited block" - so
// PersistentCoherentMapScenario must go red under it.
//
// PHASE 1 IS CONSERVATIVE, AND SAYS SO. The whole mapped span is pushed, by block, at every
// validate point; no dirty bits, no memcmp. Phase 2 (MOBILEGL_IPC_SHADOW_SHM, P6+) makes it
// precise, and ARCHITECTURE.md:499 grants it the right to be pulled forward if Phase 1 is
// unacceptable on the Create/Flywheel fixtures - the one place in the plan where a
// measurement may reorder phases.
//
// ORDERING IS THE CORRECTNESS PROPERTY, NOT THE GRANULARITY. In monolith the push is a memcpy
// on the same thread as the draw that follows it, so the bytes the application wrote before
// the draw are the bytes the draw sees. Under split both travel SEG_CMD in order, so the ring
// preserves it - PROVIDED the push is emitted AT the validate point and not lazily. A push
// deferred past its own draw record is the C-1 regression re-committed at the transport layer.

#pragma once
#include <Includes.h>

#include <Config.h>

namespace MobileGL::MG_State::GLState {
    class BufferObject;
}

namespace MobileGL::MG_Remote::Client {

    class PersistentMapTracker {
    public:
        // Client-role singleton (table 3: the MG_Impl/MG_Remote/Client singletons are
        // client-exclusive). Leaks at exit for ID-8's reason, once per role-local singleton.
        static PersistentMapTracker& Instance();

        // MOBILEGL_IPC_PERSISTENT_BLOCK_KB * 1024. Zero means the push is OFF (E3(a)).
        static Uint64 BlockBytes();
        // Transport != Monolith. The whole module is inert on the monolith path: an extra
        // resource_subdata record there would be new behaviour, which D-J forbids.
        static Bool PushIsArmed();

        // SyncPersistentMappedRange's early-out chain as a predicate. THE only spelling.
        static Bool IsLivePersistentMap(const MG_State::GLState::BufferObject& buffer);

        // Membership maintenance, both idempotent and both safe to call on a buffer that is
        // not a member. Called from BufferObject on every event that can move the predicate:
        // map, unmap, respecify, adoption, destruction.
        void NoteMapStateChanged(MG_State::GLState::BufferObject& buffer);
        void Forget(const MG_State::GLState::BufferObject& buffer);

        // One member's whole mapped span, by block. Re-checks the predicate first, so a
        // member that stopped being one (an adoption, an unmap that did not route through
        // NoteMapStateChanged) is dropped rather than pushed.
        void PushBlocksFor(MG_State::GLState::BufferObject& buffer);

        // THE VALIDATE-POINT HOOK. Every member, before the verb record is emitted.
        void PushAllMembers();

        SizeT MemberCount() const { return m_livePersistentMaps.size(); }
        Uint64 BlocksPushed() const { return m_blocksPushed; }
        Uint64 BytesPushed() const { return m_bytesPushed; }
        // Unit tests only: the counters are diagnostics, the set is not reset by it.
        void ResetCountersForTest() {
            m_blocksPushed = 0;
            m_bytesPushed = 0;
            m_blockZeroAnnounced = false;
        }
        void ClearForTest() {
            m_livePersistentMaps.clear();
            ResetCountersForTest();
        }

    private:
        // Keyed on BufferObject::GetLifetimeId(), which is globally unique and never reused -
        // never the GL name (LIFO-recycled by glGenBuffers) and never the heap address
        // (recycled by the allocator). The raw pointer is safe because every removal path is
        // explicit: ~BufferObject and ReleaseMemory both call Forget/NoteMapStateChanged, and
        // PushBlocksFor re-checks the predicate before it dereferences anything it kept.
        UnorderedMap<Uint64, MG_State::GLState::BufferObject*> m_livePersistentMaps;
        Uint64 m_blocksPushed = 0;
        Uint64 m_bytesPushed = 0;
        // E3(a)'s diagnostic is emitted ONCE per process. PushBlocksFor runs at every validate
        // point of every member, so an unlatched MGLOG_W would be one line per draw per mapping
        // - and a control that has to grep a log cannot tell a message that fired from a
        // message that flooded.
        Bool m_blockZeroAnnounced = false;
    };

    // What the client's emit table calls immediately BEFORE emitting any verb that can read a
    // buffer (draw, dispatch, readback, blit, present). It is a free function rather than a
    // method so the emit table does not have to name the singleton, and so the one-line call
    // reads as what it is: "publish everything the application wrote with no call".
    //
    // In P5 the 21 SyncPersistentMappedRange sites (CONTRACT-P5.md section 3: 9 Espryt + 12
    // Magma, MEASUREMENTS.md:111's 20 being one low) still stand where they are and route
    // into PushBlocksFor through BufferObject::SyncPersistentMappedRange, so the push already
    // happens at every point monolith pushes at. They retire into THIS call at P8, when the
    // draw-path binding walks move to the client.
    void PushPersistentMapsBeforeVerb();

    // R-6's tier gate, and the ONE spelling of it. True for MOBILEGL_IPC_ADOPT_TIER=2, the
    // only tier P5 implements; 0 (a real cross-process shared mapping) and 1 (a server-side
    // staging map) parse - so the negative control has a name before the thing it controls
    // exists - and are a NAMED refusal here rather than a silent fall back to T2. It is asked
    // by MGPipeApplyMapPersistent, which is where the decline is decided, so the client's
    // three adoption call sites keep their existing "null means declined" branch and the
    // map-persistent-roundtrips counter keeps counting ATTEMPTS in both arms (E3(c) asserts
    // mpr is equal between the monolith and the split arm, which is only true if the decline
    // happens after the count, on the applier's side of the emission).
    Bool AdoptTierIsEmulate();

} // namespace MobileGL::MG_Remote::Client
