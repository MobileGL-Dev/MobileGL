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
// computed by comparing a shutter against what the tracker last pushed. P2 EMITS for bits
// 0..4 only (the value-class ones); bits 5..17 are computed, latched and counted so the
// per-bit fire rate is a measurement rather than a plan, and their fields keep going
// through the residual fill until P3a/P3b/P4a/P4b.
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
        // ---- value class: computed and counted, emitted from P3a on ----
        NewVertexElements,       // the bound VAO's attribute configuration
        NewShader,               // the current program's link version
        NewShaderBindings,       // image units, block bindings, uniform write set
        NewGlobalConstants,      // the default-uniform-block image
        // ---- object class: computed and counted, emitted from P3b/P4b on ----
        NewVertexBuffers,
        NewIndexBuffer,
        NewFramebuffer,
        NewSamplerViews,
        NewSamplers,
        NewShaderImages,
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

    // The five P2 emits for.
    inline constexpr Uint32 kMGPipeDirtyEmittedAtP2 =
        MGPipeDirtyBit(MGPipeDirty::NewRenderState) | MGPipeDirtyBit(MGPipeDirty::NewPipelineState) |
        MGPipeDirtyBit(MGPipeDirty::NewPixelPack) | MGPipeDirtyBit(MGPipeDirty::NewPatchState) |
        MGPipeDirtyBit(MGPipeDirty::NewVertexAttribDefaults);

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
        default:
            // Bits 5..17 have no call of their own until P3a/P3b/P4a/P4b, so there is no
            // subsystem to switch and the residual fill keeps supplying their fields.
            return 0;
        }
    }

    // A COMPOSITE shutter, for the bits whose "did anything move" is more than one counter.
    // It is a hash, so two different states can in principle collide and cost a MISSED fire.
    // That is acceptable for bits 5..17 and only for them: nothing consumes those bits in
    // P2, and P3 replaces each with its own exact shutter as it takes the subsystem over.
    // The five bits P2 EMITS for are never composed - they are widened counters and byte
    // compares, neither of which can collide.
    inline constexpr Uint64 MGPipeMixShutter(Uint64 accumulator, Uint64 value) {
        accumulator ^= value + 0x9e3779b97f4a7c15ull + (accumulator << 6) + (accumulator >> 2);
        return accumulator;
    }

    // A Uint16 counter widened at the TRACKER boundary, never in MG_State
    // (ARCHITECTURE.md 5.2: MG_State is not changed for this). A decrease is a wrap and adds
    // 65536. A wrap is harmless locally - one extra re-push, never a missed one - which is
    // exactly what TrackerTest.WrapAroundRePushesButNeverMisses pins.
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

            now[Index(MGPipeDirty::NewVertexBuffers)] =
                MGPipeMixShutter(ctx.GetAnyVaoAttributeGeneration(), vaoIdentity);
            // The index buffer lives in the bound VAO's element slot and P2 has no cheap
            // shutter for that slot alone, so it shares the buffer aggregate and over-fires
            // on any buffer write anywhere. P3b narrows it when it takes the subsystem over.
            now[Index(MGPipeDirty::NewIndexBuffer)] = MGPipeMixShutter(buffers, vaoIdentity);
            now[Index(MGPipeDirty::NewFramebuffer)] = MGPipeMixShutter(
                ctx.GetAnyFramebufferAttachmentGeneration(),
                m_framebufferBind.Observe(
                    ctx.GetFramebufferBindingSlot(FramebufferTarget::Draw).GetVersion()));
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
        void Reset() {
            std::memset(m_lastPushed, 0, sizeof(m_lastPushed));
            m_renderStateVersion.Reset();
            m_pipelineStateVersion.Reset();
            m_framebufferBind.Reset();
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

    // The monolith's one tracker. Under split there is one per client context; the context
    // identity check inside Update is what makes the single instance safe today.
    inline MGPipeTracker& MGPipeTrackerInstance() {
        static MGPipeTracker tracker;
        return tracker;
    }
} // namespace MobileGL::MG_Pipe
#endif // MOBILEGL_PIPE_PUSH
