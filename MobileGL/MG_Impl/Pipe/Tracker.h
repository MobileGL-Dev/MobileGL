// MobileGL - MobileGL/MG_Impl/Pipe/Tracker.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// The frontend state tracker (ARCHITECTURE.md 5.2, P2 brief D4).
//
// WHERE IT RUNS. Not above MGP_FILL and not in the GL setter: MGPipeValidateForVerb, the
// one statement MGP_FILL already expands to before every gBackendFunctionsTable.GL call
// (PipeFill.h). Blaze3D brackets every batch with glEnable/glDisable(GL_BLEND), so a
// setter that pushed would push twice per batch for a state the batch may not even read;
// the validate point coalesces the whole bracket into the two draws that observe it
// (ARCHITECTURE.md 5.1).
//
// WHAT IT DOES. One Uint32 dirty mask per verb, one bit per row of ARCHITECTURE.md 5.2,
// computed by comparing a shutter against what the tracker last pushed. P2 emitted for bits
// 0..4 (the value-class ones); P3a adds bits 5, 9 and 10 - the vertex-input family - and P4a
// adds SEVEN: 6, 7 and 8 (the program family), 11 (the framebuffer) and 12, 13 and 14 (the
// three unit sets). Only bits 15, 16 and 17 - the const-buffer, shader-buffer and
// stream-output sets - are still computed, latched and counted without an emitter, so the
// per-bit fire rate is a measurement rather than a plan and their fields go through the
// residual fill until P4b.
//
// P4a NARROWS NOTHING AND WIDENS ONE THING: bit 11's shutter gains the READ framebuffer
// binding slot's version, because set_framebuffer_state is emitted per bound TARGET and a
// glBindFramebuffer(GL_READ_FRAMEBUFFER, ...) moved no shutter at all before. Over-firing is
// free; that was an under-fire.
//
// WHY EVERY SHUTTER OVER-FIRES. A bit that fires too often costs one extra push. A bit
// that fires too rarely renders stale, and ARCHITECTURE.md 13.2 names that as the
// dangerous direction precisely because the P1 verify comparator cannot see it for
// object-class state (it compares those by identity only). So each shutter below is
// deliberately coarser than the state it guards - five bits share one buffer aggregate,
// the framebuffer bit fires on any attachment write anywhere - and the narrowing is P3's
// work, paid for with the fire rates this file publishes.
//
// NO TIMER LIVES HERE. ROADMAP.md forbids committing hot-path instrumentation; the
// absolute ns/draw comes from DriverBench, which times whole frames from outside the
// library (P2 brief D17). The only counting is the per-bit fire tally, behind
// PipeStats::Enabled() like every other counting site in the tree.
//
// HEADER-ONLY, and that is an ownership decision rather than a design one: the P2 brief
// asks for Tracker.{h,cpp}, but the root CMakeLists.txt that would have to name a new .cpp
// belongs to package A and is frozen behind the p2/contract tag. Everything here is
// included by exactly one translation unit in the library (MG_Impl/Pipe/PipeFill.cpp) plus
// the unit tests, so inline costs nothing. Splitting it back out is one list(APPEND) line.
#if MOBILEGL_PIPE_PUSH
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
        NewVertexElements,       // the bound VAO's attribute configuration -> create/bind_vertex_elements
        NewShader,               // the current program's link version -> create/bind_shader_state,
                                 //    set_draw_program, set_dispatch_program (P4a)
        NewShaderBindings,       // image units, block bindings, uniform write set (P4a)
        NewGlobalConstants,      // the default-uniform-block image -> set_global_constants (P4a)
        // ---- object class. THE FIRST TWO ARE P3a's, not P3b/P4b's: the roadmap puts
        // set_vertex_buffers and set_index_buffer in the same phase as the vertex-elements
        // trio, and this comment said otherwise until the commit that wired them. THE NEXT
        // FOUR ARE P4a's. The last three are still computed and counted only, until P4b. ----
        NewVertexBuffers,        // -> set_vertex_buffers (P3a)
        NewIndexBuffer,          // -> set_index_buffer   (P3a)
        NewFramebuffer,          // -> set_framebuffer_state, per bound target (P4a)
        NewSamplerViews,         // -> set_sampler_views   (P4a)
        NewSamplers,             // -> bind_sampler_states (P4a)
        NewShaderImages,         // -> set_shader_images   (P4a)
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
        MGPipeDirtyBit(MGPipeDirty::NewShaderBindings) |
        MGPipeDirtyBit(MGPipeDirty::NewGlobalConstants) |
        MGPipeDirtyBit(MGPipeDirty::NewFramebuffer) | MGPipeDirtyBit(MGPipeDirty::NewSamplerViews) |
        MGPipeDirtyBit(MGPipeDirty::NewSamplers) | MGPipeDirtyBit(MGPipeDirty::NewShaderImages);

    inline constexpr const char* kMGPipeDirtyNames[kMGPipeDirtyCount] = {
        "NEW_RENDER_STATE",
        "NEW_PIPELINE_STATE",
        "NEW_PIXEL_PACK",
        "NEW_PATCH_STATE",
        "NEW_VERTEX_ATTRIB_DEFAULTS",
        "NEW_VERTEX_ELEMENTS",
        "NEW_SHADER",
        "NEW_SHADER_BINDINGS",
        "NEW_GLOBAL_CONSTANTS",
        "NEW_VERTEX_BUFFERS",
        "NEW_INDEX_BUFFER",
        "NEW_FRAMEBUFFER",
        "NEW_SAMPLER_VIEWS",
        "NEW_SAMPLERS",
        "NEW_SHADER_IMAGES",
        "NEW_CONST_BUFFERS",
        "NEW_SHADER_BUFFERS",
        "NEW_SO_TARGETS",
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
        // NO BIT NAMES kMGPipeSubsystemTextureResources, and that is deliberate rather than an
        // omission: the texture and renderbuffer resource_* calls and set_texture_params are
        // dispatched from the GL entry points that cause them - a constructor, a storage
        // definition, a glTexParameter - not from a dirty walk, exactly as P3a's buffer family
        // is. Bit 10 gates those dispatch sites; there is no dirty bit to map onto it and
        // there must not be one, or the emission would be gated twice and disagree with itself.
        default:
            // The remaining bits have no call of their own until P4b, so there is no
            // subsystem to switch and the residual fill keeps supplying their fields.
            return 0;
        }
    }

    // A COMPOSITE shutter, for the bits whose "did anything move" is more than one counter.
    // It is a hash, so two different states can in principle collide and cost a MISSED fire.
    // The five bits P2 emits for are never composed - they are widened counters and byte
    // compares, neither of which can collide.
    //
    // P3a's three ARE composed, so the risk is now real rather than academic, and it is
    // accepted with its size stated: each mix takes a 64-bit input into a 64-bit
    // accumulator, so two DIFFERENT vertex configurations collide with probability ~2^-64
    // per pair, and the inputs are a monotone lifetime id, a monotone configuration version
    // and a widened slot version - none of which an application can steer. The alternative,
    // comparing the whole 32-attribute configuration byte for byte on every verb, is the
    // per-draw cost the shutter exists to avoid. The narrowing that removes the composition
    // for bit 10 - its own slot version plus the bound object's identity - is what this
    // phase already did to the one shutter that was composed over an unrelated aggregate.
    inline constexpr Uint64 MGPipeMixShutter(Uint64 accumulator, Uint64 value) {
        accumulator ^= value + 0x9e3779b97f4a7c15ull + (accumulator << 6) + (accumulator >> 2);
        return accumulator;
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
            if (m_context != &ctx) {
                Reset();
                m_context = &ctx;
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
            const Uint64 vaoIdentity =
                vao ? MGPipeMixShutter(vao->GetLifetimeId(), vao->GetConfigVersion()) : 0;
            now[Index(MGPipeDirty::NewVertexElements)] = vaoIdentity;

            // Deliberately NOT GetProgramForDraw: that joins a pending link, and the tracker
            // must not force a compile just to answer "did the shader move". These version
            // counters are plain members and are exactly what the backends already read
            // without joining (Core.cpp, the glUseProgram half of join site J1).
            const auto& program = ctx.GetCurrentProgram();
            Uint64 shader = 0;
            Uint64 bindings = 0;
            Uint64 constants = 0;
            Uint64 programImages = 0;
            if (program) {
                shader = MGPipeMixShutter(program->GetLifetimeId(), program->GetLinkVersion());
                bindings = MGPipeMixShutter(
                    MGPipeMixShutter(MGPipeMixShutter(program->GetImageUnitVersion(),
                                                      program->GetBackendStateVersion()),
                                     program->GetBlockBindingVersion()),
                    program->GetUniformWriteSetVersion());
                constants = MGPipeMixShutter(program->GetLifetimeId(), program->GetUBOContentVersion());
                programImages = program->GetImageUnitVersion();
            }
            now[Index(MGPipeDirty::NewShader)] = shader;
            now[Index(MGPipeDirty::NewShaderBindings)] = bindings;
            now[Index(MGPipeDirty::NewGlobalConstants)] = constants;

            // ---- the object-class bits 9..17 ----
            const Uint64 textureContent = ctx.GetAnyTextureContentGeneration();
            const Uint64 textureParams = ctx.GetAnyTextureParamsGeneration();
            const Uint64 buffers = ctx.GetAnyBufferChangeGeneration();

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
            // Bit 11, WIDENED AT P4a AND THIS IS A REQUIREMENT RATHER THAN AN OPTION. The
            // shutter observed the DRAW binding slot only, so glBindFramebuffer(
            // GL_READ_FRAMEBUFFER, ...) moved nothing at all - which was harmless while
            // nothing was emitted for the bit and is an UNDER-FIRE the moment P4a emits
            // set_framebuffer_state per bound target (D-C2): the read record would never be
            // sent and the server's ReadSurface would stay the previous framebuffer's. Over-
            // firing costs one extra push; under-firing renders stale, and this file's own
            // rule is that under-firing is the dangerous direction.
            //
            // A RENDERBUFFER RESPECIFY IS STILL INVISIBLE HERE, and deliberately so:
            // RenderbufferObject's SetInternalFormat / AllocateStorage / SetSamples bump no
            // version and raise no notice, so re-storaging an ALREADY-ATTACHED renderbuffer
            // moves neither half of this shutter. That hole is closed by emitting
            // resource_respecify straight from the storage entry point - not by widening this
            // shutter and not by adding a version counter to RenderbufferObject, which would
            // resize the pull build's object and break G1.
            //
            // AND A TRAP THE NEXT NARROWING WOULD WALK INTO, recorded here because it is
            // invisible from the shutter: FramebufferObject::SetDrawBuffer versions the VALUE
            // being written rather than the index being written TO - it calls
            // BumpAttachmentVersion(buffer). The object version and the aggregate still move,
            // so THIS shutter is safe; a narrower one built on m_attachmentVersions would not
            // be, and P4a must not build one.
            now[Index(MGPipeDirty::NewFramebuffer)] = MGPipeMixShutter(
                MGPipeMixShutter(
                    ctx.GetAnyFramebufferAttachmentGeneration(),
                    m_framebufferBind.Observe(
                        ctx.GetFramebufferBindingSlot(FramebufferTarget::Draw).GetVersion())),
                m_readFramebufferBind.Observe(
                    ctx.GetFramebufferBindingSlot(FramebufferTarget::Read).GetVersion()));
            now[Index(MGPipeDirty::NewSamplerViews)] =
                MGPipeMixShutter(textureContent, ctx.GetTextureBindGeneration());
            now[Index(MGPipeDirty::NewSamplers)] =
                MGPipeMixShutter(textureParams, ctx.GetSamplingResolutionGeneration());
            now[Index(MGPipeDirty::NewShaderImages)] =
                MGPipeMixShutter(MGPipeMixShutter(textureContent, textureParams), programImages);
            now[Index(MGPipeDirty::NewConstBuffers)] = buffers;
            now[Index(MGPipeDirty::NewShaderBuffers)] = buffers;
            now[Index(MGPipeDirty::NewSoTargets)] =
                MGPipeMixShutter(buffers, ctx.GetTransformFeedbackGeneration());

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
            for (SizeT i = 0; i < 4; ++i) patch.Outer[i] = render.PatchDefaultOuterLevel[i];
            for (SizeT i = 0; i < 2; ++i) patch.Inner[i] = render.PatchDefaultInnerLevel[i];
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
            for (SizeT i = 0; i < kMGPipeVerbClassCount; ++i) total += m_fires[Index(bit)][i];
            return total;
        }
        Uint64 WalkCount(MGPipeVerbClass verbClass) const {
            return m_walks[static_cast<SizeT>(verbClass)];
        }
        Uint64 WalkCount() const {
            Uint64 total = 0;
            for (SizeT i = 0; i < kMGPipeVerbClassCount; ++i) total += m_walks[i];
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

        // ---- P3a D-H2: the draw's vertex-FETCH base instance ----
        //
        // It lives HERE rather than in a file static because bit 9's shutter has to see it:
        // an ambient process global cannot cross a pushed boundary, and the value is now an
        // explicit field of set_vertex_buffers and an input to its content hash, so a draw
        // whose only change is its base instance has to reach the emitter. Set immediately
        // before the fill at the three *BaseInstance draw entry points; CONSUMED and cleared
        // by the validate point once it has been emitted, so a plain draw that follows one
        // sees 0 again.
        //
        // THE CLEAR THAT ACTUALLY RUNS IN PRODUCTION IS THE VALIDATE POINT'S. MGPipeLeaveVerb
        // clears it too, but no GL entry point calls MGPipeLeaveVerb - only MG_Test's
        // ScopedPipeVerb and TrackerTest do - so the production guarantee is entirely
        // PipeFill.cpp's, on BOTH of its exits: the end of step 3, and the no-live-context
        // early return that skips step 3 altogether. Reset() deliberately does not clear it
        // (see there): it is this call's argument, not a latch.
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

        Uint64 m_lastPushed[kMGPipeDirtyCount]{};
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
        Uint32 m_lastDirty = 0;
        Bool m_primed = false;
        Bool m_freshlyPrimed = false;

        Uint64 m_fires[kMGPipeDirtyCount][kMGPipeVerbClassCount]{};
        Uint64 m_walks[kMGPipeVerbClassCount]{};
    };

    // ONE attribute default, flattened onto the wire (P2 brief D10). A named function rather
    // than four lines inside the emitter because this flattening is the whole correctness
    // question of set_vertex_attrib_defaults: a CurrentVertexAttributeValue is one value in
    // three views and GLContext converts NUMERICALLY between them, so four words alone are
    // not the value - glVertexAttrib4f(loc, 1.5f, ...) leaves 1 in intValue and 0x3FC00000 in
    // floatValue. MGPAttribValue::ValueClass is what makes the four words readable again, and
    // TrackerAttribPayload pins that here instead of leaving it to the emitter's shape.
    inline void MGPipeFillAttribValue(Uint32 location,
                                      const MG_State::GLState::CurrentVertexAttributeValue& value,
                                      Uint32 writtenClass, MGPAttribValue& out) {
        out = MGPAttribValue{};
        out.Location = location;
        out.ValueClass = static_cast<Uint8>(writtenClass);
        static_assert(sizeof(out.Data) == sizeof(value.floatValue), "MGPAttribValue::Data is four words");
        switch (writtenClass) {
        case MG_State::GLState::kVertexAttribValueClassInt:
            std::memcpy(out.Data, value.intValue.data(), sizeof(out.Data));
            break;
        case MG_State::GLState::kVertexAttribValueClassUint:
            std::memcpy(out.Data, value.uintValue.data(), sizeof(out.Data));
            break;
        default:
            std::memcpy(out.Data, value.floatValue.data(), sizeof(out.Data));
            break;
        }
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
#endif // MOBILEGL_PIPE_PUSH
