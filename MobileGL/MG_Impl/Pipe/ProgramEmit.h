// MobileGL - MobileGL/MG_Impl/Pipe/ProgramEmit.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The CLIENT side of P4a's program family: create/bind/delete_shader_state,
// set_draw_program, set_dispatch_program and set_global_constants.
//
// WHERE create_shader_state IS EMITTED FROM, and why it is not the tracker's business: the
// tracker's bit-6 shutter reads GetCurrentProgram() and DELIBERATELY NOT GetProgramForDraw(),
// because the tracker must not force a compile just to answer "did the shader move". So the
// tracker keeps its shutter and the EMITTER joins - from the same GetProgramForDraw() /
// GetProgramForDispatch() call the verb is about to make anyway, so no join happens that would
// not have happened. Emitting from the compile pool's terminal continuation is a real
// asynchronous win and is a LATER phase's: in monolith the applier is one function call away,
// so it is unmeasurable here.
//
// WHAT THE SERVER STILL SPECIALISES, so nobody reads create_shader_state as self-contained
// and produces a per-draw rebuild: the draw-FBO clamp masks, the fragColor broadcast count,
// the storage-block binding signature, the atomic-counter set, the live image formats and the
// patch parameters are all inputs a backend program depends on BEYOND the artefacts. This call
// publishes the ARTEFACTS; the server specialises at the verb from the state it holds. The
// clause count does not shrink - its inputs move.
//
// THE ARTEFACTS DO NOT TRAVEL IN MONOLITH. All seven of MGPProgramDesc's blob refs are
// declared with Size 0 and the LinkArtifacts / SpirvArtifacts ride beside the record through
// MGPipeApplyCreateShaderState's companion pointers, so the codec is never called on the hot
// path; the verify build is where it is exercised.
//
// THIS FILE IS CREATED BY THE CONTRACT COMMIT AND FILLED BY THE PACKAGE THAT OWNS IT - see
// FramebufferEmit.h for why, in full.
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/CompositeResolver.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHostSpan.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_Util/Metrics/PipeStats.h>

namespace MobileGL::MG_Pipe {

    // WIRED. create/bind/delete_shader_state, set_draw_program, set_dispatch_program and
    // set_global_constants all have bodies, so this family contributes its bit to
    // kMGPipeWiredSubsystems.
    //
    // AND SINCE c0b THAT CONSTANT REALLY IS PART OF THE EMISSION GATE, so the note that used to
    // say otherwise here was true only against the contract commit: the validate point's
    // `wants()` asks the subsystem mapping, the operator's MOBILEGL_PIPE_PUSH mask, THIS
    // CONSTANT and the dirty bit, and the birth hooks' `FamilyIsLive` asks the same pair one
    // level in. It is also a compile-time contract - while it is non-zero PipeFill.cpp's
    // `if constexpr` seam instantiates the forward to EmitShaderCso below, so a missing entry
    // point is a build error here rather than at the merge. The RUNTIME A/B that switches the
    // family off is still the mask. See SamplerEmit.h's twin note.
    inline constexpr Uint64 kMGPipeWiredProgramSubsystem = kMGPipeSubsystemPrograms;

    // D-H6. ~0u is the BACKENDS' "never uploaded" sentinel for a global-constants version, and
    // ProgramObject::MarkUBOContentDirty skips it on the wrap for exactly that reason. The
    // client must never put it on the wire either: a server that received it would read its own
    // record as "nothing has ever been uploaded here" and re-upload for ever.
    inline constexpr Uint32 kMGPipeGlobalConstantsNeverUploaded = ~Uint32{0};

    inline constexpr Bool MGPipeGlobalConstantsVersionIsEmittable(Uint32 version) {
        return version != kMGPipeGlobalConstantsNeverUploaded;
    }

    // A program's identity for the wire, out of the SNAPSHOT the last link consumed and never
    // out of the live attach list: glAttachShader and glCompileShader take effect only at the
    // NEXT link and neither moves m_linkVersion, so a stage mask built from GetAttachedShaders
    // would describe a program that does not exist yet. GetLinkedShaderStages() is also what
    // indexes GetGeneratedSpirv(), so the two halves of this descriptor are guaranteed to agree
    // by construction rather than by care.
    inline Uint32 MGPipeStageMaskOf(const MG_State::GLState::ProgramObject& program) {
        Uint32 mask = 0;
        for (const ShaderStage stage : program.GetLinkedShaderStages()) {
            if (stage == ShaderStage::Unknown) continue;
            mask |= Uint32{1} << static_cast<Uint32>(stage);
        }
        return mask;
    }

    class MGPipeProgramEmitter {
    public:
        using GLContext = MG_State::GLState::GLContext;
        using ProgramObject = MG_State::GLState::ProgramObject;

        // create_shader_state (re-issued on the SAME handle whenever the link version moves -
        // Gen moves only on slot reuse), then bind_shader_state and set_draw_program /
        // set_dispatch_program. Two program calls because the frontend has two joins and two
        // PipeInputs slots.
        //
        // BOTH JOINS HAPPEN HERE and both are the verb's own: GetProgramForDraw flattens a
        // bound pipeline into its composite and GetProgramForDispatch answers the compute
        // question, and with a plain glUseProgram they are the same object, so the ordinary
        // frame pays one join it was going to pay anyway.
        Uint64 EmitShaderState(GLContext& ctx) {
            Uint64 bytes = 0;
            const auto& drawProgram = ctx.GetProgramForDraw();
            const auto& dispatchProgram = ctx.GetProgramForDispatch();

            const MGPipeHandle drawCso =
                drawProgram ? AcquireShaderCso(*drawProgram, bytes) : kMGPipeNullHandle;
            // THE COMPOSITE'S SECOND RELEASE PATH is spoken here, not in a destructor: when the
            // bound pipeline's draw-program signature moves, the resolver releases the slot the
            // previous composite held. Whichever of the two paths runs second - this one or the
            // composite ProgramObject's own ~ProgramObject - is a proven no-op, because the slot
            // allocator refuses a slot that is not live at that generation.
            if (drawProgram && MGPipeProgramIsPipelineComposite(*drawProgram)) {
                if (const auto& pipeline = ctx.GetBoundProgramPipeline()) {
                    // THE CONTEXT IS PART OF THE RESOLVER's KEY and this is the only place that
                    // supplies it: the resolver is a process singleton and a pipeline's GL name
                    // is per context, so without it a make-current between two contexts holding
                    // one pipeline name released the other context's LIVE composite.
                    // GetTextureContextId() is the tree's never-reused per-context id, the same
                    // one PipeInputs carries and the backends' per-context memos key on.
                    MGPipeCompositeResolverInstance().Observe(ctx.GetTextureContextId(), *pipeline,
                                                             *drawProgram, drawCso);
                }
            }
            const MGPipeHandle dispatchCso =
                dispatchProgram ? (dispatchProgram == drawProgram ? drawCso
                                                                  : AcquireShaderCso(*dispatchProgram, bytes))
                                : kMGPipeNullHandle;

            // THE BOUND CSO IS THE DRAW ONE WHEN THERE IS ONE. bind_shader_state names what
            // glUseProgram selected, and when a program pipeline is bound instead that is the
            // composite; a compute-only pipeline has no draw program at all, and then the
            // dispatch program is the only thing bound. A null handle is legal here and means
            // exactly "nothing bound".
            const MGPipeHandle boundCso = !MGPipeHandleIsNull(drawCso) ? drawCso : dispatchCso;
            if (boundCso != m_boundCso) {
                MGPipeApplyBindShaderState(HandleOnly(boundCso));
                m_boundCso = boundCso;
                ++m_binds;
                bytes += sizeof(MGPHandleOnly);
            }
            if (drawCso != m_drawCso) {
                MGPipeApplySetDrawProgram(HandleOnly(drawCso));
                m_drawCso = drawCso;
                ++m_drawSets;
                bytes += sizeof(MGPHandleOnly);
            }
            if (dispatchCso != m_dispatchCso) {
                MGPipeApplySetDispatchProgram(HandleOnly(dispatchCso));
                m_dispatchCso = dispatchCso;
                ++m_dispatchSets;
                bytes += sizeof(MGPHandleOnly);
            }
            return bytes;
        }

        // set_global_constants: the DEFAULT UNIFORM BLOCK only, keyed (ShaderCso, Version) and
        // at most once per program per frame. Version is GetUBOContentVersion() and must never
        // be ~0u, which is the backends' "never uploaded" sentinel - the wrap skips it.
        //
        // NAMED uniform blocks are NOT this call's: set_shader_buffers(Uniform) is a later
        // phase's and BindCurrentProgramWithResources' named-UBO block is untouched. What
        // travels here is globalUboScratch, the link phase's CPU array, which has no GL name
        // and no BufferObject behind it.
        Uint64 EmitGlobalConstants(GLContext& ctx) {
            const auto& program = ctx.GetProgramForDraw();
            if (!program) return 0;
            const Uint32 version = program->GetUBOContentVersion();
            // THE SENTINEL IS NEVER EMITTED. A server that received ~0u would read its own
            // record as "never uploaded" and re-upload every frame for ever.
            if (!MGPipeGlobalConstantsVersionIsEmittable(version)) return 0;
            const Uint size = program->GetUBOSize();
            if (size == 0) return 0;

            Uint64 bytes = 0;
            const MGPipeHandle cso = AcquireShaderCso(*program, bytes);
            // (ShaderCso, Version) IS the key, so the latch is the key: an unchanged pair means
            // the server already holds these bytes and re-sending them would move the record's
            // serial for nothing.
            if (cso == m_constantsCso && version == m_constantsVersion) return bytes;

            m_lastConstants = MGPGlobalConstants{};
            m_lastConstants.ShaderCso = cso;
            m_lastConstants.Version = version;
            // THE ONE BLOB RULE: Size 0 means "this record does not declare its blob" - which
            // is what a monolith emission is - and the bytes ride beside it as a companion
            // pointer. Offset carries the staging address for diagnostics only; nothing reads
            // it as a length.
            m_lastConstants.Blob.Seg = kMGHostSpanSegNone;
            m_lastConstants.Blob.Offset = reinterpret_cast<Uint64>(program->GetUBOData());
            m_lastConstants.Blob.Size = 0;
            MGPipeApplySetGlobalConstants(m_lastConstants, program->GetUBOData());
            m_constantsCso = cso;
            m_constantsVersion = version;
            ++m_constantSets;
            if (MG_Util::PipeStats::Enabled()) {
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::CsoBlobBytes, size);
            }
            return bytes + sizeof(MGPGlobalConstants) + size;
        }

        // D-H4's re-issue rule, and it is the CreateVertexElements shape one for one: the
        // record goes out again on the SAME handle whenever the link version moves, which is
        // legal because MGPipeHandle::Gen increments only on slot reuse and never on a
        // respecify. A program that relinks is the same GL object and the server's twin table
        // must not be asked to mint a second one.
        MGPipeHandle AcquireShaderCso(const ProgramObject& program, Uint64& payloadBytes) {
            const MGPipeHandle handle = AcquireShaderCsoHandle(program);
            if (MGPipeHandleIsNull(handle)) return handle;
            Latch& latch = LatchFor(handle);

            const Uint32 linkVersion = program.GetLinkVersion();
            if (latch.RecordLive && latch.RecordGen == handle.Gen && latch.LinkVersion == linkVersion) {
                return handle;
            }

            const auto& link = program.GetLinkReflection();
            const auto& spirv = program.GetSpirvReflection();

            m_lastDesc = MGPProgramDesc{};
            m_lastDesc.Cso = handle;
            m_lastDesc.StageMask = MGPipeStageMaskOf(program);
            m_lastDesc.GlobalUboSize = static_cast<Uint32>(program.GetUBOSize());
            m_lastDesc.ReservedNumSamplesOffset = static_cast<Uint32>(spirv.reservedNumSamplesOffset);
            m_lastDesc.SpirvStatus = spirv.spirvStatus ? 1 : 0;
            m_lastDesc.NativeFloat64 = spirv.nativeFloat64 ? 1 : 0;
            m_lastDesc.PointSizeDemoted = spirv.pointSizeDemoted ? 1 : 0;
            m_lastDesc.EnableSpirvValidation = spirv.enableSpirvValidation ? 1 : 0;

            // ONE BLOB REF PER MODULE, IN THE LINKED-SHADER-SNAPSHOT'S ORDER, which is the
            // order GetGeneratedSpirv() is indexed in - so Spirv[i] and StageMask agree because
            // they came out of the same snapshot. Every one of them declares Size 0 (the one
            // Blob rule); Offset carries the module's staging address so a reader can see which
            // slots are occupied without the record pretending to declare a length it does not
            // own.
            //
            // A COUNTED REFUSAL AND NOT AN ASSERTION (D-J3). MOBILEGL_ASSERT compiles out at
            // INFO, which is all three gate builds and every shipped build, so an assert here
            // would leave the truncation below completely silent in exactly the builds that
            // run - which is the idiom D-J3 exists to forbid. generatedSpirv cannot exceed six
            // stages today, so this is a guard against a seventh; truncation is the safe
            // direction and the counter is what makes it visible.
            const SizeT moduleCount = spirv.generatedSpirv.size();
            if (moduleCount > 6) ++m_moduleTruncations;
            for (SizeT i = 0; i < moduleCount && i < 6; ++i) {
                m_lastDesc.Spirv[i].Seg = kMGHostSpanSegNone;
                m_lastDesc.Spirv[i].Offset = reinterpret_cast<Uint64>(spirv.generatedSpirv[i].data());
                m_lastDesc.Spirv[i].Size = 0;
            }
            m_lastDesc.Reflection.Seg = kMGHostSpanSegNone;
            m_lastDesc.Reflection.Offset = reinterpret_cast<Uint64>(&link);
            m_lastDesc.Reflection.Size = 0;

            MGPipeApplyCreateShaderState(m_lastDesc, &link, &spirv);
            // THE CREATE WENT OUT, so the publication latch is taken here and nowhere else
            // (contract-v2 §3.1). MGPipeEmitShaderCsoDestroyAndFree reads it, and without it
            // delete_shader_state can never go out - for an ordinary program or for a
            // composite, both of which take that one helper.
            MGPipeNoteHandlePublished(MGPipeKind::ShaderCso, handle);
            ++m_creates;
            payloadBytes += sizeof(MGPProgramDesc);

            // A RE-ISSUED create_shader_state CLEARS THE APPLIER's DEFAULT UNIFORM BLOCK (wire
            // W6), so the (Cso, Version) latch that suppresses set_global_constants has to go
            // with it or the block is never re-sent. The case the design worries about is a
            // FAILED relink of a bound program - GL keeps the previous executable and its
            // uniforms running - and the general one is any future re-issue trigger that does
            // not happen to move the content version, of which a recycled slot is one.
            // Invalidated rather than re-emitted here, because this function has no business
            // deciding when the constants go out: the next EmitGlobalConstants sees an
            // unlatched key and sends them.
            if (m_constantsCso == handle) {
                m_constantsCso = kMGPipeNullHandle;
                m_constantsVersion = kMGPipeGlobalConstantsNeverUploaded;
            }

            latch.RecordLive = true;
            latch.RecordGen = handle.Gen;
            latch.LinkVersion = linkVersion;
            return handle;
        }

        // ---- THE CONTRACT ENTRY POINT THIS FAMILY OWES (contract-v2 §3.4) ----
        //
        // PipeFill.cpp's MGPipeEmitShaderCsoCreate forwards here through the `if constexpr`
        // seam keyed on kMGPipeWiredProgramSubsystem, so while that constant is non-zero this
        // must exist and be spelled exactly like this. A thin wrapper on purpose:
        // AcquireShaderCso above IS this family's handle rule - identity-addressed per
        // ProgramObject, the composite band entered through the one door, the re-issue on the
        // same handle and the publication - and a second copy of any of it here would be a
        // second authority.
        //
        // THE HOOK HAS ALREADY APPLIED BOTH GATES (the operator's mask and the wired constant),
        // so this body applies none of its own. The byte count is discarded: a birth is not a
        // validate-point emission and has no payload budget to report into.
        void EmitShaderCso(ProgramObject& program) {
            Uint64 bytes = 0;
            AcquireShaderCso(program, bytes);
        }

        // The emitter's OWN record memo - "have I already published a create_shader_state at
        // this slot, for this generation, at this link version".
        //
        // IT IS NOT WHAT THE DEATH PATH ASKS, and that changed at c0b (contract-v2 §3.1/D17):
        // MGPipeEmitShaderCsoDestroyAndFree reads A's publication latch, which is one answer
        // per {kind, slot, gen} that all six death helpers share. This stays because the
        // VERSION-FIRST SKIP needs it - it is the same latch AcquireShaderCso consults before
        // it builds a descriptor - and because a unit case reads it.
        //
        // THE COMPOSITE BAND IS INDEXED SEPARATELY, for the allocator's own reason: the band
        // base is 983040, so a slot-indexed vector would allocate ~983k latches for one program
        // pipeline. Both spaces stay dense against their own high-water mark.
        Bool RecordIsPublished(MGPipeHandle handle) const {
            if (MGPipeHandleIsNull(handle)) return false;
            const Vector<Latch>& table = TableOf(handle);
            const SizeT slot = SlotIndexOf(handle);
            if (slot >= table.size()) return false;
            const Latch& latch = table[slot];
            return latch.RecordLive && latch.RecordGen == handle.Gen;
        }

        // The memo's other half, and the bound-mirror clearing beside it.
        //
        // THE CALLER IS THE CONTRACT's DEATH HELPER (P4a final review C-2): the death path
        // reads the contract's latch for the wire delete and then forwards here, before the
        // slot is freed, so a dead handle no longer reads as published in this memo between
        // the death and the recycle and the three bound mirrors never name a dead program.
        // Gen-keyed, so a late notice for a slot already handed out again clears nothing of
        // the successor's.
        void NoteRecordDestroyed(MGPipeHandle handle) {
            if (MGPipeHandleIsNull(handle)) return;
            Vector<Latch>& table = TableOf(handle);
            const SizeT slot = SlotIndexOf(handle);
            if (slot < table.size() && table[slot].RecordGen == handle.Gen) {
                table[slot] = Latch{};
            }
            if (m_boundCso == handle) m_boundCso = kMGPipeNullHandle;
            if (m_drawCso == handle) m_drawCso = kMGPipeNullHandle;
            if (m_dispatchCso == handle) m_dispatchCso = kMGPipeNullHandle;
            if (m_constantsCso == handle) {
                m_constantsCso = kMGPipeNullHandle;
                m_constantsVersion = kMGPipeGlobalConstantsNeverUploaded;
            }
        }

        // The validate point's FreshlyPrimed arm. MGPipeApplierReset clears DrawProgram,
        // DispatchProgram and BoundShaderCso - all three are per-context WORKING STATE - so
        // the three mirrors here go with them, or the first emission after a make-current
        // would be suppressed as unchanged and the server would draw with the previous
        // context's program bound.
        //
        // The RECORD half stays, and that is the rule rather than an oversight: the applier
        // keeps its shader-CSO records across a make-current because a program lives in a share
        // group, and re-publishing one would move its Serial for nothing. The global-constants
        // key goes with the working state because its record's bytes are per (Cso, Version) and
        // a fresh server has not been told them.
        void Reset() {
            m_boundCso = kMGPipeNullHandle;
            m_drawCso = kMGPipeNullHandle;
            m_dispatchCso = kMGPipeNullHandle;
            m_constantsCso = kMGPipeNullHandle;
            m_constantsVersion = kMGPipeGlobalConstantsNeverUploaded;
            // The composite memo's freshness goes with them - and only its freshness. Its
            // ENTRIES name composites whose frontend objects outlive the context switch, so
            // releasing them here would emit a delete for a live program.
            MGPipeCompositeResolverInstance().Reset();
        }

        void ResetCounters() {
            m_creates = m_binds = m_drawSets = m_dispatchSets = m_constantSets = 0;
            m_moduleTruncations = 0;
        }

        // ---- what a unit case reads ----
        const MGPProgramDesc& LastProgramDesc() const { return m_lastDesc; }
        const MGPGlobalConstants& LastGlobalConstants() const { return m_lastConstants; }
        // THE (Cso, Version) KEY set_global_constants is suppressed against. Exposed so a case
        // can pin that a re-issued create_shader_state invalidates it - the applier clears the
        // block on the re-issue (wire W6), so a latch that survived it would never re-send.
        MGPipeHandle GlobalConstantsCso() const { return m_constantsCso; }
        Uint32 GlobalConstantsVersion() const { return m_constantsVersion; }
        // D-J3's counted refusal: programs whose linked snapshot carried more modules than
        // MGPProgramDesc::Spirv[] can name, and whose tail was therefore dropped.
        Uint64 TruncatedModuleCount() const { return m_moduleTruncations; }
        MGPipeHandle BoundCso() const { return m_boundCso; }
        MGPipeHandle DrawCso() const { return m_drawCso; }
        MGPipeHandle DispatchCso() const { return m_dispatchCso; }
        Uint64 CreateCount() const { return m_creates; }
        Uint64 BindCount() const { return m_binds; }
        Uint64 DrawProgramSetCount() const { return m_drawSets; }
        Uint64 DispatchProgramSetCount() const { return m_dispatchSets; }
        Uint64 GlobalConstantsSetCount() const { return m_constantSets; }

    private:
        // THE ONE PLACE THE BAND CAN ENTER. An ordinary program's slot comes from the ordinary
        // allocator door keyed on its lifetime id. CompositeResolver.h widens this to send a
        // pipeline composite through MGPipeSlotAllocator::AllocateComposite instead, and
        // nothing else about the emission changes - the server never learns a composite is a
        // composite.
        MGPipeHandle AcquireShaderCsoHandle(const ProgramObject& program) {
            const Uint64 lifetimeId = program.GetLifetimeId();
            const MGPipeHandle existing = MGPipeSlots().FindByLifetimeId(MGPipeKind::ShaderCso, lifetimeId);
            if (!MGPipeHandleIsNull(existing)) return existing;
            // A composite is minted off ITS OWN lifetime id, out of the reserved band, and is
            // an ordinary ShaderCso handle in every other respect - the same kind, the same
            // {slot, gen} rules, the same Free, the same death helper. Keying it on its own
            // lifetime id rather than on the pipeline's signature is what makes ~ProgramObject
            // able to release it at all, and it is why two pipelines that happen to have the
            // same signature keep their own composite: sharing one handle between two frontend
            // objects would let the first one's death free a slot the second still names.
            return MGPipeProgramIsPipelineComposite(program)
                       ? MGPipeSlots().AllocateComposite(lifetimeId)
                       : MGPipeSlots().AllocateFor(MGPipeKind::ShaderCso, lifetimeId);
        }

        struct Latch {
            Bool RecordLive = false;
            Uint32 RecordGen = 0;
            Uint32 LinkVersion = 0;
        };

        // TWO TABLES, NOT A WIDER ONE, and it is the allocator's own reason repeated where it
        // bites a second time: the composite band starts at slot 983040, so folding a composite
        // into the ordinary slot-indexed vector would allocate ~983k latches - and grow them
        // again on every future push_back - for a single program pipeline. Both spaces stay
        // dense against their own high-water mark, which is exactly what the allocator does one
        // level down.
        Vector<Latch>& TableOf(MGPipeHandle handle) {
            return MGPipeIsCompositeShaderSlot(handle.Slot) ? m_compositeLatch : m_latch;
        }
        const Vector<Latch>& TableOf(MGPipeHandle handle) const {
            return MGPipeIsCompositeShaderSlot(handle.Slot) ? m_compositeLatch : m_latch;
        }
        static SizeT SlotIndexOf(MGPipeHandle handle) {
            return MGPipeIsCompositeShaderSlot(handle.Slot)
                       ? static_cast<SizeT>(handle.Slot - kMGPipeShaderCsoCompositeSlotBase)
                       : static_cast<SizeT>(handle.Slot);
        }
        Latch& LatchFor(MGPipeHandle handle) {
            Vector<Latch>& table = TableOf(handle);
            const SizeT slot = SlotIndexOf(handle);
            if (slot >= table.size()) table.resize(slot + 1);
            return table[slot];
        }

        static MGPHandleOnly HandleOnly(MGPipeHandle handle) {
            MGPHandleOnly only{};
            only.Handle = handle;
            only.Kind = static_cast<Uint32>(MGPipeKind::ShaderCso);
            return only;
        }

        MGPProgramDesc m_lastDesc{};
        MGPGlobalConstants m_lastConstants{};

        Vector<Latch> m_latch;
        Vector<Latch> m_compositeLatch;
        MGPipeHandle m_boundCso = kMGPipeNullHandle;
        MGPipeHandle m_drawCso = kMGPipeNullHandle;
        MGPipeHandle m_dispatchCso = kMGPipeNullHandle;
        MGPipeHandle m_constantsCso = kMGPipeNullHandle;
        Uint32 m_constantsVersion = kMGPipeGlobalConstantsNeverUploaded;

        Uint64 m_creates = 0;
        Uint64 m_binds = 0;
        Uint64 m_drawSets = 0;
        Uint64 m_dispatchSets = 0;
        Uint64 m_constantSets = 0;
        Uint64 m_moduleTruncations = 0;
    };

    inline MGPipeProgramEmitter& MGPipeProgramEmitterInstance() {
        // NEVER DESTROYED, for MGPipeTrackerInstance()' reason; heap-constructed and
        // intentionally leaked at exit, and it MUST NOT hold a frontend SharedPtr - that is
        // the exit-order rule, stated over every MGPipe process singleton rather than over the
        // ones a destructor reaches today.
        static MGPipeProgramEmitter* emitter = new MGPipeProgramEmitter();
        return *emitter;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
