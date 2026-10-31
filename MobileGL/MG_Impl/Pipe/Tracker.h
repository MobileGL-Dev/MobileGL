// MobileGL - MobileGL/MG_Impl/Pipe/Tracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The frontend state tracker (ARCHITECTURE.md §5.2).
//
// Update() runs at the validate point, MGPipeValidateForVerb, which MGP_FILL expands to before
// every backend call (PipeFill.h), not in the GL setters: a glEnable/glDisable bracket around a
// batch then costs one push at the draws that observe it (ARCHITECTURE.md §5.1). It returns one
// MGPipeDirty bit per shutter that moved since the last push. A shutter may over-fire (one extra
// push) but must never under-fire (silently stale state, ARCHITECTURE.md §5.2.1), so each is
// coarser than the state it guards. Bit 17 is computed and counted but has no emitter.
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeValueTypes.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Metrics/PipeStats.h>

#include <cstring>

namespace MobileGL::MG_Pipe {

    // One bit per row of the ARCHITECTURE.md 5.2 table, hand-written rather than generated:
    // the list is design, not derived data, and the generator has nothing to derive it from.
    enum class MGPipeDirty : Uint32 {
        // ---- value class: P2 emits for these five ----
        NewRenderState = 0,      // RenderState::m_version              -> set_dynamic_state
        NewPipelineState,        // RenderState::m_pipelineStateVersion -> create/bind_render_state
        NewPixelPack,            // PixelStoreParameters (pack)         -> set_pixel_pack_state
        NewPatchState,           // the patch trio, NaN legal           -> set_patch_state
        NewVertexAttribDefaults, // glVertexAttrib* defaults            -> set_vertex_attrib_defaults
        // ---- value class: NEW_VERTEX_ELEMENTS is emitted from P3a and the other three from
        // P4a - the program family, one subsystem, three bits because the frontend moves them
        // as three separate events ----
        NewVertexElements,  // the bound VAO's attribute configuration -> create/bind_vertex_elements
        NewShader,          // the current program's link version -> create/bind_shader_state,
                            //    set_draw_program, set_dispatch_program (P4a)
        NewShaderBindings,  // image units, block bindings, uniform write set (P4a)
        NewGlobalConstants, // the default-uniform-block image -> set_global_constants (P4a)
        // ---- object class. THE FIRST TWO ARE P3a's, not P3b/P4b's: the roadmap puts
        // set_vertex_buffers and set_index_buffer in the same phase as the vertex-elements
        // trio, and this comment said otherwise until the commit that wired them. THE NEXT
        // FOUR ARE P4a's. The last three are still computed and counted only, until P4b. ----
        NewVertexBuffers, // -> set_vertex_buffers (P3a)
        NewIndexBuffer,   // -> set_index_buffer   (P3a)
        NewFramebuffer,   // -> set_framebuffer_state, per bound target (P4a)
        NewSamplerViews,  // -> set_sampler_views   (P4a)
        NewSamplers,      // -> bind_sampler_states (P4a)
        NewShaderImages,  // -> set_shader_images   (P4a)
        NewConstBuffers,
        NewShaderBuffers,
        NewSoTargets,
        Count,
    };

    inline constexpr SizeT kMGPipeDirtyCount = static_cast<SizeT>(MGPipeDirty::Count);
    static_assert(kMGPipeDirtyCount <= 32, "the dirty mask is a Uint32");

    inline constexpr Uint32 MGPipeDirtyBit(MGPipeDirty bit) {
        return Uint32{1} << static_cast<Uint32>(bit);
    }

    // The five P2 emits for. Each phase's constant survives as the next phase's A/B control
    // and as what a test compares the subsystem map against, so none of them is edited in
    // place when a later phase takes more bits over.
    inline constexpr Uint32 kMGPipeDirtyEmittedAtP2 =
        MGPipeDirtyBit(MGPipeDirty::NewRenderState) | MGPipeDirtyBit(MGPipeDirty::NewPipelineState) |
        MGPipeDirtyBit(MGPipeDirty::NewPixelPack) | MGPipeDirtyBit(MGPipeDirty::NewPatchState) |
        MGPipeDirtyBit(MGPipeDirty::NewVertexAttribDefaults);

    // The three P3a adds: the vertex-input family, all on one subsystem.
    inline constexpr Uint32 kMGPipeDirtyEmittedAtP3a =
        kMGPipeDirtyEmittedAtP2 | MGPipeDirtyBit(MGPipeDirty::NewVertexElements) |
        MGPipeDirtyBit(MGPipeDirty::NewVertexBuffers) | MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer);

    // The SEVEN P4a adds, across FOUR subsystems: bits 6/7/8 are the program family, 11 the
    // framebuffer, and 12/13/14 the sampler-view / sampler-state / image-unit sets. Added
    // rather than edited into the two above, for the reason those two exist: each phase's
    // constant survives as the next phase's A/B control and as what a test compares the
    // subsystem map against.
    //
    // EVERY ONE OF THESE SHUTTERS WAS ALREADY COMPUTED, LATCHED AND COUNTED before P4a; what
    // P4a adds is an emitter for them. That is why this is a one-line constant and not seven
    // new shutters - and it is also why the two narrowings below are stated as requirements.
    inline constexpr Uint32 kMGPipeDirtyEmittedAtP4a =
        kMGPipeDirtyEmittedAtP3a | MGPipeDirtyBit(MGPipeDirty::NewShader) |
        MGPipeDirtyBit(MGPipeDirty::NewShaderBindings) | MGPipeDirtyBit(MGPipeDirty::NewGlobalConstants) |
        MGPipeDirtyBit(MGPipeDirty::NewFramebuffer) | MGPipeDirtyBit(MGPipeDirty::NewSamplerViews) |
        MGPipeDirtyBit(MGPipeDirty::NewSamplers) | MGPipeDirtyBit(MGPipeDirty::NewShaderImages);

    // The THREE P5e adds, all one subsystem (kMGPipeSubsystemBufferBindings): the indexed
    // buffer binding points, whose three bits have been computed and counted since P2 and have
    // named no subsystem since. Added rather than edited in, for the reason above.
    //
    // NewSoTargets rides the same bit even though set_stream_output_targets stays UNEMITTED
    // for the whole of P5e (XFB is lockstep by escalation, CONTRACT-P5E.md §5.7): the bit is
    // "which A/B switch owns this family's legacy arm", and an operator clearing bit 13 has to
    // get the whole binding-point family's frontend walk back rather than two thirds of it -
    // the rule P3a's three and P4a's two triples already state.
    inline constexpr Uint32 kMGPipeDirtyEmittedAtP5e =
        kMGPipeDirtyEmittedAtP4a | MGPipeDirtyBit(MGPipeDirty::NewConstBuffers) |
        MGPipeDirtyBit(MGPipeDirty::NewShaderBuffers) | MGPipeDirtyBit(MGPipeDirty::NewSoTargets);

    inline constexpr const char* kMGPipeDirtyNames[kMGPipeDirtyCount] = {
        "NEW_RENDER_STATE",           "NEW_PIPELINE_STATE",  "NEW_PIXEL_PACK",    "NEW_PATCH_STATE",
        "NEW_VERTEX_ATTRIB_DEFAULTS", "NEW_VERTEX_ELEMENTS", "NEW_SHADER",        "NEW_SHADER_BINDINGS",
        "NEW_GLOBAL_CONSTANTS",       "NEW_VERTEX_BUFFERS",  "NEW_INDEX_BUFFER",  "NEW_FRAMEBUFFER",
        "NEW_SAMPLER_VIEWS",          "NEW_SAMPLERS",        "NEW_SHADER_IMAGES", "NEW_CONST_BUFFERS",
        "NEW_SHADER_BUFFERS",         "NEW_SO_TARGETS",
    };

    // Which runtime MOBILEGL_PIPE_PUSH subsystem bit gates a dirty bit's emission. Zero for
    // a bit P2 does not emit, which is what makes "the bitmask is a true per-subsystem A/B"
    // literally true rather than approximately.
    inline constexpr Uint64 MGPipeSubsystemForDirty(MGPipeDirty bit) {
        switch (bit) {
        case MGPipeDirty::NewRenderState:
        case MGPipeDirty::NewPipelineState:
            return kMGPipeSubsystemRenderState;
        case MGPipeDirty::NewPixelPack:
            return kMGPipeSubsystemPixelPack;
        case MGPipeDirty::NewPatchState:
            return kMGPipeSubsystemPatchState;
        case MGPipeDirty::NewVertexAttribDefaults:
            return kMGPipeSubsystemVertexAttribDefaults;
        // P3a's three, all one subsystem: create/bind_vertex_elements, set_vertex_buffers
        // and set_index_buffer are the vertex-input family and an operator switching it off
        // has to get the whole family's legacy arm, not two thirds of it.
        // PipeFill.cpp's SubsystemForEmitter carries the pairing static_asserts.
        case MGPipeDirty::NewVertexElements:
        case MGPipeDirty::NewVertexBuffers:
        case MGPipeDirty::NewIndexBuffer:
            return kMGPipeSubsystemVertexInput;
        // P4a's seven, across four subsystems. FOUR AND NOT ONE for P3a's reason one level
        // out: a framebuffer path that regressed, a texture path that regressed, a sampler
        // path that regressed and a program path that regressed are four different findings.
        //
        // The program family is three bits because the frontend moves them separately - a
        // relink, a binding change and a uniform write are three events - but one subsystem,
        // because an operator switching programs off has to get the whole family's legacy arm.
        // Same for the three unit sets: create_sampler_state, create_sampler_view and the
        // three kVarTail sets are one family, and half of it is not a control.
        case MGPipeDirty::NewShader:
        case MGPipeDirty::NewShaderBindings:
        case MGPipeDirty::NewGlobalConstants:
            return kMGPipeSubsystemPrograms;
        case MGPipeDirty::NewFramebuffer:
            return kMGPipeSubsystemFramebuffer;
        case MGPipeDirty::NewSamplerViews:
        case MGPipeDirty::NewSamplers:
        case MGPipeDirty::NewShaderImages:
            return kMGPipeSubsystemSamplers;
            // P5e's three, one subsystem (§1): the indexed buffer
            // binding points. Bits 15/16/17 have been computed and counted since P2 and have named
            // no subsystem since - "the remaining bits have no call of their own until P4b", which
            // the default arm below used to say for them. set_shader_buffers is the call, and
            // set_stream_output_targets stays unemitted while riding the same A/B bit, for the
            // reason kMGPipeDirtyEmittedAtP5e states.
            //
            // NAMING THE SUBSYSTEM IS NOT THE SAME AS EMITTING. The emission gate is five
            // conjuncts (PipeFill.cpp's `wants()`), one of which is kMGPipeWiredSubsystems - and
            // the buffer-binding family's wired constant is 0 until the package that gives the
            // emitter its body sets it, exactly as P4a's four families were. So this map moves
            // here, inert, and nothing is emitted and no field is skipped on its account yet.
        case MGPipeDirty::NewConstBuffers:
        case MGPipeDirty::NewShaderBuffers:
        case MGPipeDirty::NewSoTargets:
            return kMGPipeSubsystemBufferBindings;
        // NO BIT NAMES kMGPipeSubsystemTextureResources, and that is deliberate rather than an
        // omission: the texture and renderbuffer resource_* calls and set_texture_params are
        // dispatched from the GL entry points that cause them - a constructor, a storage
        // definition, a glTexParameter - not from a dirty walk, exactly as P3a's buffer family
        // is. Bit 10 gates those dispatch sites; there is no dirty bit to map onto it and
        // there must not be one, or the emission would be gated twice and disagree with itself.
        default:
            // Every dirty bit now names a subsystem; the arm stays because the switch is over
            // a value cast from an index and a future bit must not fall off the end.
            return 0;
        }
    }

    // The mix for composite shutters, built from several counters. It is a hash, so two states
    // can collide and miss a fire; over lifetime ids, versions and generations an application
    // cannot steer, that is about 2^-64 per pair, accepted instead of comparing the whole state
    // on every verb. The mix must avalanche: a hash_combine-style step collides outright on small
    // neighbouring counters ((1, 64) and (2, 1)), and a missed bit-12 fire then leaves a draw
    // sampling the previous texture set. Each step is a bijection in either argument with the
    // other fixed, so a change to one input alone always changes the result.
    inline constexpr Uint64 MGPipeShutterAvalanche(Uint64 value) {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebull;
        value ^= value >> 31;
        return value;
    }
    inline constexpr Uint64 MGPipeMixShutter(Uint64 accumulator, Uint64 value) {
        return MGPipeShutterAvalanche(accumulator ^ MGPipeShutterAvalanche(value + 0x9e3779b97f4a7c15ull));
    }

    // A Uint16 counter widened at the TRACKER boundary, never in MG_State
    // (ARCHITECTURE.md 5.2: MG_State is not changed for this). A decrease is a wrap and adds
    // 65536. A wrap is harmless locally - one extra re-push, never a missed one - which is
    // exactly what TrackerTest.WrapAroundRePushesButNeverMisses pins.
    //
    // THE ONE CASE IT CANNOT SEE, stated because "never a missed push" is otherwise stronger
    // than what is true: the wrap test is `now < m_last`, so a counter that advances by
    // EXACTLY 65536 (or a multiple) between two walks reads as unchanged. That needs 65536
    // render-state mutations inside one verb boundary, and it is pre-existing in class -
    // both backends already compare raw Uint16 versions the same way - so P2 records it
    // rather than widening MG_State's counters, which ARCHITECTURE.md 5.2 rules out.
    class MGPipeWidenedCounter {
    public:
        Uint64 Observe(Uint16 now) {
            if (m_started && now < m_last) m_high += 0x10000ull;
            m_started = true;
            m_last = now;
            return m_high + now;
        }
        void Reset() {
            m_high = 0;
            m_last = 0;
            m_started = false;
        }

    private:
        Uint64 m_high = 0;
        Uint16 m_last = 0;
        Bool m_started = false;
    };

    class MGPipeTracker {
    public:
        using GLContext = MG_State::GLState::GLContext;

        // The dirty walk. Compares every shutter against what was last pushed, LATCHES the
        // new values, counts the fires per verb class, and returns the mask. Latching here
        // rather than after emission is deliberate: a bit whose subsystem is switched off is
        // not emitted, but its fields are then still pulled by the residual fill, so the
        // pushed block is correct either way and a bit can never fire twice for one change.
        Uint32 Update(GLContext& ctx, MGPipeVerbClass verbClass) {
            // A different context is a different server: nothing the tracker latched about
            // the old one says anything about this one, and the first walk on a fresh
            // context must publish a COMPLETE state rather than an increment.
            //
            // KEYED ON THE CONTEXT'S NEVER-REUSED ID, NOT ITS ADDRESS: a context destroyed and
            // remade at the same heap address (Qt does this) is a fresh server-side applier
            // with empty windows, and an address compare would skip the re-prime that refills
            // them.
            if (m_context != &ctx || m_contextId != ctx.GetTextureContextId()) {
                Reset();
                m_context = &ctx;
                m_contextId = ctx.GetTextureContextId();
            }
            const Bool wasPrimed = m_primed;

            Uint64 now[kMGPipeDirtyCount];
            const RenderStateParameters& render = ctx.GetRenderStateParameters();

            // ---- bits 0..1: the two Uint16 render-state counters, widened HERE ----
            now[Index(MGPipeDirty::NewRenderState)] =
                m_renderStateVersion.Observe(static_cast<Uint16>(ctx.GetRenderStateParametersVersion()));
            now[Index(MGPipeDirty::NewPipelineState)] =
                m_pipelineStateVersion.Observe(static_cast<Uint16>(ctx.GetPipelineStateVersion()));

            // ---- bit 4 and the value-class bits 5..8 ----
            now[Index(MGPipeDirty::NewVertexAttribDefaults)] = ctx.GetAnyVertexAttribDefaultGeneration();

            const auto& vao = ctx.GetBoundVertexArray();
            const Uint64 vaoLifetime = vao ? vao->GetLifetimeId() : 0;
            const Uint32 vaoConfig = vao ? vao->GetConfigVersion() : 0;
            const Bool vaoChanged = !m_primed || vaoLifetime != m_lastVaoLifetime || vaoConfig != m_lastVaoConfig;
            const Uint64 vaoIdentity = vao ? MGPipeMixShutter(vaoLifetime, vaoConfig) : 0;
            now[Index(MGPipeDirty::NewVertexElements)] = vaoIdentity;

            // Reads plain version counters, never GetProgramForDraw(): that joins a pending
            // link, and the tracker must not force a compile. The source is the program in use
            // or, when there is none (glUseProgram(0) with a separable pipeline bound), the bound
            // pipeline's stage programs' {lifetime id, link version}, the pairs
            // ComputeDrawProgramSignature() packs, so bit 6 fires exactly when the composite
            // changes. The pipeline name is mixed in because two pipelines with the same stages
            // cache separate composites. Known gap: a deleted pipeline name regenerated onto the
            // same stages is not seen, since ProgramPipelineObject has no lifetime id to mix in.
            const auto& program = ctx.GetCurrentProgram();
            Uint64 shader = 0;
            Uint64 bindings = 0;
            Uint64 constants = 0;
            Uint64 programImages = 0;
            // THE PROGRAM INPUT OF THE PROGRAM-RESOLVED VIEW SET (P4a fable seam F-1).
            // set_sampler_views is resolved for the program in use (SamplerEmit.h: the sampler
            // uniform's TYPE picks which of a unit's targets is the view) and the emitter
            // memoises that resolution on (lifetime id, link version, backend state version). A
            // shutter that read only the texture generations therefore missed a glUseProgram:
            // `glBindTexture x N; glUseProgram(P1); draw; glUseProgram(P2); draw` moved nothing
            // bit 12 read, so the view set stayed P1's - and E's record epoch, keyed on the two
            // set serials, then never rebuilt the texture sync list for P2 either. This value is
            // that memo key, and bit 12 mixes it in below: over-firing costs one re-resolution
            // the set-hash suppressor absorbs, under-firing left the record describing the
            // previous program's units.
            Uint64 opaqueUnits = 0;
            if (program) {
                shader = MGPipeMixShutter(program->GetLifetimeId(), program->GetLinkVersion());
                bindings = MGPipeMixShutter(MGPipeMixShutter(MGPipeMixShutter(program->GetImageUnitVersion(),
                                                                              program->GetBackendStateVersion()),
                                                             program->GetBlockBindingVersion()),
                                            program->GetUniformWriteSetVersion());
                constants = MGPipeMixShutter(program->GetLifetimeId(), program->GetUBOContentVersion());
                // THE IDENTITY IS MIXED IN (P4a fable seam F-2), exactly as the pipeline arm
                // below mixes stageLinks into its half: the counter alone is a per-program
                // number two programs routinely share - 0 == 0 for any pair that never moved an
                // image unit through glUniform1i, and 0 == 0 against no program at all - so a
                // glUseProgram between them fired nothing, set_shader_images' window stayed the
                // previous program's, and a program whose only image is a BUFFER image (E's
                // SD-4: nothing else moves between the bind and the dispatch) never reached the
                // record at all.
                programImages = MGPipeMixShutter(shader, program->GetImageUnitVersion());
                opaqueUnits = MGPipeMixShutter(shader, program->GetBackendStateVersion());
            } else if (const auto& pipeline = ctx.GetBoundProgramPipeline(); pipeline) {
                using Pipeline = MG_State::GLState::ProgramPipelineObject;
                // THE FIELDS ARE READ DIRECTLY RATHER THAN THROUGH THE TWO FUNCTIONS THAT
                // ALREADY PACK THEM, and that is a gate constraint, not a preference. Calling
                // ComputeDrawProgramSignature() / ComputeUniformMirrorVersions() would say
                // "the same pairs the composite cache and the uniform-mirror gate compare"
                // far better than this loop does - but gen_pipe_dirty_surface.py derives a
                // shutter by following each accessor to the member it returns, and both of
                // those build a LOCAL array and return that, which it cannot place. A shutter
                // naming them is UNRESOLVED, and then every DirtySurface.def row that names
                // bits 6, 7, 8 or 14 loses its verdict - including the derivation that is the
                // only mechanism able to catch the next under-fire here. So the pairs are
                // spelled out, and the two static_asserts below are what say they must stay in
                // step with the functions they mirror.
                static_assert(sizeof(Pipeline::DrawProgramSignature) ==
                                  2 * Pipeline::kGraphicsStageCount * sizeof(Uint64),
                              "bit 6 reads the {lifetimeId, linkVersion} pair per graphics "
                              "stage that ComputeDrawProgramSignature packs");
                static_assert(sizeof(Pipeline::UniformMirrorVersions) ==
                                  2 * Pipeline::kGraphicsStageCount * sizeof(Uint64),
                              "bits 7 and 8 read the four counters per graphics stage that "
                              "ComputeUniformMirrorVersions packs");

                // Bit 6 is the pipeline identity plus the composite cache key. Bits 7 and 8
                // add the stage programs' own state (glUniform* reaches the active program,
                // glProgramUniform* and the block-binding calls a named one), which reaches
                // the composite only through RefreshCompositeUniforms. Bit 14 reads the image
                // unit version, a separate counter because Espryt bakes image units into the
                // ESSL it generates (see ProgramObject). stageLinks is mixed into all of them:
                // a rebuilt composite is a new ProgramObject with no uniforms or backend state
                // and must not inherit the old one's bindings, constants or image units.
                Uint64 stageLinks = static_cast<Uint64>(ctx.GetBoundProgramPipelineName());
                Uint64 stageState = 0;
                Uint64 stageImages = 0;
                // The per-stage sampler/image unit assignments alone (glUniform1i on a stage
                // program's sampler moves its backend state version and reaches the composite
                // through the uniform mirror), for bit 12's program input below.
                Uint64 stageOpaque = 0;
                for (SizeT stage = 0; stage < Pipeline::kGraphicsStageCount; ++stage) {
                    const auto& staged = pipeline->GetStageProgram(static_cast<ShaderStage>(stage));
                    if (!staged) continue;
                    stageLinks = MGPipeMixShutter(MGPipeMixShutter(stageLinks, staged->GetLifetimeId()),
                                                  staged->GetLinkVersion());
                    stageState = MGPipeMixShutter(
                        MGPipeMixShutter(
                            MGPipeMixShutter(stageState, staged->GetBackendStateVersion()),
                            MGPipeMixShutter(staged->GetUBOContentVersion(), staged->GetBlockBindingVersion())),
                        staged->GetUniformWriteSetVersion());
                    stageImages = MGPipeMixShutter(stageImages, staged->GetImageUnitVersion());
                    stageOpaque = MGPipeMixShutter(stageOpaque, staged->GetBackendStateVersion());
                }
                shader = stageLinks;
                stageState = MGPipeMixShutter(stageLinks, stageState);
                bindings = MGPipeMixShutter(stageState, stageImages);
                constants = stageState;
                programImages = MGPipeMixShutter(stageLinks, stageImages);
                opaqueUnits = MGPipeMixShutter(stageLinks, stageOpaque);
            }
            now[Index(MGPipeDirty::NewShader)] = shader;
            now[Index(MGPipeDirty::NewShaderBindings)] = bindings;
            now[Index(MGPipeDirty::NewGlobalConstants)] = constants;

            // Every field of an emitted record must change only through setters that move an
            // input of the emitting bit's shutter, or be emitted unconditionally at its setter
            // (resource_*, set_texture_params). DirtySurface.def cannot check derived fields,
            // so the record-field -> setter -> shutter table for bits 6-8 and 11-14 is kept by
            // hand in ARCHITECTURE.md §5.2.2; add a row there when a record gains a field.

            // ---- the object-class bits 9..17 ----
            const Uint64 textureContent = ctx.GetAnyTextureContentGeneration();
            const Uint64 textureParams = ctx.GetAnyTextureParamsGeneration();
            // P5e (sb): the buffer CONTENT aggregate is no longer read here at all. It was
            // bits 15/16/17's whole shutter and it answered the wrong question for every one of
            // them (see those bits below); the aggregate itself stays, because
            // MGPipeAggregate::BufferChange is still one of the six the walk reports and a
            // counter with no reader is a different removal from a shutter with a better input.

            // Bit 9. The VAO attribute aggregate mixed with the bound VAO's identity is
            // already exact for the SET - it is bumped by all three Bump*Version functions,
            // which are the only writers of an attribute's format, buffer or enable state -
            // and a driver-id re-mint that moves no client counter is caught server-side by
            // the backend's own id generation.
            //
            // THE PENDING BASE INSTANCE IS MIXED IN, and this is a deviation from the design
            // note that said "keep the shutter" (recorded in client-v1.md): the draw's
            // baseInstance is now an EXPLICIT field of set_vertex_buffers and a
            // ContentHash input, and it moves neither the attribute aggregate nor the VAO
            // identity. Without it here, a draw whose only change is its base instance would
            // never reach the emitter at all and the server would keep the previous fetch
            // shift - which is the same silently-wrong-geometry the backend's
            // baseInstanceDirty flag exists to prevent, one level further out. It fires
            // extra only on the draws that actually carry one.
            now[Index(MGPipeDirty::NewVertexBuffers)] = MGPipeMixShutter(
                MGPipeMixShutter(ctx.GetAnyVaoAttributeGeneration(), vaoIdentity), m_pendingBaseInstance);
            // Bit 10, NARROWED (P3a, D-I). It used to mix the whole buffer-CONTENT aggregate
            // with the VAO identity and therefore fired on any buffer write anywhere; what
            // it guards is one binding slot, so it now reads that slot's own version and the
            // identity of what is bound to it. The version is a WRAPPING Uint16 bumped only
            // on a real change, so it goes through the widened counter at this boundary; the
            // bound object's lifetime id joins it because identity is what closes the wrap
            // hole. The VAO identity stays in the mix because the element slot BELONGS to
            // the bound VAO - switching VAOs switches slots.
            Uint64 indexShutter = 0;
            if (vao) {
                const auto& indexSlot = vao->GetIndexBufferBindingSlot();
                const auto& indexObject = indexSlot.GetBoundObject();
                indexShutter = MGPipeMixShutter(m_indexSlotVersion.Observe(indexSlot.GetVersion()),
                                                indexObject ? indexObject->GetLifetimeId() : 0);
            }
            now[Index(MGPipeDirty::NewIndexBuffer)] = MGPipeMixShutter(vaoIdentity, indexShutter);
            // Bit 11 reads the draw and the read binding-slot versions, since the record is
            // emitted per bound target. It inlines each attachment's format, target, extent,
            // samples and completeness, so a storage redefinition of an attached object must fire
            // it too: TextureObjectBase's and RenderbufferObject's PipePublishDescriptor bump the
            // attachment aggregate (an unattached object over-fires once). The texture-content
            // aggregate stays out; it would rebuild the record on every glTexSubImage*. Do not
            // narrow this onto m_attachmentVersions: FramebufferObject::SetDrawBuffer bumps the
            // version of the value written, not of the index.
            now[Index(MGPipeDirty::NewFramebuffer)] = MGPipeMixShutter(
                MGPipeMixShutter(
                    ctx.GetAnyFramebufferAttachmentGeneration(),
                    m_framebufferBind.Observe(ctx.GetFramebufferBindingSlot(FramebufferTarget::Draw).GetVersion())),
                m_readFramebufferBind.Observe(ctx.GetFramebufferBindingSlot(FramebufferTarget::Read).GetVersion()));
            // Bit 12 reads FOUR things (F-1): the two texture aggregates, the bind generation
            // and the program input computed above. The params aggregate is here because
            // SamplerEmit.h drops a unit's view to null when SamplesAsIncompleteTexture says so,
            // and that predicate reads the effective sampler's filters - a glTexParameteri(
            // MIN_FILTER) that completes a texture fired bit 13 and not this one, so the entry
            // stayed null. The program input is here because the set is resolved FOR THE
            // PROGRAM IN USE, and a glUseProgram alone moved nothing this shutter read.
            now[Index(MGPipeDirty::NewSamplerViews)] = MGPipeMixShutter(
                MGPipeMixShutter(MGPipeMixShutter(textureContent, textureParams), ctx.GetTextureBindGeneration()),
                opaqueUnits);
            // Bit 13 mixes in the texture bind generation because glBindSampler moves only that
            // one (through NoteTextureUnitTouched and TextureUnit::SetSamplerObject); the
            // sampling-resolution generation moves only on parameter changes. A texture bind can
            // change the applied sampler too, when the unit has no sampler object. Mixing it in,
            // rather than gating the emitter on bit 12, keeps the per-bit fire tallies meaningful;
            // the extra fires are absorbed by the emitter's set-hash suppressor. A missed fire
            // here means wrong filtering, since bind_sampler_states has no pulled fallback.
            now[Index(MGPipeDirty::NewSamplers)] = MGPipeMixShutter(
                MGPipeMixShutter(textureParams, ctx.GetSamplingResolutionGeneration()), ctx.GetTextureBindGeneration());
            now[Index(MGPipeDirty::NewShaderImages)] =
                MGPipeMixShutter(MGPipeMixShutter(MGPipeMixShutter(textureContent, textureParams), programImages),
                                 ctx.GetTextureBindGeneration());
            // Bits 15-17 read the buffer bind-point generations that glBindBufferBase/Range move
            // (BufferState::NoteBindPointChanged), not the buffer content aggregate: the records
            // carry {handle, offset, size}, a base binding's size travels as kMGPipeWholeBuffer
            // and is resolved by the server at use, and content is the resource records' job.
            // Bit 15 mixes in the program identity (`shader`) because the uniform window is
            // indexed by the program's block bindings. Bit 16 covers storage and atomic-counter
            // points, which are emitted together. Bit 17 adds the transform-feedback generation
            // (capture points latch at Begin) and has no emitter.
            now[Index(MGPipeDirty::NewConstBuffers)] =
                MGPipeMixShutter(ctx.GetBufferBindPointGeneration(BufferTarget::Uniform), shader);
            now[Index(MGPipeDirty::NewShaderBuffers)] =
                MGPipeMixShutter(ctx.GetBufferBindPointGeneration(BufferTarget::ShaderStorage),
                                 ctx.GetBufferBindPointGeneration(BufferTarget::AtomicCounter));
            now[Index(MGPipeDirty::NewSoTargets)] =
                MGPipeMixShutter(ctx.GetBufferBindPointGeneration(BufferTarget::TransformFeedback),
                                 ctx.GetTransformFeedbackGeneration());

            Uint32 dirty = 0;
            for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
                // Bits 2 and 3 are handled below: they are BitwiseEqual shutters, not
                // counters, so they have no entry in `now`.
                if (i == Index(MGPipeDirty::NewPixelPack) || i == Index(MGPipeDirty::NewPatchState)) {
                    continue;
                }
                if (!m_primed || now[i] != m_lastPushed[i]) dirty |= Uint32{1} << static_cast<Uint32>(i);
                m_lastPushed[i] = now[i];
            }

            // The lifetime/configuration pair is an identity, not a content hash.
            // hash_combine collides readily for nearby integer pairs; missing a
            // VAO bind can pair the old layout with the new VAO's buffer window.
            // Revalidate all three vertex families on an exact pair change. Their
            // own emitters still suppress unchanged records.
            if (vaoChanged) {
                dirty |= MGPipeDirtyBit(MGPipeDirty::NewVertexElements) |
                         MGPipeDirtyBit(MGPipeDirty::NewVertexBuffers) | MGPipeDirtyBit(MGPipeDirty::NewIndexBuffer);
            }
            m_lastVaoLifetime = vaoLifetime;
            m_lastVaoConfig = vaoConfig;
            // A redundant bind of the default texture can grow the unit window
            // without changing either binding or sampling generations. Both sets
            // must still publish that new prefix, including its null samplers.
            const Int maxTextureUnit = ctx.GetMaxTouchedTextureUnit();
            if (!m_primed || maxTextureUnit != m_lastMaxTextureUnit) {
                dirty |= MGPipeDirtyBit(MGPipeDirty::NewSamplerViews) | MGPipeDirtyBit(MGPipeDirty::NewSamplers);
            }
            m_lastMaxTextureUnit = maxTextureUnit;

            // ---- bits 11..14 across contexts: the share clocks (C8) ----
            // The aggregates bits 11-14 read are this context's, and a texture, renderbuffer or
            // sampler changed through a SIBLING context bumps only the sibling's - so with nothing
            // done here A's walk after B's glTexParameteri / glTexImage2D / glRenderbufferStorage
            // fired none of the four and A recorded the stale framebuffer, view and sampler sets.
            // The group keeps a twin of each aggregate, and each bit fires here on exactly the
            // twins of what its own shutter reads above. An exact compare rather than one more
            // MGPipeMixShutter for vaoChanged's reason: the four are small neighbouring counters.
            // A single-context group skips the clocks behind one relaxed load; the first walk
            // after a second context joins may fire the four once, which is the safe direction.
            // The program bits need nothing of this: a program is a group object too, but its
            // versions are read off the object itself, which every context sees move.
            const auto& group = *ctx.GetShareGroup();
            if (group.HasSeveralContexts()) {
                using SharedClock = MG_State::GLState::ShareGroupState::SharedClock;
                const SharedClocks clocks{group.ReadSharedClock(SharedClock::FramebufferAttachment),
                                          group.ReadSharedClock(SharedClock::TextureContent),
                                          group.ReadSharedClock(SharedClock::TextureParams),
                                          group.ReadSharedClock(SharedClock::SamplingResolution)};
                const Bool content = clocks.TextureContent != m_lastSharedClocks.TextureContent;
                const Bool params = clocks.TextureParams != m_lastSharedClocks.TextureParams;
                if (clocks.FramebufferAttachment != m_lastSharedClocks.FramebufferAttachment) {
                    dirty |= MGPipeDirtyBit(MGPipeDirty::NewFramebuffer);
                }
                if (content || params) {
                    dirty |=
                        MGPipeDirtyBit(MGPipeDirty::NewSamplerViews) | MGPipeDirtyBit(MGPipeDirty::NewShaderImages);
                }
                if (params || clocks.SamplingResolution != m_lastSharedClocks.SamplingResolution) {
                    dirty |= MGPipeDirtyBit(MGPipeDirty::NewSamplers);
                }
                m_lastSharedClocks = clocks;
            }

            // ---- bit 2: the PACK half of the pixel store, BitwiseEqual ----
            const PixelStoreParameters pack = ctx.GetPixelStoreParameters(false);
            if (!m_primed || std::memcmp(&pack, &m_pack, sizeof(pack)) != 0) {
                dirty |= MGPipeDirtyBit(MGPipeDirty::NewPixelPack);
                m_pack = pack;
            }

            // ---- bit 3: the patch trio, BitwiseEqual, and NaN IS LEGAL ----
            // A NaN outer level is a legal glPatchParameterfv value and must compare equal to
            // itself (ARCHITECTURE.md 5.2). Float equality says it is not; memcmp says it is,
            // which is the whole reason this is a byte compare.
            PatchTrio patch{};
            patch.PatchVertices = render.PatchVertices;
            for (SizeT i = 0; i < 4; ++i)
                patch.Outer[i] = render.PatchDefaultOuterLevel[i];
            for (SizeT i = 0; i < 2; ++i)
                patch.Inner[i] = render.PatchDefaultInnerLevel[i];
            if (!m_primed || std::memcmp(&patch, &m_patch, sizeof(patch)) != 0) {
                dirty |= MGPipeDirtyBit(MGPipeDirty::NewPatchState);
                m_patch = patch;
            }

            m_primed = true;
            m_freshlyPrimed = !wasPrimed;
            m_lastDirty = dirty;

            if (MG_Util::PipeStats::Enabled()) {
                const SizeT cls = static_cast<SizeT>(verbClass);
                ++m_walks[cls];
                for (SizeT i = 0; i < kMGPipeDirtyCount; ++i) {
                    if (dirty & (Uint32{1} << static_cast<Uint32>(i))) ++m_fires[i][cls];
                }
            }
            return dirty;
        }

        // Context teardown, server reset, a unit test's fixture. The next Update returns
        // every bit set, which is what makes the first verb on a fresh context publish a
        // complete state rather than an increment. Deliberately does NOT clear the fire
        // tallies: they are a per-run measurement, not per-context state.
        //
        // AND IT DELIBERATELY DOES NOT CLEAR m_pendingBaseInstance. Everything else this
        // function clears is a LATCH describing what the server was last told; the pending
        // base instance is THIS CALL'S ARGUMENT, written by the draw entry point one
        // statement before MGP_FILL and not yet read by anybody. Update() calls Reset() from
        // inside itself whenever the current GLContext pointer moves, so clearing it here
        // meant that `eglMakeCurrent(ctxB); glDrawArraysInstancedBaseInstance(..., 7)` put a
        // BaseInstance of 0 on the wire - one silently mis-shifted instanced draw per context
        // switch, on the emulation path, with nothing to catch it. The value is cleared by the
        // verb that consumes it (PipeFill.cpp's step 3, and its no-context early return) and
        // by MGPipeLeaveVerb, which is where a per-call argument belongs.
        void Reset() {
            std::memset(m_lastPushed, 0, sizeof(m_lastPushed));
            m_lastVaoLifetime = 0;
            m_lastVaoConfig = 0;
            m_lastMaxTextureUnit = -1;
            m_lastSharedClocks = SharedClocks{};
            m_renderStateVersion.Reset();
            m_pipelineStateVersion.Reset();
            m_framebufferBind.Reset();
            m_readFramebufferBind.Reset();
            m_indexSlotVersion.Reset();
            m_pack = PixelStoreParameters{};
            m_patch = PatchTrio{};
            m_staged = RenderStateParameters{};
            m_stagedAttribs = AttribDefaults{};
            m_context = nullptr;
            m_contextId = 0;
            m_lastDirty = 0;
            m_primed = false;
            m_freshlyPrimed = false;
        }

        void ResetCounters() {
            std::memset(m_fires, 0, sizeof(m_fires));
            std::memset(m_walks, 0, sizeof(m_walks));
        }

        Uint64 FireCount(MGPipeDirty bit, MGPipeVerbClass verbClass) const {
            return m_fires[Index(bit)][static_cast<SizeT>(verbClass)];
        }
        Uint64 FireCount(MGPipeDirty bit) const {
            Uint64 total = 0;
            for (SizeT i = 0; i < kMGPipeVerbClassCount; ++i)
                total += m_fires[Index(bit)][i];
            return total;
        }
        Uint64 WalkCount(MGPipeVerbClass verbClass) const { return m_walks[static_cast<SizeT>(verbClass)]; }
        Uint64 WalkCount() const {
            Uint64 total = 0;
            for (SizeT i = 0; i < kMGPipeVerbClassCount; ++i)
                total += m_walks[i];
            return total;
        }

        Uint32 LastDirty() const { return m_lastDirty; }
        Bool Primed() const { return m_primed; }
        // True when the LAST Update was the first one after a Reset - a fresh context, or a
        // server reset. The emission step reads it to send a COMPLETE state rather than an
        // increment against a staging mirror that describes a context that is gone.
        Bool FreshlyPrimed() const { return m_freshlyPrimed; }

        // "What the server has" (P2 brief D8). set_dynamic_state sends the dynamic chunks
        // that differ from this, which is the chunk-level suppressor; a chunk that
        // memcmp-matches is not sent at all.
        RenderStateParameters& Staged() { return m_staged; }
        const RenderStateParameters& Staged() const { return m_staged; }

        // The same mirror for the 32 glVertexAttrib* defaults: set_vertex_attrib_defaults
        // names only the attributes that differ from it, which is the var-tail's own
        // suppressor underneath D11's set-hash one.
        using AttribDefaults = Array<MG_State::GLState::CurrentVertexAttributeValue,
                                     MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS>;
        AttribDefaults& StagedAttribDefaults() { return m_stagedAttribs; }
        const AttribDefaults& StagedAttribDefaults() const { return m_stagedAttribs; }

        // The draw's vertex-fetch base instance, held here because bit 9's shutter mixes it in:
        // it is a field of set_vertex_buffers and an input to its content hash. The three
        // *BaseInstance draw entry points set it just before the fill. In production only the
        // validate point clears it, on both exits of PipeFill.cpp (after emission and on the
        // no-live-context early return); MGPipeLeaveVerb also clears it, but only tests call
        // that. Reset() must not clear it: it is the current call's argument, not a latch.
        void SetPendingBaseInstance(Uint32 baseInstance) { m_pendingBaseInstance = baseInstance; }
        Uint32 PendingBaseInstance() const { return m_pendingBaseInstance; }
        void ClearPendingBaseInstance() { m_pendingBaseInstance = 0; }

    private:
        static constexpr SizeT Index(MGPipeDirty bit) { return static_cast<SizeT>(bit); }

        struct PatchTrio {
            Uint PatchVertices;
            Float Outer[4];
            Float Inner[2];
        };
        // The share group's four clocks as of this context's last walk (C8).
        struct SharedClocks {
            Uint64 FramebufferAttachment;
            Uint64 TextureContent;
            Uint64 TextureParams;
            Uint64 SamplingResolution;
        };

        Uint64 m_lastPushed[kMGPipeDirtyCount]{};
        Uint64 m_lastVaoLifetime = 0;
        Uint32 m_lastVaoConfig = 0;
        Int m_lastMaxTextureUnit = -1;
        SharedClocks m_lastSharedClocks{};
        MGPipeWidenedCounter m_renderStateVersion;
        MGPipeWidenedCounter m_pipelineStateVersion;
        // The draw framebuffer BINDING slot version, widened for the same reason: a Uint16
        // that wrapped would let a composite shutter repeat and cost a missed fire.
        MGPipeWidenedCounter m_framebufferBind;
        // P4a: the READ framebuffer binding slot's version, its own counter for the same
        // reason the draw one exists. Two counters rather than one over both slots: a single
        // widened counter fed two independent Uint16s reads a decrease as a wrap on every
        // alternation and would add 65536 per switch, which costs nothing in correctness
        // (over-firing) but makes the high word meaningless.
        MGPipeWidenedCounter m_readFramebufferBind;
        // The BOUND VAO's element-array slot version, widened for the same reason. One
        // counter over a slot that changes with the bound VAO: a stale high word can only
        // ADD a fire, never drop one, and the VAO identity in the same mix is what makes a
        // switch between two VAOs differ whatever their slot versions read.
        MGPipeWidenedCounter m_indexSlotVersion;
        Uint32 m_pendingBaseInstance = 0;
        // Bits 2 and 3 are BitwiseEqual shutters, not counters.
        PixelStoreParameters m_pack{};
        PatchTrio m_patch{};

        RenderStateParameters m_staged{};
        AttribDefaults m_stagedAttribs{};

        const void* m_context = nullptr;
        Uint64 m_contextId = 0;
        Uint32 m_lastDirty = 0;
        Bool m_primed = false;
        Bool m_freshlyPrimed = false;

        Uint64 m_fires[kMGPipeDirtyCount][kMGPipeVerbClassCount]{};
        Uint64 m_walks[kMGPipeVerbClassCount]{};
    };

    // ONE attribute default, flattened onto the wire (P2 brief D10, AMENDED at P5c rv). A
    // named function rather than four lines inside the emitter because this flattening is the
    // whole correctness question of set_vertex_attrib_defaults: a CurrentVertexAttributeValue
    // is one value in three views and GLContext converts NUMERICALLY between them, so four
    // words alone are not the value - glVertexAttrib4f(loc, 1.5f, ...) leaves 1 in intValue
    // and 0x3FC00000 in floatValue. Since rv the record carries ALL THREE VIEWS VERBATIM
    // (MGPAttribValue::FloatView/IntView/UintView, CONTRACT-P5C.md §5.3) and the applier writes
    // each view from its own array; ValueClass is the record of which view the application
    // wrote directly, kept for the comparator and for readers - the applier no longer needs
    // it to rebuild anything.
    inline void MGPipeFillAttribValue(Uint32 location, const MG_State::GLState::CurrentVertexAttributeValue& value,
                                      Uint32 writtenClass, MGPAttribValue& out) {
        out = MGPAttribValue{};
        out.Location = location;
        out.ValueClass = static_cast<Uint8>(writtenClass);
        static_assert(sizeof(out.FloatView) == sizeof(value.floatValue), "MGPAttribValue's views are four words each");
        static_assert(sizeof(out.IntView) == sizeof(value.intValue) && sizeof(out.UintView) == sizeof(value.uintValue),
                      "MGPAttribValue's views are four words each");
        std::memcpy(out.FloatView, value.floatValue.data(), sizeof(out.FloatView));
        std::memcpy(out.IntView, value.intValue.data(), sizeof(out.IntView));
        std::memcpy(out.UintView, value.uintValue.data(), sizeof(out.UintView));
    }

    // The monolith's one tracker. Under split there is one per client context; the context
    // identity check inside Update is what makes the single instance safe today.
    inline MGPipeTracker& MGPipeTrackerInstance() {
        // NEVER DESTROYED, for MGPipeSlots()' reason (MG_Impl/Pipe/SlotAllocator.cpp). The
        // rule is stated over the SET of MGPipe process singletons rather than over the two
        // that a frontend destructor reaches today: which of them a destructor reaches is a
        // property of the emitters, and the emitters change (C-1 added a second reaching
        // path in one commit). One allocation per process, no destructor to lose - this type
        // has none - and nothing can then answer a late call out of freed storage.
        static MGPipeTracker* tracker = new MGPipeTracker();
        return *tracker;
    }
} // namespace MobileGL::MG_Pipe
