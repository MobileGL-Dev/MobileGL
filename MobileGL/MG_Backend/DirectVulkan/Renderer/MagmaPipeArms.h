// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <Config.h>
#if MOBILEGL_PIPE_PUSH
// kMGPipeSubsystem* - the runtime bitmask's named bits - and MGPipeHandle itself. Both are
// header-only constant/POD declarations, and both are push-only, so the pull build's include
// graph is unchanged (G1).
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHandles.h>
#endif

#include <cstdlib>

// Magma's arm selector for the P2 Track H / render-state re-keys (P2 brief D14), and the
// bounded {slot, gen} mint the re-keyed sites are written against.
//
// Two switches decide which arm a re-keyed site runs, and they are NOT the same switch:
//
//   MOBILEGL_PIPE_PUSH (compile)          - is the pushed state there to be keyed on at all
//   Features.PipePush  (runtime bitmask)  - is THIS subsystem migrated in THIS run
//   MOBILEGL_PIPE_LEGACY_MEMOS (compile)  - is the pre-handle arm compiled beside it
//   Features.PipeLegacyMemos (runtime)    - may the pre-handle arm be ENTERED in this run
//
// ARCHITECTURE.md 9.6's point: once a handle wave lands, a clear MOBILEGL_PIPE_PUSH bit is
// only a valid A/B while the legacy arm is still compiled, because with the bit clear the
// backend would otherwise still run the re-keyed code. So a clear bit selects the legacy
// arm, and a run that has explicitly disabled the legacy arm may not fall into it.
//
// D14 spends that last sentence at STARTUP, not per draw: "a Track-H subsystem whose bit is
// clear is a startup Fatal{PipeLegacyMemosDisabled}". Nothing in the draw path aborts, and
// nothing outside Track H consults the legacy-memo lever at all - see
// MagmaPipeValidateSubsystemConfiguration below for both halves of that rule.
//
// The whole header is inert in a pull build: MOBILEGL_PIPE_PUSH is 0 there, every helper
// below is behind it, and the pull build's translation units are byte-identical (G1).
namespace MobileGL::MG_Backend::DirectVulkan {

#if MOBILEGL_PIPE_PUSH
    // Is `subsystemBit` (MG_Pipe/MGPipe.h's kMGPipeSubsystem*) migrated in this run?
    inline Bool MagmaPipeSubsystemOn(Uint64 subsystemBit) {
        return (MG_Config::Features.PipePush & subsystemBit) != 0;
    }

    // ---------------------------------------------------------------------------------
    // D14's startup gate
    // ---------------------------------------------------------------------------------
    //
    // Called once from VulkanRenderer::Initialize(), i.e. only when Magma is the backend
    // that is actually running. It answers exactly one question and it answers it before the
    // first draw: is there an arm for Magma's Track-H subsystem in this configuration?
    //
    // Three deliberate boundaries, each of which the per-draw shape this replaces got wrong:
    //
    //  * ONLY Magma's own Track-H bit is checked. Espryt's bit 5 is Espryt's business (a
    //    DirectVulkan run does not execute one line of DirectGLES' re-key), so
    //    MOBILEGL_PIPE_PUSH=0x20 must not kill a Magma run, and MOBILEGL_PIPE_PUSH=0x40 must
    //    not kill an Espryt one.
    //  * bit 0 (kMGPipeSubsystemRenderState) is NOT Track H and is NOT checked. It is not a
    //    memo re-key at all: it decides where the pipeline memo's STATE KEY comes from, and
    //    a clear bit there simply means the client is not pushing render-state CSOs in this
    //    run, which GetOrCreatePipeline answers with its own state hash. D14 labels bits 5
    //    and 6 "Track H" and labels bit 0 nothing of the sort.
    //  * it is Fatal at STARTUP, once, not on a draw. A per-draw abort inside
    //    GetOrCreatePipeline turns a configuration mistake into a mid-frame crash and puts a
    //    branch nobody needs on the hottest path in the backend.
    inline void MagmaPipeValidateSubsystemConfiguration() {
#if MOBILEGL_PIPE_LEGACY_MEMOS
        // The pre-handle arm is compiled AND the operator has not forbidden entering it, so a
        // clear bit is an ordinary, valid A/B: the site takes the legacy arm.
        if (MG_Config::Features.PipeLegacyMemos) return;
#endif
        if (MagmaPipeSubsystemOn(MG_Pipe::kMGPipeSubsystemMagmaVertexInput)) return;
#if MOBILEGL_PIPE_LEGACY_MEMOS
        const char* const why = "this run has MOBILEGL_PIPE_LEGACY_MEMOS=0";
#else
        const char* const why =
            "this build has cmake -DMOBILEGL_PIPE_LEGACY_MEMOS=OFF, which compiles no such arm";
#endif
        MGLOG_F("MGPipe: Fatal{PipeLegacyMemosDisabled} Magma's Track-H subsystem "
                "(kMGPipeSubsystemMagmaVertexInput, bit 6 of MOBILEGL_PIPE_PUSH) is clear, so the "
                "vertex-input cache and the VAO draw memo want the pre-handle arm - but %s. Set "
                "bit 6 (MOBILEGL_PIPE_PUSH=0x%llx, or the default 0x%llx), or allow the legacy arm.",
                why,
                static_cast<unsigned long long>(MG_Config::Features.PipePush |
                                                MG_Pipe::kMGPipeSubsystemMagmaVertexInput),
                static_cast<unsigned long long>(MG_Pipe::kMGPipeSubsystemsMigratedAtP2));
        std::abort();
    }

    // "Does this Track-H site run the handle arm?" - the ONE question every re-keyed Track-H
    // site asks, so that they cannot disagree with each other or with the startup gate.
    inline Bool MagmaPipeTrackHArmIsHandles(Uint64 trackHBit) {
#if MOBILEGL_PIPE_LEGACY_MEMOS
        return MagmaPipeSubsystemOn(trackHBit);
#else
        // No pre-handle arm exists in this build, and MagmaPipeValidateSubsystemConfiguration
        // has already made a clear bit a startup Fatal, so the handle arm is the only arm a
        // running process can be on.
        (void)trackHBit;
        return true;
#endif
    }

    // ---------------------------------------------------------------------------------
    // The {slot, gen} mint
    // ---------------------------------------------------------------------------------
    //
    // A FIXED-CAPACITY, SELF-RECYCLING identity table: 2-way set-associative, indexed by the
    // frontend object's lifetime id, LRU victim within the set, and Gen incremented whenever
    // a slot changes owner. It hands out slots in [kMGPipeFirstAllocatableSlot, Count], so a
    // consumer's per-slot table is a BIJECTION with this one - one entry per slot, no
    // masking, no collision, no probe.
    //
    // Why not MG_Impl/Pipe/SlotAllocator (the client's allocator, which is what mints handles
    // in the finished design)? Because in P2 nothing on this side ever frees one. The tracker
    // does not emit object-class state yet (P2 emits for dirty bits 0-4), so no create_*/
    // delete_* pair travels for a VAO or a buffer, and the frontend has no death notification
    // Magma could hook: BufferBackendOps::OnDestroy is handed a BackendBufferResource, not the
    // BufferObject, and fires only for a buffer that ever had one, while VertexArrayObject has
    // no hook at all (adding one is D13's explicit-destroy work, and it covers Espryt's six
    // kinds, not VertexElementsCso). An allocator with a live Allocate and a dead Free grows
    // by one SlotState plus one hash-map node per object EVER created, for the life of the
    // process, on a platform with an LMK - and its slot numbers then grow monotonically with
    // objects ever created, which is exactly what would make a slot-indexed table collide.
    //
    // So Magma mints its own, bounded, and says so. This is a P2 STAND-IN either way (the
    // client is what mints handles once object-class state travels); what it must not be is a
    // leak. Recycling costs the same thing the address-hashed table it replaces cost: a
    // colliding pair of live objects evicts each other and re-derives. It is strictly better
    // than that table, because the {slot, gen} compare is an exact identity, so an eviction
    // can only ever cost a recompute - never the ABA the lifetime-id compare was added for.
    //
    // Single-threaded, like MGPipeSlots() and like the rest of the renderer.
    class MagmaPipeIdentityTable {
    public:
        explicit MagmaPipeIdentityTable(Uint32 entryCount) : m_entryCount(entryCount) {}

        // One entry per slot, so a consumer table sized Count() and indexed by
        // MagmaPipeSlotIndex() has exactly one entry per handle this table can hand out.
        Uint32 Count() const { return m_entryCount; }

        MG_Pipe::MGPipeHandle Acquire(Uint64 lifetimeId) {
            // Unreachable: MG_State hands out lifetime ids from 1 precisely so that a
            // zero-initialised memo slot cannot carry a live object's id. Guarded anyway so
            // that a zero can never be minted into a slot and then indexed with.
            if (lifetimeId == 0) return MG_Pipe::kMGPipeNullHandle;
            if (m_entries.empty()) m_entries.resize(m_entryCount);

            // Lifetime ids are monotonic from 1, so the low bits ARE the dense index: object
            // n and object n+1 land in adjacent sets. No mix, because there is no entropy to
            // spread - a multiply here would only scatter a sequence that is already perfect.
            const Uint32 set = static_cast<Uint32>(lifetimeId) & (SetCount() - 1u);
            const Uint32 way0 = set * 2u;
            const Uint32 way1 = way0 + 1u;

            if (m_entries[way0].LifetimeId == lifetimeId) return Touch(way0);
            if (m_entries[way1].LifetimeId == lifetimeId) return Touch(way1);

            // Miss. Evict the set's least recently used way - the same victim rule the
            // address-hashed VaoDrawMemo table used, kept here so that it lives in ONE place
            // instead of once per consumer table.
            const Uint32 victim = (m_entries[way0].LastUse <= m_entries[way1].LastUse) ? way0 : way1;
            Entry& entry = m_entries[victim];
            // The one place Gen may move, and it moves on REUSE: a respecify of the same
            // object keeps its {slot, gen} because its lifetime id still matches above.
            MOBILEGL_ASSERT(entry.Gen != ~Uint32{0},
                            "Magma handle generation wrapped on slot %u; {slot, gen} is no longer "
                            "unique",
                            victim + MG_Pipe::kMGPipeFirstAllocatableSlot);
            ++entry.Gen;
            entry.LifetimeId = lifetimeId;
            return Touch(victim);
        }

    private:
        struct Entry {
            Uint64 LifetimeId = 0;
            Uint32 Gen = 0;
            Uint32 LastUse = 0;
        };

        Uint32 SetCount() const { return m_entryCount / 2u; }

        MG_Pipe::MGPipeHandle Touch(Uint32 index) {
            m_entries[index].LastUse = ++m_clock;
            return MG_Pipe::MGPipeHandle{index + MG_Pipe::kMGPipeFirstAllocatableSlot,
                                         m_entries[index].Gen};
        }

        Uint32 m_entryCount = 0;
        // Wraps every 2^32 acquisitions. A wrapped clock can only ever pick the wrong victim
        // inside one set - a cache decision, never a correctness one.
        Uint32 m_clock = 0;
        Vector<Entry> m_entries;
    };

    // The table entry a handle names. Every per-slot table Magma keeps is sized Count() and
    // indexed by this, so the index is exact and in range by construction.
    //
    // A null handle has no slot, and it is unreachable here: both lifetime-id sources start at
    // 1 (VertexArrayObject.cpp, BufferObject.cpp), so Acquire's zero guard never fires.
    // Asserted rather than assumed, because being wrong about it would be an out-of-range
    // index rather than a wrong answer.
    inline Uint32 MagmaPipeSlotIndex(const MG_Pipe::MGPipeHandle& handle) {
        MOBILEGL_ASSERT(!MG_Pipe::MGPipeHandleIsNull(handle),
                        "a null MGPipeHandle has no slot to index a per-slot table with");
        return MG_Pipe::MGPipeHandleIsNull(handle)
                   ? 0u
                   : handle.Slot - MG_Pipe::kMGPipeFirstAllocatableSlot;
    }

    // A VAO is kind VertexElementsCso: that is the gallium-shaped CSO a vertex array resolves
    // to, and it is the only kind in MGPipeKind that names vertex-input state. 2048 entries
    // is what the address-hashed VaoDrawMemo table it replaces held, so the working set this
    // covers without eviction is unchanged; at 16 B/entry the table itself is 32 KB.
    inline constexpr Uint32 kMagmaVaoIdentityEntries = 2048;
    // Buffers are far more numerous than VAOs (Minecraft cycles chunk vertex/index buffers),
    // and unlike the VAO table this one feeds a CONTENT hash: an eviction changes the key a
    // vertex-input cache entry was built under, so it costs a rebuild rather than a lookup.
    // It is only ever consulted when a VAO's configuration version moved (ComputeHash is
    // memoised per VAO), so the price is paid per reconfiguration, not per draw - but the
    // table is sized four times the VAO one anyway, 128 KB, to keep it rare.
    inline constexpr Uint32 kMagmaBufferIdentityEntries = 8192;

    inline MagmaPipeIdentityTable& MagmaPipeVaoIdentity() {
        static MagmaPipeIdentityTable table(kMagmaVaoIdentityEntries);
        return table;
    }
    inline MagmaPipeIdentityTable& MagmaPipeBufferIdentity() {
        static MagmaPipeIdentityTable table(kMagmaBufferIdentityEntries);
        return table;
    }

    // The {slot, gen} of a frontend object. `lifetimeId` is the client's own identity for the
    // object - never a GL name, never a heap address - so a deleted-and-recreated object at
    // the same address cannot reproduce a handle, which is precisely the ABA
    // HandleRecycleScenario reproduces.
    inline MG_Pipe::MGPipeHandle MagmaPipeHandleOf(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId) {
        return kind == MG_Pipe::MGPipeKind::Buffer ? MagmaPipeBufferIdentity().Acquire(lifetimeId)
                                                   : MagmaPipeVaoIdentity().Acquire(lifetimeId);
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace MobileGL::MG_Backend::DirectVulkan
