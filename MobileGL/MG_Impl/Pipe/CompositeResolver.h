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
// THE KEY IS ComputeDrawProgramSignature(), the per-graphics-stage {lifetimeId, GetLinkVersion()}
// array - and DELIBERATELY NOT GetBackendStateVersion(), which is what made the SSO
// conformance loop rebuild the composite (glslang + SPIR-V + spirv-opt) on every draw, because
// a glUniform1i to a sampler moves it.
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
        MGPipeHandle Observe(const ProgramPipelineObject& pipeline, const ProgramObject& composite,
                             MGPipeHandle handle) {
            const DrawProgramSignature signature = pipeline.ComputeDrawProgramSignature();
            const Uint key = pipeline.GetExternalIndex();
            Entry* entry = Find(key);
            if (entry != nullptr) {
                if (entry->Signature == signature && entry->Handle == handle) {
                    ++m_counters.Reuses;
                    return handle;
                }
                ReleaseEntry(*entry);
            } else {
                m_entries.push_back(Entry{});
                entry = &m_entries.back();
                entry->PipelineName = key;
            }
            entry->Signature = signature;
            entry->Handle = handle;
            entry->CompositeLifetimeId = composite.GetLifetimeId();
            entry->Live = true;
            ++m_counters.Mints;
            return handle;
        }

        // The pipeline object itself is going away, or a test is tearing down. Speaks the same
        // release for whatever it still holds.
        void Forget(Uint pipelineName) {
            for (SizeT i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].PipelineName != pipelineName) continue;
                ReleaseEntry(m_entries[i]);
                m_entries[i] = m_entries.back();
                m_entries.pop_back();
                return;
            }
        }

        // A make-current. The entries name composites that belong to the frontend objects of
        // the context being left, and those objects outlive the switch, so the RECORDS are not
        // released here - releasing them would emit a delete for a live program. Only the
        // memo's freshness is dropped, which costs at most one re-observation.
        void Reset() {
            for (Entry& entry : m_entries) entry.Live = false;
        }

        void ResetCounters() { m_counters = Counters{}; }

        MGPipeHandle HandleFor(Uint pipelineName) const {
            for (const Entry& entry : m_entries) {
                if (entry.PipelineName == pipelineName && entry.Live) return entry.Handle;
            }
            return kMGPipeNullHandle;
        }
        SizeT Size() const { return m_entries.size(); }
        const Counters& GetCounters() const { return m_counters; }

    private:
        struct Entry {
            // NO FRONTEND SharedPtr, and that is the exit-order rule rather than a style
            // choice: a static that held one would put a frontend destructor on an exit
            // handler's path into a torn-down pipe. A GL name, a signature of plain integers,
            // a handle and a lifetime id are all this needs.
            Uint PipelineName = 0;
            DrawProgramSignature Signature{};
            MGPipeHandle Handle = kMGPipeNullHandle;
            Uint64 CompositeLifetimeId = 0;
            Bool Live = false;
        };

        Entry* Find(Uint pipelineName) {
            for (Entry& entry : m_entries) {
                if (entry.PipelineName == pipelineName) return &entry;
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
