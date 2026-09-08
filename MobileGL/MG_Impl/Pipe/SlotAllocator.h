// MobileGL - MobileGL/MG_Impl/Pipe/SlotAllocator.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipeHandles.h>

// The CLIENT's slot allocator: the thing that mints every MGPipeHandle in the system
// (ARCHITECTURE.md 4.2 - no create_* call in the catalogue returns a server-cast handle,
// which is what lets the whole catalogue be remoted with zero creation round trips).
//
// Per kind: a free list plus a high-water mark, so slots stay DENSE and the server's object
// table is an array rather than a hash map. It has nothing to do with MG_State's
// IndexGenerator - that container's LIFO GL-name reuse is the very problem {slot, gen}
// exists to close, and the whole point of the identity is that an ABA on the GL name, on
// the heap address or on the lifetime id cannot reproduce a handle.
//
// Gen increments ONLY when a slot is reused, never on a respecify: a glBufferData on a live
// buffer keeps the same {slot, gen}, because the object is the same object. Two generations
// exist in the design and they are strictly separate - this is the client's answer to "is
// this still the same GL object"; MGGen is the server's epoch for "did I recast my driver
// object", and no MGPipe call may require the client to know it.
//
// The lifetimeId -> slot map is what keeps a GL NAME out of every key (ARCHITECTURE.md 4.2):
// the frontend object's lifetime id is the client's own identity for it, so the backend key
// is the handle and the frontend key is the lifetime id, and neither is a recyclable name.
//
// Lives in MG_Impl (the client side, unrestricted) and is compiled only under
// MOBILEGL_PIPE_PUSH. It is in the P2 CONTRACT commit rather than in a Track H package
// because both Track H slices - Espryt 0b and Magma subsystem 4 - key off it.
namespace MobileGL::MG_Pipe {

    class MGPipeSlotAllocator {
    public:
        static constexpr SizeT kKindCount = static_cast<SizeT>(MGPipeKind::KindCount);

        // A fresh {slot, gen} of this kind, from the free list if one is waiting and from the
        // high-water mark otherwise. Never returns slot 0 (reserved: null, and the default
        // framebuffer for kind Framebuffer), and never returns a ShaderCso slot inside the
        // composite band, which the program-pipeline resolver mints out of separately.
        MGPipeHandle Allocate(MGPipeKind kind);
        // Allocate and remember `lifetimeId` as this handle's frontend identity.
        MGPipeHandle AllocateFor(MGPipeKind kind, Uint64 lifetimeId);

        // P4a, D-H7: THE ONE ENTRY POINT INTO THE ShaderCso COMPOSITE BAND, and the only one
        // there will ever be. Allocate() above refuses that band on purpose, so a program
        // pipeline's flattened composite - minted client-side from the stage programs bound to
        // the pipeline object, and indistinguishable from an ordinary program to the server -
        // needs a door of its own rather than a flag on the handle. The kind is implied: only
        // ShaderCso has a band.
        //
        // It behaves exactly like AllocateFor in every other respect (free list first, then
        // the band's own high-water mark; Gen moves only on reuse; the lifetimeId -> slot map
        // is written) and it carries the band's own exhaustion assert, so exhausting the
        // composite space is a NAMED Fatal rather than silent slot theft from ordinary
        // programs. Returns kMGPipeNullHandle when the band is full.
        //
        // Freed through the ordinary Free(MGPipeKind::ShaderCso, handle): a composite's slot
        // has two independent release paths - the pipeline cache's LRU eviction and the
        // composite ProgramObject's own destructor - and Free refusing a slot that is not live
        // at that generation is what makes the second one a proven no-op.
        MGPipeHandle AllocateComposite(Uint64 lifetimeId);
        // The handle a lifetime id was allocated for, or kMGPipeNullHandle. A recycled heap
        // address does NOT reproduce a mapping: MG_State hands out a fresh lifetime id per
        // object, so the map key is unique for the life of the process.
        MGPipeHandle FindByLifetimeId(MGPipeKind kind, Uint64 lifetimeId) const;
        // FindByLifetimeId, then AllocateFor when it misses. The ordinary client path.
        MGPipeHandle Acquire(MGPipeKind kind, Uint64 lifetimeId);

        // Returns the slot to the free list. The Gen bump happens on the NEXT handout of that
        // slot, not here, so a handle that is freed twice cannot skip a generation and the
        // "gen moves only on reuse" contract holds for an object that is never reused.
        void Free(MGPipeKind kind, MGPipeHandle handle);

        Bool IsLive(MGPipeKind kind, MGPipeHandle handle) const;
        // 0 for a slot that was never handed out; the generation of the LAST handout
        // otherwise, live or not.
        Uint32 GenOfSlot(MGPipeKind kind, Uint32 slot) const;
        Uint64 LifetimeIdOfSlot(MGPipeKind kind, Uint32 slot) const;
        // One past the highest slot ever handed out of this kind - which for ShaderCso means
        // the COMPOSITE band's top once a composite has been minted, because that really is
        // the highest slot handed out. It is what the leak cases read (a leaked slot of any
        // kind, composite included, moves it), and it is NOT a table size for kind ShaderCso:
        // the band is sparse against the ordinary space by design, so a consumer indexing by
        // slot keeps the band in a table of its own, exactly as this allocator does.
        Uint32 HighWater(MGPipeKind kind) const;
        Uint32 LiveCount(MGPipeKind kind) const;
        Uint32 FreeCount(MGPipeKind kind) const;

        // Context teardown / server reset / a unit test's fixture.
        void Reset();

    private:
        struct SlotState {
            Uint32 Gen = 0;
            Bool Live = false;
            Bool EverHandedOut = false;
            Uint64 LifetimeId = 0;
        };

        struct KindState {
            // Indexed by slot; [0] is the reserved slot and is never live.
            Vector<SlotState> Slots;
            Vector<Uint32> FreeList;
            // P4a: the ShaderCso COMPOSITE band, indexed by (slot - the band's base) and
            // EMPTY for every other kind. A SECOND VECTOR RATHER THAN MORE OF THE FIRST, and
            // it is not a micro-optimisation: the band starts at 983040, so minting one
            // composite into the slot-indexed vector above would allocate ~983k SlotStates -
            // ~23 MB - for a single program pipeline, and a consumer that sized a table off
            // HighWater would pay the same shape again with a far bigger record. Both spaces
            // stay dense against their own high-water mark, which is the property this
            // allocator exists to give the server.
            Vector<SlotState> BandSlots;
            Vector<Uint32> BandFreeList;
            UnorderedMap<Uint64, Uint32> ByLifetimeId;
            Uint32 LiveCount = 0;
        };

        KindState& StateOf(MGPipeKind kind);
        const KindState& StateOf(MGPipeKind kind) const;
        // The SlotState a (kind, slot) names, in whichever of the two vectors holds it, or
        // null when the slot has never been handed out. One resolver, so a caller that forgets
        // the band cannot exist.
        static SlotState* EntryOf(KindState& state, MGPipeKind kind, Uint32 slot);
        static const SlotState* EntryOf(const KindState& state, MGPipeKind kind, Uint32 slot);

        Array<KindState, kKindCount> m_kinds{};
    };

    // The monolith's one client allocator. Under split there is one per client context.
    MGPipeSlotAllocator& MGPipeSlots();
} // namespace MobileGL::MG_Pipe
