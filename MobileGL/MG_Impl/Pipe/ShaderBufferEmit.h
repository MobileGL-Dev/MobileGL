// MobileGL - MobileGL/MG_Impl/Pipe/ShaderBufferEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of set_shader_buffers - the INDEXED BUFFER BINDING POINTS (P5e package sb,
// MG_Remote/CONTRACT-P5E.md §5.6, rulings 10 and 11). The call has been in the catalogue since
// P4a with no emitter, no route and no applier; this file is the emitter half.
//
// ONE RECORD PER CLASS, THREE CLASSES. MGPShaderBuffers::Class names which binding-point array
// the record describes - Uniform=0, ShaderStorage=1, AtomicCounter=2 - so the three are three
// emissions through one route and one applier, not one record with three tails. That is also
// why the suppressor has THREE SLOTS (ruling 11): a single slot would let a uniform set cancel
// the previous storage set, and the shader-storage bindings would be suppressed as "unchanged"
// by a record that described something else entirely.
//
// XFB IS NOT ONE OF THEM (§5.7). The transform-feedback capture points are span-scoped state
// latched at glBeginTransformFeedback, and set_stream_output_targets carries a Generation this
// payload has no field for - so the catalogue's split between the two rows stays, XFB keeps its
// lockstep escalation, and dirty bit 17 fires for a family that emits nothing this phase.
//
// TWO INVARIANTS THAT MUST SURVIVE INTO THE BODY:
//   1. THE HIGH-WATER-ZERO EARLY-OUT. A class whose touched high-water mark is 0 emits nothing,
//      BEFORE any hash and before any walk. Minecraft's touched SSBO and atomic-counter counts
//      are both 0, so the whole family costs one integer read per class per validate point on
//      the workload this phase exists for.
//   2. A BASE BINDING TRAVELS AS kMGPipeWholeBuffer AND NEVER AS A RESOLVED EXTENT (§5.6,
//      table 0). glBindBufferBase does not freeze anything: GL resolves the range against the
//      object's size at every USE, and BindingSlotRange1D::GetRange() reproduces that by asking
//      the bound object. Resolving it HERE would freeze the size as of the emission, and under
//      run-ahead a glBufferData issued between the emission and the apply would then bind the
//      OLD extent - silently, with the right handle. So Offset/Size are copied only for an
//      EXPLICIT range (glBindBufferRange), and a base binding says "whole buffer" and lets the
//      server re-resolve against its own descriptor.
//
// THE WINDOW IS THE TOUCHED HIGH-WATER MARK (ruling 10), the same value the backend's own walk
// has used since P2 and the same value MGPContextValues already carries. A program-derived
// window would be narrower for the uniform class - Minecraft reaches UBOs through the program's
// block bindings, so a program binding block 0 at GL point 83 pays a 84-entry record - but that
// is a NARROWING and this package is a correctness landing; BRIEF-P5E §5 lists it as trailing.
//
// HEADER-ONLY, and the wired constant below is what switches the family on, for the ownership
// reason PipeFill.cpp states in full: that file belongs to the contract package for the whole
// phase, so the bit that turns an emitter on is a constant in the emitter's OWN header.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/ResourceTracker.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Impl/Pipe/Tracker.h> // MGPipeMixShutter, the one shutter-mixing function
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeTypes.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeRoute.h>
#include <MG_State/GLState/BufferState/BufferState.h>
#include <MG_State/GLState/Core.h>

#include <xxhash.h>

namespace MobileGL::MG_Pipe {

    // WHICH SUBSYSTEM BIT THIS BUILD ACTUALLY EMITS FOR. PipeFill.cpp ORs the per-family
    // constants into kMGPipeWiredSubsystems, so the bit is added by the commit that gives this
    // emitter its body and no file is touched twice.
    //
    // IT MUST MOVE WITH MG_Backend/Init.cpp's CONSUMER BIT (ID-106). c0e deliberately withheld
    // bit 13 from ConsumedSubsystemsFor because no emitter existed and withholding is the safe
    // direction; the client's R-8 liveness gate (PipeFill.cpp's P5eFamilyIsLive) asks the server
    // for THIS bit in particular, so a build that set the constant here and left the server's
    // mask alone would emit nothing at all and look exactly like a build that had not landed.
    //
    // TURNING IT ON DOES NOT RETIRE A PULL. GetBufferBindingPoint is the family's Coverage.def
    // emitted row and PipeFill.cpp's EmittedCallSuppliesTheWholeField answers FALSE for it: the
    // field is four raw bases into the frontend's binding-point table and the record carries
    // resolved {handle, offset, size} triples, so the residual fill keeps writing the mirror and
    // the verify comparator keeps proving it. What retires the pull is the four Espryt consumers
    // reading the applier's window instead, which is the other half of this package.
    inline constexpr Uint64 kMGPipeWiredBufferBindingSubsystem = kMGPipeSubsystemBufferBindings;

    // The BufferTarget each wire class names. One table, because the class is a wire value and
    // the target is a frontend enum, and a switch written at each of the four call sites is four
    // chances to pair Uniform with ShaderStorage.
    inline constexpr BufferTarget MGPipeBufferTargetForShaderBufferClass(Uint32 cls) {
        switch (cls) {
        case kMGPipeShaderBufferClassShaderStorage: return BufferTarget::ShaderStorage;
        case kMGPipeShaderBufferClassAtomicCounter: return BufferTarget::AtomicCounter;
        default: return BufferTarget::Uniform;
        }
    }

    // And the suppressor slot, for ruling 11's reason - one per class.
    inline constexpr MGPipeSuppressorSlot MGPipeSuppressorSlotForShaderBufferClass(Uint32 cls) {
        switch (cls) {
        case kMGPipeShaderBufferClassShaderStorage:
            return MGPipeSuppressorSlot::SetShaderBuffersShaderStorage;
        case kMGPipeShaderBufferClassAtomicCounter:
            return MGPipeSuppressorSlot::SetShaderBuffersAtomicCounter;
        default: return MGPipeSuppressorSlot::SetShaderBuffersUniform;
        }
    }

    // D-G3's shape, the same one the three unit sets use: XXH64 over the tail with the header's
    // own discriminators mixed in. Class is mixed BECAUSE the three classes share nothing but
    // this function - two classes whose windows happen to hold the same ranges must not hash the
    // same, or the per-class A/B tallies would be reading each other's traffic - and the mask
    // words are mixed because they are a FIELD of the record that the entries alone do not
    // determine (a uniform range and a storage range with the same handle differ only there).
    inline Uint64 MGPipeShaderBufferSetContentHash(const MGPBufferRange* entries, Uint32 cls,
                                                   Uint32 start, Uint32 count,
                                                   const Uint32* writableMask) {
        Uint64 hash = XXH64(entries, static_cast<SizeT>(count) * sizeof(MGPBufferRange), 0);
        hash = MGPipeMixShutter(hash, cls);
        hash = MGPipeMixShutter(hash, start);
        hash = MGPipeMixShutter(hash, count);
        for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
            hash = MGPipeMixShutter(hash, writableMask[w]);
        }
        return hash;
    }

    class MGPipeShaderBufferEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // Dirty bit 15 (NEW_CONST_BUFFERS) -> the uniform binding points.
        Uint64 EmitConstBuffers(GLContext& ctx) {
            return EmitClass(ctx, kMGPipeShaderBufferClassUniform);
        }

        // Dirty bit 16 (NEW_SHADER_BUFFERS) -> the two WRITABLE classes, which share a bit
        // because they share a shutter: a storage bind and a counter bind are both "the
        // shader's writable binding points moved", and a workload that touches one without the
        // other pays one suppressed record rather than a second dirty bit.
        Uint64 EmitShaderBuffers(GLContext& ctx) {
            return EmitClass(ctx, kMGPipeShaderBufferClassShaderStorage) +
                   EmitClass(ctx, kMGPipeShaderBufferClassAtomicCounter);
        }

        // The validate point's FreshlyPrimed arm. A fresh context is a fresh set of binding
        // points AND a fresh applier window (MGPipeApplierReset clears all three), so the
        // emitter's mirrors go with them. The suppressor slots are invalidated beside this call
        // by InvalidateAll(); without that the first emission after a make-current would be
        // suppressed as unchanged and the server would draw against a cleared window.
        void Reset() {
            for (Uint32 cls = 0; cls < kMGPipeShaderBufferClassCount; ++cls) {
                m_last[cls] = MGPShaderBuffers{};
            }
        }

        void ResetCounters() {
            for (Uint32 cls = 0; cls < kMGPipeShaderBufferClassCount; ++cls) m_emissions[cls] = 0;
        }

        // What a unit case reads. The emitter hands m_entries straight to the route, so "what
        // was emitted" costs no copy at all - VertexInputEmit.h's G6/G7 property.
        const MGPShaderBuffers& LastHeader(Uint32 cls) const {
            return m_last[cls < kMGPipeShaderBufferClassCount ? cls : 0];
        }
        const Array<MGPBufferRange, kMGPipeMaxBufferBindingPoints>& LastRanges() const {
            return m_entries;
        }
        Uint64 EmissionCount(Uint32 cls) const {
            return m_emissions[cls < kMGPipeShaderBufferClassCount ? cls : 0];
        }

    private:
        Uint64 EmitClass(GLContext& ctx, Uint32 cls) {
            const BufferTarget target = MGPipeBufferTargetForShaderBufferClass(cls);
            // INVARIANT 1, and it is one integer read on every draw of every application that
            // never binds an indexed buffer of this class.
            const SizeT touched = ctx.GetTouchedBufferBindingPointCount(target);
            if (touched == 0) return 0;
            const Uint32 count = touched < kMGPipeMaxBufferBindingPoints
                                     ? static_cast<Uint32>(touched)
                                     : kMGPipeMaxBufferBindingPoints;
            // NO CLIENT-SIDE CLAMP TO THE DEVICE's LIMIT (ruling 10). MobileGL advertises the
            // GL 4.5 minimum of 84 uniform binding points while ES 3.2's is 72, and which of
            // them the driver can actually hold is a SERVER question: the backend clamps to
            // GL_MAX_UNIFORM_BUFFER_BINDINGS exactly as it does today, because a client reading
            // a device capability would be answering it from the wrong side of the wire.

            // The writable mask is rebuilt from scratch for every emission: it is a property of
            // the window as it stands now, not an accumulation, and a sticky one would keep
            // claiming a point that has since been unbound.
            Uint32 writableMask[kMGPipeShaderBufferWritableMaskWords] = {};
            const Bool classIsWritable = cls != kMGPipeShaderBufferClassUniform;

            for (Uint32 i = 0; i < count; ++i) {
                const auto& point = ctx.GetBufferBindingPoint(target, i);
                const auto& object = point.GetBoundObject();
                MGPBufferRange& entry = m_entries[i];
                entry = MGPBufferRange{};
                if (!object) {
                    // A null handle is "nothing bound at that point", which is what the server
                    // binds 0 for. Spelled by clearing the entry rather than by shortening the
                    // window: the window is the touched high-water mark and a hole in the
                    // middle of it is ordinary.
                    entry.Res = kMGPipeNullHandle;
                    continue;
                }
                entry.Res = MGPipeSlots().Acquire(MGPipeKind::Buffer, object->GetLifetimeId());
                // D-A3's sticky bind mask, ORed HERE for the reason VertexInputEmit.h ORs it at
                // every draw: a buffer given its storage through glNamedBufferData and only ever
                // bound with glBindBufferBase is bound to NOTHING at its resource emission, so
                // the mask sampled there would never carry its UNIFORM / SHADER_STORAGE /
                // ATOMIC_COUNTER bit. This is the resolution site, and it is sticky, so one
                // validate point is enough for the rest of the buffer's life.
                MGPipeResourceTrackerInstance().NoteBoundAs(entry.Res, target);
                // INVARIANT 2. HasExplicitRange() is exactly "this point was bound with
                // glBindBufferRange": BindingSlotRange1D keeps the flag precisely so GetRange()
                // can re-resolve a BASE binding against the object every time it is asked, and
                // that re-resolution is what must happen on the SERVER rather than here.
                if (point.HasExplicitRange()) {
                    const auto range = point.GetRange();
                    entry.Offset = static_cast<Uint64>(range.start);
                    entry.Size = static_cast<Uint64>(range.end - range.start);
                } else {
                    entry.Offset = 0;
                    entry.Size = kMGPipeWholeBuffer;
                }
                // WHAT REPLACES THE BACKEND's GPU-WRITE WALK. Every bound storage-buffer and
                // atomic-counter point is marked writable, which is exactly the set
                // MarkShaderStorageBuffersGpuWritten and SyncAtomicCounterBuffers used to walk
                // the frontend for - conservative in the same direction and for the same reason
                // (GpuWritePending.h: an over-approximate set costs a readback, an under-
                // approximate one reads a stale shadow and says nothing). The uniform class
                // never sets a bit: a UBO is read-only to the shader by definition.
                if (classIsWritable) MGPipeShaderBufferMaskSet(writableMask, i);
            }

            const Uint64 hash =
                MGPipeShaderBufferSetContentHash(m_entries.data(), cls, 0, count, writableMask);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(
                    MGPipeSuppressorSlotForShaderBufferClass(cls), hash)) {
                return 0;
            }
            MGPShaderBuffers& header = m_last[cls];
            header = MGPShaderBuffers{};
            header.Class = cls;
            header.Start = 0;
            header.Count = count;
            for (Uint32 w = 0; w < kMGPipeShaderBufferWritableMaskWords; ++w) {
                header.WritableMask[w] = writableMask[w];
            }
            // HostSpanCount IS 0 ALWAYS on Espryt, and that is a ruling rather than an omission:
            // the second var-tail exists for kCapNeedsHostUboBytes (Magma's named-UBO ring) and
            // that bit is 0 for the whole of P5 (CONTRACT-P5.md table 0). The codec's host-span
            // honesty pass is the guard that says so out loud if a backend ever publishes it.
            header.HostSpanCount = 0;
            header.ContentHash = hash;
            MGPipeRouteSetShaderBuffers(header, m_entries.data());
            ++m_emissions[cls];
            return sizeof(MGPShaderBuffers) + static_cast<Uint64>(count) * sizeof(MGPBufferRange);
        }

        // ONE entry buffer for all three classes, not three. A class is built and routed before
        // the next one is built (EmitShaderBuffers above calls EmitClass twice in sequence and
        // the route copies or stages the tail synchronously in both arms), so three would be
        // three times 84 * 24 bytes of resident staging for no reader.
        Array<MGPBufferRange, kMGPipeMaxBufferBindingPoints> m_entries{};
        MGPShaderBuffers m_last[kMGPipeShaderBufferClassCount]{};
        Uint64 m_emissions[kMGPipeShaderBufferClassCount] = {0, 0, 0};
    };

    inline MGPipeShaderBufferEmitter& MGPipeShaderBufferEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason; heap-constructed and
        // intentionally leaked at exit, like every other MGPipe process singleton.
        static MGPipeShaderBufferEmitter* emitter = new MGPipeShaderBufferEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
