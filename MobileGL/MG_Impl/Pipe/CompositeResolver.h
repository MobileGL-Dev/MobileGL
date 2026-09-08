// MobileGL - MobileGL/MG_Impl/Pipe/CompositeResolver.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// P4a's PROGRAM-PIPELINE COMPOSITE, on the client side.
//
// GLContext::GetProgramForDraw() already flattens a bound pipeline into one hidden composite
// ProgramObject entirely in the frontend - it joins every graphics stage, computes the
// pipeline's draw-program signature, looks it up in the pipeline's own cache and, on a miss,
// attaches each stage's LINKED SNAPSHOT into a fresh ProgramObject and links it. All of that
// is frontend work and none of it moves. What this file adds is the one thing the wire needs:
// the composite gets ONE handle, out of the ShaderCso reserved high band, and
// create_shader_state goes out for it exactly as for an ordinary program. THE SERVER NEVER
// LEARNS IT IS A COMPOSITE and needs no "resolved draw program" hook at all.
//
// WHY A BAND RATHER THAN A FLAG ON THE HANDLE: a flag would have to be carried, honoured and
// masked off by every consumer of a ShaderCso handle, on both sides; a reserved slot range is
// a property of the allocator instead, so "an ordinary program can never be handed a composite
// slot" is true by construction. MGPipeSlotAllocator::Allocate refuses the band outright and
// AllocateComposite is the only door in.
//
// WHAT THIS FILE IS ACTUALLY FOR: the composite's slot has TWO INDEPENDENT RELEASE PATHS and
// either order has to free it exactly once.
//   * the pipeline cache drops the composite when the draw-program signature moves. In the
//     frontend that overwrite drops the last SharedPtr, so the composite's own destructor
//     usually runs first; the resolver still speaks the release, because "usually" is not a
//     contract and a client that only reacted to destructors would leak a slot the moment the
//     frontend started holding a second reference.
//   * the composite ProgramObject's own ~ProgramObject, which is an ordinary program's death
//     path and takes the same helper.
// Both go through MGPipeEmitShaderCsoDestroyAndFree, and whichever runs second is a PROVEN
// no-op: MGPipeSlotAllocator::Free refuses a slot that is not live at that generation and
// bumps no generation of its own, so a double release cannot skip a generation either.
//
// THE MEMO's KEY IS (CONTEXT ID, PIPELINE GL NAME) AND THE CONTEXT HALF IS NOT OPTIONAL.
// This resolver is a PROCESS singleton while a pipeline's GL name is per context: GLContext
// owns m_programPipelines AND its own name generator m_programPipelineNames (Core.h), so name
// N names two different ProgramPipelineObjects in two contexts, each with its own composite
// and its own handle. Keyed on the name alone, the first emission after a make-current found
// the OTHER context's entry, matched nothing - two composites are two ProgramObjects with two
// lifetime ids, so the handles differ even when the stage set and the signature are identical
// - and released it: a delete_shader_state and a cleared publication latch for a composite
// whose frontend ProgramObject is alive, its band slot handed back and re-issued at gen + 1,
// and the server rebuilding that program (glslang + SPIR-V + spirv-opt, the very cost the
// signature below exists to avoid) once per context switch.
//
// THE CONTEXT ID IS GLContext::GetTextureContextId() AND NOTHING ELSE - the tree's existing
// never-reused per-context id (TextureState::AllocateContextId; PipeInputs carries it as
// m_textureContextId at seven fill points and the backends' own per-context memos key on it).
// Deliberately NOT the GLContext ADDRESS that MGB_CTX_IDENTITY and MGPipeTracker::m_context
// compare, because Core.h states the reason that id exists at all: a context freed and remade
// lands on the old heap address, which would put this same defect back one context recreation
// later.
//
// WHAT RELEASES A DESTROYED CONTEXT's ENTRIES: nothing in this file, and that is the correct
// answer rather than an omission. Destroying a context drops m_programPipelines, which drops
// each ProgramPipelineObject, which drops the composite it cached; ~ProgramObject then runs
// MGPipeEmitShaderCsoDestroyAndFree - the composite's OWN release path, the second of the two
// above - and the slot goes back exactly once. The entries those composites leave behind can
// never be found again (no future Observe can carry a dead context id) and could not release
// anything if they were (the allocator erases the lifetime-id mapping on Free), so Reset()
// DROPS them instead of releasing them. That is also what bounds the vector; see Reset().
//
// THE SIGNATURE IS ComputeDrawProgramSignature(), the per-graphics-stage {lifetimeId,
// GetLinkVersion()} array - and DELIBERATELY NOT GetBackendStateVersion(), which is what made
// the SSO conformance loop rebuild the composite (glslang + SPIR-V + spirv-opt) on every draw,
// because a glUniform1i to a sampler moves it.
//
// HEADER-ONLY, for the ownership reason Tracker.h states: a new .cpp would need the root
// CMakeLists.txt, which is the contract package's.
//
// IT IS INCLUDED BY ProgramEmit.h AND NOT THE OTHER WAY ROUND, deliberately: the composite is
// a special case of the program family's own emission, so the family header depends on this
// one and this one depends on nothing of the family's. The reverse arrangement would make the
// resolver reachable only from a translation unit that had already decided to use it, i.e.
// dead in the build that matters and live only in the tests.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_State/GLState/ProgramState/ProgramPipelineObject.h>

namespace MobileGL::MG_Pipe {

    // IS THIS PROGRAM A PIPELINE COMPOSITE? A composite is the one ProgramObject in the system
    // constructed with external index 0 (Core.cpp's MakeShared<ProgramObject>(0u)), and that is
    // not an accident of implementation: it is deliberately not a named program, so it must not
    // answer glIsProgram and must not consume a GL name, and glCreateProgram never returns 0.
    //
    // ASKED THIS WAY RATHER THAN CARRIED ON THE OBJECT because a Bool member on ProgramObject
    // would resize the pull build's object and break G1 outright - the phase's admitted-resize
    // set is empty - and a hook in Core.cpp would have to be maintained on a path that already
    // states the invariant in its own comment.
    inline Bool MGPipeProgramIsPipelineComposite(const MG_State::GLState::ProgramObject& program) {
        return program.GetExternalIndex() == 0;
    }

    class MGPipeCompositeResolver {
    public:
        using ProgramObject = MG_State::GLState::ProgramObject;
        using ProgramPipelineObject = MG_State::GLState::ProgramPipelineObject;
        using DrawProgramSignature = ProgramPipelineObject::DrawProgramSignature;

        struct Counters {
            Uint64 Mints = 0;    // signatures this resolver has seen minted
            Uint64 Reuses = 0;   // a signature that had not moved
            Uint64 Releases = 0; // signature-move releases, i.e. the pipeline-cache path
            // Entries dropped by Reset() because the composite's slot was already gone - the
            // shape every entry of a DESTROYED CONTEXT ends in. A dropped entry is not a
            // release: nothing is emitted and nothing is freed, the obligation having been
            // discharged by the composite's own ~ProgramObject.
            Uint64 Sweeps = 0;
        };

        // Told, at every emission, which composite the frontend handed out for which pipeline.
        // Returns the handle the emitter should use, which is always the one already minted off
        // the composite's own lifetime id - the resolver never mints a second identity for an
        // object that has one.
        //
        // WHEN THE SIGNATURE MOVES the previous composite's slot is released here, through the
        // one death helper and in its fixed order. That is the pipeline-cache release path; the
        // composite's own destructor is the other one and the second of the two is the proven
        // no-op.
        MGPipeHandle Observe(Uint64 contextId, const ProgramPipelineObject& pipeline,
                             const ProgramObject& composite, MGPipeHandle handle) {
            const DrawProgramSignature signature = pipeline.ComputeDrawProgramSignature();
            const Uint pipelineName = pipeline.GetExternalIndex();
            Entry* entry = Find(contextId, pipelineName);
            if (entry != nullptr) {
                if (entry->Signature == signature && entry->Handle == handle) {
                    // THE SAME COMPOSITE. Not merely "the same signature": the handle is minted
                    // off the composite ProgramObject's own lifetime id, so an identical handle
                    // IS an identical object and there is nothing to release. Live is
                    // deliberately NOT touched - it is the release obligation and it is still
                    // owed for exactly this handle.
                    ++m_counters.Reuses;
                    return handle;
                }
                // A MOVED SIGNATURE ON THIS CONTEXT's OWN ENTRY, which is the only thing that
                // can reach here now: another context's pipeline of the same name is not found
                // above and therefore not released, its obligation staying owed to the context
                // that took it.
                ReleaseEntry(*entry);
            } else {
                m_entries.push_back(Entry{});
                entry = &m_entries.back();
                entry->ContextId = contextId;
                entry->PipelineName = pipelineName;
            }
            entry->Signature = signature;
            entry->Handle = handle;
            entry->CompositeLifetimeId = composite.GetLifetimeId();
            entry->Live = true;
            ++m_counters.Mints;
            return handle;
        }

        // A make-current, and it RELEASES NOTHING. The entries name composites that belong to
        // the frontend objects of the context being left, those objects outlive the switch, and
        // releasing them would emit a delete for a live program.
        //
        // NOR IS ANY MEMO INVALIDATED, and that is what the context key bought. This used to
        // clear a per-entry `Fresh` flag beside `Live`, because with a name-only key an entry
        // could not say whether it described "my own pipeline before the switch" or "another
        // context's pipeline of the same name" - and exactly one of those two properties could
        // hold at a time. The key answers the question directly now, so the freshness flag and
        // its one reader (a HandleFor() accessor that had no caller anywhere in the tree) are
        // both gone rather than left as scaffolding: `Live`, the release obligation, is the
        // entry's only state and nothing but ReleaseEntry may clear it.
        //
        // WHAT IS LEFT TO DO HERE IS RECLAMATION, and this is the one moment the client is told
        // that a context boundary was crossed. An entry whose composite slot is no longer live
        // has had its obligation discharged elsewhere - by that composite's own ~ProgramObject,
        // which is precisely what happened to EVERY entry of a context that has just been
        // destroyed - so it is DROPPED rather than released: a release would resolve nothing
        // anyway (the allocator erases the lifetime-id mapping on Free) and no reader is left.
        // Without this the vector would grow by one per (context, pipeline name) pair the
        // process ever used, where the name-only key bounded it by the highest pipeline name;
        // with it, it is bounded by the pairs whose composite slot is actually live.
        void Reset() {
            SizeT kept = 0;
            for (SizeT i = 0; i < m_entries.size(); ++i) {
                if (!m_entries[i].Live || !MGPipeSlots().IsLive(MGPipeKind::ShaderCso, m_entries[i].Handle)) {
                    ++m_counters.Sweeps;
                    continue;
                }
                if (kept != i) m_entries[kept] = m_entries[i];
                ++kept;
            }
            m_entries.resize(kept);
        }

        void ResetCounters() { m_counters = Counters{}; }

        // Diagnostics and unit cases only; nothing on the emission path asks. There is no
        // HandleFor(name) accessor and there must not be one: the emitter takes the handle from
        // the composite ProgramObject it already holds, so a lookup by name would be a second
        // authority on an identity the allocator already owns.
        SizeT Size() const { return m_entries.size(); }
        const Counters& GetCounters() const { return m_counters; }

    private:
        struct Entry {
            // NO FRONTEND SharedPtr, and that is the exit-order rule rather than a style
            // choice: a static that held one would put a frontend destructor on an exit
            // handler's path into a torn-down pipe. A GL name, a signature of plain integers,
            // a handle and a lifetime id are all this needs.
            // KEYED ON (CONTEXT ID, GL NAME), and the name half is the GL name because a
            // ProgramPipelineObject has no lifetime id - ComputeDrawProgramSignature reads the
            // STAGE programs' ids and the pipeline itself carries none. The context half is
            // GLContext::GetTextureContextId(); see the file header for why the name alone was
            // wrong and why the context ADDRESS would be too.
            //
            // WITHIN ONE CONTEXT glGenProgramPipelines recycles names, so a deleted-and-
            // recreated pipeline can still inherit its predecessor's entry; that is bounded and
            // self-correcting rather than a hazard. The first Observe on the new object finds a
            // signature and a handle that do not match and releases the old entry, and that
            // release resolves NOTHING - the allocator erases the lifetime-id mapping on Free,
            // so a stale CompositeLifetimeId emits no delete and frees no slot; all it costs is
            // one redundant, idempotent death notice, which is the same shape the composite's
            // own second release path already has.
            Uint64 ContextId = 0;
            Uint PipelineName = 0;
            DrawProgramSignature Signature{};
            MGPipeHandle Handle = kMGPipeNullHandle;
            Uint64 CompositeLifetimeId = 0;
            // THE RELEASE OBLIGATION. Set when this entry takes responsibility for a composite's
            // slot, cleared ONLY by ReleaseEntry when that responsibility is discharged.
            Bool Live = false;
        };

        // BOTH HALVES OF THE KEY, always. An entry of another context is not this pipeline's
        // entry: not found, not matched, not released.
        Entry* Find(Uint64 contextId, Uint pipelineName) {
            for (Entry& entry : m_entries) {
                if (entry.ContextId == contextId && entry.PipelineName == pipelineName) return &entry;
            }
            return nullptr;
        }

        void ReleaseEntry(Entry& entry) {
            if (!entry.Live || entry.CompositeLifetimeId == 0) return;
            entry.Live = false;
            MGPipeEmitShaderCsoDestroyAndFree(entry.CompositeLifetimeId);
            entry.Handle = kMGPipeNullHandle;
            entry.CompositeLifetimeId = 0;
            ++m_counters.Releases;
        }

        Vector<Entry> m_entries;
        Counters m_counters;
    };

    inline MGPipeCompositeResolver& MGPipeCompositeResolverInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason, and named in the phase's risk
        // list beside the other three new client singletons: heap-constructed and intentionally
        // leaked at exit, holding no frontend SharedPtr.
        static MGPipeCompositeResolver* resolver = new MGPipeCompositeResolver();
        return *resolver;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
