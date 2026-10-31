// MobileGL - MobileGL/MG_Impl/Pipe/CompositeResolver.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// Client side of the program-pipeline composite. GLContext::GetProgramForDraw() builds the
// composite in the frontend, and the program emitter gives it one ShaderCso handle from the
// allocator's reserved band (AllocateComposite), so the server never learns it is a composite.
// This resolver remembers each (context, pipeline)'s composite and releases the old slot when
// the draw-program signature moves; the composite's ~ProgramObject is the other release path,
// both go through MGPipeEmitShaderCsoDestroyAndFree, and the later one is a no-op. Entries are
// keyed on (GLContext::GetTextureContextId(), pipeline GL name); see ARCHITECTURE.md §5.6.1.
// Included by ProgramEmit.h; it depends on nothing of the program family.
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
        MGPipeHandle Observe(Uint64 contextId, const ProgramPipelineObject& pipeline, const ProgramObject& composite,
                             MGPipeHandle handle) {
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

        // Called at a make-current. It releases nothing, since the entries' composites still
        // belong to live frontend objects, and invalidates nothing, since entries are keyed by
        // context. It only drops entries already released or whose composite slot is no longer
        // live, freed by the composite's ~ProgramObject (every entry of a destroyed context ends
        // this way); releasing those again would resolve nothing. This bounds the vector by the
        // (context, pipeline) pairs whose composite is still live.
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
            // Holds no frontend SharedPtr: the resolver is a never-destroyed static, and a
            // frontend destructor must not run at exit into a torn-down pipe. Keyed on
            // (context id, pipeline GL name) because a ProgramPipelineObject has no lifetime
            // id; see the file header. A name recycled within one context can inherit its
            // predecessor's entry; the first Observe then releases it, a no-op because the old
            // composite's ~ProgramObject already freed the slot and its lifetime-id mapping.
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
