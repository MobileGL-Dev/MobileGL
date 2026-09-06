// MobileGL - MobileGL/MG_Impl/Pipe/PipeFill.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client side of the PipeInputs block (ARCHITECTURE.md 9.2 phase A): the only place in
// the push arm that reads MG_State::pGLContext. Holds the per-verb filler, the F-class
// forwarders, IsLive, the MOBILEGL_PIPE_POISON_OMIT knob and - in a verify build - the
// second arm (SnapshotFromGLContext), the entry compare, the compare-at-read hook and the
// MOBILEGL_PIPE_VERIFY_CORRUPT / _FATAL knobs. Compiled only under MOBILEGL_PIPE_PUSH
// (CMakeLists.txt appends it to SOURCE_FILES there).
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/BufferState/BufferState.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/CsoCache.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_Impl/Pipe/SetHashSuppressor.h>
#include <MG_Impl/Pipe/Tracker.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Pipe/PipeMutation.h>
#include <Config.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Pipe {
    using GLContext = MG_State::GLState::GLContext;

    // The one door into PipeInputs' storage on the client side. A struct rather than a
    // list of friend functions so the header names exactly one friend.
    struct MGPipeFillAccess {
        // Copies ONE field's storage out of the live context by calling the GLContext
        // accessor of the same name (P1 brief D4: no derivation logic is re-implemented
        // here, which is what keeps the copy semantically identical by construction).
        // A forwarded field has no storage and copies nothing.
        // The two doors the P2 emission step needs into PipeInputs' storage. They exist
        // only for ApplierDerivesRenderStateFields' one-shot probe below; nothing on the hot
        // path writes through them.
        static RenderStateParameters& RenderStateOf(PipeInputs& inputs) { return inputs.m_renderState; }
        static Uint32& ClearStencilOf(PipeInputs& inputs) { return inputs.m_clearStencil; }

        static void CopyField(PipeInputs& dst, GLContext& ctx, MGPipeInputField field) {
            using F = MGPipeInputField;
            using MG_State::GLState::BufferBindPointTargets;
            using MG_State::GLState::GlobalBufferTargets;
            switch (field) {
            case F::GetActiveTextureUnit:
                dst.m_activeTextureUnit = ctx.GetActiveTextureUnit();
                break;
            case F::GetBlendColor:
                dst.m_blendColor = ctx.GetBlendColor();
                break;
            case F::GetBlendEquationIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    ctx.GetBlendEquationIndexed(i, dst.m_blendEquation[i][0], dst.m_blendEquation[i][1]);
                }
                break;
            case F::GetBlendFuncIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    ctx.GetBlendFuncIndexed(i, dst.m_blendFunc[i][0], dst.m_blendFunc[i][1], dst.m_blendFunc[i][2],
                                            dst.m_blendFunc[i][3]);
                }
                break;
            case F::GetBoundTransformFeedbackName:
                dst.m_boundTransformFeedbackName = ctx.GetBoundTransformFeedbackName();
                break;
            case F::GetBoundVertexArray:
                dst.m_boundVertexArray = ctx.GetBoundVertexArray();
                break;
            case F::GetBufferBindingSlot:
                // Every global target has a slot; Index stays null - GLContext resolves it
                // through the bound VAO's element-buffer slot (Core.cpp), a derivation no
                // FillPoints.def row can copy - and a read of it is the poison Fatal in the
                // accessor. No backend reads it today (every slot read is DrawIndirect,
                // DispatchIndirect, Parameter or PixelPack).
                for (const auto target : GlobalBufferTargets) {
                    dst.m_bufferBindingSlot[static_cast<SizeT>(target)] = &ctx.GetBufferBindingSlot(target);
                }
                break;
            case F::GetBufferBindingPoint:
                // The live storage is Array<Array<BindingSlotRange1D, BufferBindingPointCount>, N>
                // (BufferState.h), so the address of point 0 is the base of that target's row.
                for (const auto target : BufferBindPointTargets) {
                    dst.m_bufferBindingPointBase[static_cast<SizeT>(target)] = &ctx.GetBufferBindingPoint(target, 0);
                }
                break;
            case F::GetTouchedBufferBindingPointCount:
                for (const auto target : BufferBindPointTargets) {
                    dst.m_touchedBindingPointCount[static_cast<SizeT>(target)] =
                        ctx.GetTouchedBufferBindingPointCount(target);
                }
                break;
            case F::GetClampReadColor:
                dst.m_clampReadColor = ctx.GetClampReadColor();
                break;
            case F::GetClearColor:
                dst.m_clearColor = ctx.GetClearColor();
                break;
            case F::GetClearDepth:
                dst.m_clearDepth = ctx.GetClearDepth();
                break;
            case F::GetClearStencil:
                dst.m_clearStencil = ctx.GetClearStencil();
                break;
            case F::GetColorMaskIndexed:
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    dst.m_colorMask[i] = ctx.GetColorMaskIndexed(i);
                }
                break;
            case F::GetCullFaceMode:
                dst.m_cullFaceMode = ctx.GetCullFaceMode();
                break;
            case F::GetCurrentVertexAttribute:
                for (Uint i = 0; i < PipeInputs::kMaxVertexAttribs; ++i) {
                    dst.m_currentVertexAttribute[i] = ctx.GetCurrentVertexAttribute(i);
                }
                break;
            case F::GetDepthFunc:
                dst.m_depthFunc = ctx.GetDepthFunc();
                break;
            case F::GetDepthMask:
                dst.m_depthMask = ctx.GetDepthMask();
                break;
            case F::GetDepthRangeIndexed:
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_depthRange[i] = ctx.GetDepthRangeIndexed(i);
                }
                break;
            case F::GetFramebufferBindingSlot:
                for (SizeT i = 0; i < PipeInputs::kFramebufferTargetCount; ++i) {
                    dst.m_framebufferBindingSlot[i] =
                        &ctx.GetFramebufferBindingSlot(static_cast<PipeInputs::FramebufferTarget>(i));
                }
                break;
            case F::GetImageTextureBinding:
                // Array<ImageTextureBinding, MAX_TEXTURE_IMAGE_UNITS> (TextureState.h): unit 0's
                // address is the base.
                dst.m_imageTextureBindingBase = &ctx.GetImageTextureBinding(0);
                break;
            case F::GetLineWidth:
                dst.m_lineWidth = ctx.GetLineWidth();
                break;
            case F::GetLogicOp:
                dst.m_logicOp = ctx.GetLogicOp();
                break;
            case F::GetMaxTouchedTextureUnit:
                dst.m_maxTouchedTextureUnit = ctx.GetMaxTouchedTextureUnit();
                break;
            case F::GetMinSampleShadingValue:
                dst.m_minSampleShadingValue = ctx.GetMinSampleShadingValue();
                break;
            case F::GetPatchDefaultInnerLevel:
                dst.m_patchDefaultInnerLevel = ctx.GetPatchDefaultInnerLevel();
                break;
            case F::GetPatchDefaultOuterLevel:
                dst.m_patchDefaultOuterLevel = ctx.GetPatchDefaultOuterLevel();
                break;
            case F::GetPatchVertices:
                dst.m_patchVertices = ctx.GetPatchVertices();
                break;
            case F::GetPipelineStateVersion:
                dst.m_pipelineStateVersion = ctx.GetPipelineStateVersion();
                break;
            case F::GetPixelStoreParameters:
                dst.m_pixelStore[0] = ctx.GetPixelStoreParameters(false);
                dst.m_pixelStore[1] = ctx.GetPixelStoreParameters(true);
                break;
            case F::GetPolygonModeFront:
                dst.m_polygonModeFront = ctx.GetPolygonModeFront();
                break;
            case F::GetPolygonOffsetFactor:
                dst.m_polygonOffsetFactor = ctx.GetPolygonOffsetFactor();
                break;
            case F::GetPolygonOffsetUnits:
                dst.m_polygonOffsetUnits = ctx.GetPolygonOffsetUnits();
                break;
            case F::GetPrimitiveRestartIndex:
                dst.m_primitiveRestartIndex = ctx.GetPrimitiveRestartIndex();
                break;
            case F::GetProgramForDispatch:
                dst.m_programForDispatch = ctx.GetProgramForDispatch();
                break;
            case F::GetProgramForDraw:
                dst.m_programForDraw = ctx.GetProgramForDraw();
                break;
            case F::GetProvokingVertexMode:
                dst.m_provokingVertexMode = ctx.GetProvokingVertexMode();
                break;
            case F::GetRenderStateParameters:
                dst.m_renderState = ctx.GetRenderStateParameters();
                break;
            case F::GetRenderStateParametersVersion:
                dst.m_renderStateParametersVersion = ctx.GetRenderStateParametersVersion();
                break;
            case F::GetSamplingResolutionGeneration:
                dst.m_samplingResolutionGeneration = ctx.GetSamplingResolutionGeneration();
                break;
            case F::GetScissorBox:
                dst.m_scissorBox = ctx.GetScissorBox();
                break;
            case F::GetStencilState:
                dst.m_stencil[0] = ctx.GetStencilState(StencilFace::Front);
                dst.m_stencil[1] = ctx.GetStencilState(StencilFace::Back);
                break;
            case F::GetTextureBindGeneration:
                dst.m_textureBindGeneration = ctx.GetTextureBindGeneration();
                break;
            case F::GetTextureContextId:
                dst.m_textureContextId = ctx.GetTextureContextId();
                break;
            case F::GetTextureUnitObject:
                // Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS> (TextureState.h): unit 0 is the base.
                dst.m_textureUnitBase = &ctx.GetTextureUnitObject(0);
                break;
            case F::GetTransformFeedbackCapturedVertices:
                dst.m_transformFeedbackCapturedVertices = ctx.GetTransformFeedbackCapturedVertices();
                break;
            case F::GetTransformFeedbackGeneration:
                dst.m_transformFeedbackGeneration = ctx.GetTransformFeedbackGeneration();
                break;
            case F::GetTransformFeedbackPausedPrimitiveCounter:
                dst.m_transformFeedbackPausedPrimitiveCounter = ctx.GetTransformFeedbackPausedPrimitiveCounter();
                break;
            case F::GetTransformFeedbackProgram:
                dst.m_transformFeedbackProgram = ctx.GetTransformFeedbackProgram();
                break;
            case F::GetViewport:
                dst.m_viewport = ctx.GetViewport();
                break;
            case F::GetViewportIndexed:
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_viewportIndexed[i] = ctx.GetViewportIndexed(i);
                }
                break;
            case F::IsCapabilityEnabled:
                // Every capability, FramebufferSrgb included: it copies today's constant false
                // (MEASUREMENTS.md), so no value changes.
                for (SizeT i = 0; i < PipeInputs::kCapabilityCount; ++i) {
                    dst.m_capability[i] = ctx.IsCapabilityEnabled(static_cast<CapabilityInput>(i));
                }
                break;
            case F::IsCapabilityEnabledIndexed:
                // The only two indexed capabilities GLContext keeps (RenderState).
                for (Uint i = 0; i < kMGMaxDrawBuffers; ++i) {
                    dst.m_capabilityIndexed.Blend[i] = ctx.IsCapabilityEnabledIndexed(CapabilityInput::Blend, i);
                }
                for (Uint i = 0; i < PipeInputs::kMaxViewports; ++i) {
                    dst.m_capabilityIndexed.ScissorTest[i] =
                        ctx.IsCapabilityEnabledIndexed(CapabilityInput::ScissorTest, i);
                }
                break;
            case F::IsTransformFeedbackActive:
                dst.m_transformFeedbackActive = ctx.IsTransformFeedbackActive();
                break;
            case F::IsTransformFeedbackPaused:
                dst.m_transformFeedbackPaused = ctx.IsTransformFeedbackPaused();
                break;
            case F::GetBoundTransformFeedbackLifetimeId:
                dst.m_boundTransformFeedbackLifetimeId = ctx.GetBoundTransformFeedbackLifetimeId();
                break;
            // The seven forwarded fields: nothing to copy.
            case F::GetBufferBindingPointCount:
            case F::GetProgramObject:
            case F::GetTextureObject:
            case F::HasOpenTransformFeedbackSpan:
            case F::InvalidateCompileEnv:
            case F::ValidateProgramName:
            case F::RecordError:
            case F::kFieldCount:
                break;
            }
        }

        static void SetIdentity(PipeInputs& inputs, GLContext* ctx) {
            inputs.m_live = ctx != nullptr;
            inputs.m_contextIdentity = ctx;
        }
        static void SetVerb(PipeInputs& inputs, MGPipeVerb verb) { inputs.m_currentVerb = verb; }
#if MOBILEGL_PIPE_POISON
        static MGPipeFilledState& Filled(PipeInputs& inputs) { return inputs.m_filled; }
#endif
    };

    namespace {
        GLContext* LiveContext() { return MG_State::pGLContext.get(); }

        template <class T>
        const SharedPtr<T>& NullShared() {
            static const SharedPtr<T> null;
            return null;
        }

        [[noreturn]] void BadKnob(const char* knob, const char* value, const char* why) {
            MGLOG_F("MGPipe: Fatal{PipeVerifyBadKnob, \"%s=%s\": %s}", knob, value, why);
            std::abort();
        }

        // ---- MOBILEGL_PIPE_POISON_OMIT (negative control B, P1 brief D6) ----
        // The filler skips the STAMP (never the value) of one (verb, field) pair: an omission
        // indistinguishable from a forgotten FillPoints.def row, so that verb's read of the
        // field is Fatal{UnmigratedPipeInput, "Field@Verb"} and no other verb is affected.
        struct PoisonOmission {
            Bool Armed = false;
            MGPipeVerb Verb = MGPipeVerb::kVerbCount;
            MGPipeInputField Field = MGPipeInputField::kFieldCount;
        };
        PoisonOmission g_omission;
        Bool g_omissionKnobParsed = false;
        String g_omissionKnobValue; // the value the last parse saw

        // Parsed on the first fill and again only when the value changes. A lane loads
        // Features once, before any fill, so that is one parse per process there; a forked
        // test child that sets Features after its parent already filled gets its own parse,
        // which is what puts the parser and its Fatal{PipeVerifyBadKnob} under a unit test.
        // An empty value never clears an omission a test armed through MGPipeSetPoisonOmission.
        void ParsePoisonOmissionKnob() {
            const String& knob = MG_Config::Features.PipePoisonOmit;
            if (g_omissionKnobParsed && knob == g_omissionKnobValue) return;
            g_omissionKnobParsed = true;
            g_omissionKnobValue = knob;
            if (knob.empty()) return;
            const auto colon = knob.find(':');
            if (colon == String::npos || colon == 0 || colon + 1 >= knob.size()) {
                BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "expected <Verb>:<FieldName>");
            }
            const String verbName = knob.substr(0, colon);
            const String fieldName = knob.substr(colon + 1);
            const auto verb = MGPipeFindVerb(verbName.c_str());
            if (!verb) BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "no such verb in kMGPipeVerbNames");
            const auto field = MGPipeFindInputField(fieldName.c_str());
            if (!field) BadKnob("MOBILEGL_PIPE_POISON_OMIT", knob.c_str(), "no such field in kMGPipeInputFieldNames");
            MGPipeSetPoisonOmission(verbName.c_str(), fieldName.c_str());
        }

        [[maybe_unused]] Bool IsOmitted(MGPipeVerb verb, MGPipeInputField field) {
            return g_omission.Armed && g_omission.Verb == verb && g_omission.Field == field;
        }

#if MOBILEGL_PIPE_VERIFY
        // ---- the MOBILEGL_PIPE_VERIFY comparator (P1 brief D8) ----
        // Two mechanisms, both active only when Features.PipeVerify is set: the ENTRY compare
        // once per verb (the pushed block against a second snapshot of the live context,
        // taken at the same instant - tautological until P2 gives the first arm a real
        // filler, and kept falsifiable by MOBILEGL_PIPE_VERIFY_CORRUPT), and the
        // COMPARE-AT-READ in every accessor (the stored value against a fresh read of the
        // live context at the moment the backend reads it - the arm that is real in P1: it
        // catches a value that changed between the verb boundary and the read).
        PipeInputs g_snapshot{};    // the second arm
        PipeInputs g_readScratch{}; // where the compare-at-read re-read lands

        // The read hook arms at the first fill (ArmVerify below), so it cannot see a read
        // made before that. That window is covered by the poison instead: MGP_INPUT_CHECK
        // precedes MGP_INPUT_VERIFY_READ in every accessor and a stamp of 0 is never fresh,
        // so such a read is Fatal{UnmigratedPipeInput, "<Field>@<none>"} before the hook
        // could matter - which holds only while a verify build always carries the poison.
        static_assert(MOBILEGL_PIPE_POISON, "the compare-at-read hook relies on the poison for reads before the first fill");

        struct VerifyState {
            Bool Parsed = false;
            Bool Enabled = false;
            Bool Fatal = true;
            Bool InHook = false; // a re-read that re-enters an accessor is not re-verified
            Optional<MGPipeInputField> Corrupt;
            String CorruptKnob; // the MOBILEGL_PIPE_VERIFY_CORRUPT value the last arm saw
            std::atomic<Uint64> Divergences{0};
            ~VerifyState() {
                const Uint64 count = Divergences.load(std::memory_order_relaxed);
                if (count != 0) {
                    MGLOG_E("MGPipe: verify summary - %llu divergence(s) survived MOBILEGL_PIPE_VERIFY_FATAL=0",
                            static_cast<unsigned long long>(count));
                }
            }
        };
        VerifyState g_verify;

        // Armed on the first fill and re-armed when any of the three verify knobs'
        // Features value (PipeVerify, PipeVerifyFatal, PipeVerifyCorrupt) differs from what
        // the last arm latched (the same reason as ParsePoisonOmissionKnob: one arm per lane
        // process, a fresh arm for a forked test child that turns a knob after its parent
        // filled). Cost: two Bool compares and one String compare per fill, verify builds only.
        void ArmVerify() {
            const auto& features = MG_Config::Features;
            if (g_verify.Parsed && g_verify.Enabled == features.PipeVerify && g_verify.Fatal == features.PipeVerifyFatal &&
                g_verify.CorruptKnob == features.PipeVerifyCorrupt) {
                return;
            }
            g_verify.Parsed = true;
            g_verify.Enabled = features.PipeVerify;
            g_verify.Fatal = features.PipeVerifyFatal;
            g_verify.CorruptKnob = features.PipeVerifyCorrupt;
            g_verify.Corrupt = Optional<MGPipeInputField>{};
            if (!g_verify.Enabled) return;
            const String& corrupt = g_verify.CorruptKnob;
            if (!corrupt.empty()) {
                const auto field = MGPipeFindInputField(corrupt.c_str());
                if (!field) {
                    BadKnob("MOBILEGL_PIPE_VERIFY_CORRUPT", corrupt.c_str(), "no such field in kMGPipeInputFieldNames");
                }
                g_verify.Corrupt = field;
            }
            // The lanes grep for this line: a verify run whose log lacks it never armed.
            MGLOG_I("MGPipe: verify armed - %u fields, %u verbs, fatal=%d", static_cast<unsigned>(kMGPipeInputFieldCount),
                    static_cast<unsigned>(kMGPipeVerbCount), g_verify.Fatal ? 1 : 0);
            if (g_verify.Corrupt) {
                MGLOG_I("MGPipe: verify corruption armed - %s", kMGPipeInputFieldNames[static_cast<SizeT>(*g_verify.Corrupt)]);
            }
        }

        void ReportDivergence(MGPipeInputField field, const char* where) {
            const Uint64 serial = MGPipeFillAccess::Filled(gPipeInputs).CurrentVerbSerial;
            MGLOG_F("MGPipe: Fatal{PipeVerifyDiffer, \"%s@%s\", verb=%llu, where=%s}",
                    kMGPipeInputFieldNames[static_cast<SizeT>(field)], MGPipeVerbName(gPipeInputs.CurrentVerb()),
                    static_cast<unsigned long long>(serial), where);
            if (g_verify.Fatal) std::abort();
            g_verify.Divergences.fetch_add(1, std::memory_order_relaxed);
        }

        void EntryCompare(PipeInputs& inputs, const MGPipeFieldMask& mask) {
            if (!g_verify.Enabled) return;
            SnapshotFromGLContext(g_snapshot, mask);
            // Negative control A: perturb the SNAPSHOT arm, so a green run goes red naming the
            // field. A field outside this verb's mask is not compared and stays untouched.
            if (g_verify.Corrupt && MGPipeFieldMaskHas(mask, *g_verify.Corrupt)) {
                MGPipeApplyVerifyCorruption(g_snapshot, *g_verify.Corrupt);
            }
            MGPipeInputField differing = MGPipeInputField::kFieldCount;
            if (!MGPipeVerifyInputs(inputs, g_snapshot, mask, &differing)) ReportDivergence(differing, "entry");
        }
#endif // MOBILEGL_PIPE_VERIFY
    } // namespace

#if MOBILEGL_PIPE_VERIFY
    void SnapshotFromGLContext(PipeInputs& snapshot, const MGPipeFieldMask& mask) {
        auto* ctx = LiveContext();
        MGPipeFillAccess::SetIdentity(snapshot, ctx);
        MGPipeFillAccess::SetVerb(snapshot, gPipeInputs.CurrentVerb());
        if (ctx == nullptr) return;
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            if (!MGPipeFieldMaskHas(mask, field) || kMGPipeInputFieldSticky[i]) continue;
            MGPipeFillAccess::CopyField(snapshot, *ctx, field);
        }
    }

    void MGPipeVerifyReadHook(const PipeInputs& self, MGPipeInputField field, Uint index0, Uint index1) {
        if (&self != &gPipeInputs || !g_verify.Enabled || g_verify.InHook) return;
        const auto index = static_cast<SizeT>(field);
        if (kMGPipeInputFieldSticky[index]) return;
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        // The whole field is re-read and compared - a superset of "the same indices", so a
        // divergence in an index the backend did not ask for is still a divergence between
        // the boundary value and the live value. The indices only decorate the report. The
        // cost is per backend read (GetRenderStateParameters re-copies and compares the whole
        // struct; GetProgramForDraw re-joins the pending link), inside the verify budget and
        // to be kept in mind when reading the verify lane's wall time.
        // InHook: the re-read calls the same GLContext accessor the filler calls, and
        // GetProgramForDraw's join can re-enter a backend and with it another gPipeInputs
        // accessor; that inner read is a plain load rather than a second hook, so the hook
        // never recurses (and never reports the inner read against a half-copied scratch).
        g_verify.InHook = true;
        MGPipeFillAccess::CopyField(g_readScratch, *ctx, field);
        const Bool equal = MGPipeInputsFieldEqual(field, self, g_readScratch);
        g_verify.InHook = false;
        if (equal) return;
        MGLOG_E("MGPipe: verify read of %s (index %u, %u) differs from the live context", kMGPipeInputFieldNames[index],
                index0, index1);
        ReportDivergence(field, "read");
    }
#endif // MOBILEGL_PIPE_VERIFY

    // ---- push on mutation (P1 lane finding F2) ----
    // A backend that writes a frontend object inside its own verb moves a value the verb
    // boundary already copied: Magma's ResolveSamplerDescriptor synthesises a fallback
    // texture for an unbound sampler and its AllocateStorage/SetInternalFormat bump the
    // context's sampling-resolution generation, so every read of that field after the
    // fallback differs from the live context (the two SampledSetStaleness / six
    // UnboundImageDescriptor entries the verify lane aborted on). The frontend mutator
    // spells MGP_NOTE_MUTATION(Field) at the point of the move and lands here.
    //
    // Only the value is refreshed. The stamp is deliberately left alone: a field whose stamp
    // this verb withheld (negative control B) must stay stale, and a field the verb never
    // filled must stay Fatal{UnmigratedPipeInput} on the next read rather than be healed by
    // an unrelated frontend write.
    void MGPipeNoteFrontendMutation(MGPipeInputField field) {
        PipeInputs& inputs = gPipeInputs;
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        const auto verb = inputs.CurrentVerb();
        if (verb == MGPipeVerb::kVerbCount) return; // nothing has filled the block yet
        const auto index = static_cast<SizeT>(field);
        if (kMGPipeInputFieldSticky[index]) return; // forwarded: no storage to refresh
        const MGPipeFieldMask& mask =
            kMGPipeClassFieldMask[static_cast<SizeT>(kMGPipeVerbClass[static_cast<SizeT>(verb)])];
        if (!MGPipeFieldMaskHas(mask, field)) return; // this verb never pushed it
        MGPipeFillAccess::CopyField(inputs, *ctx, field);
    }

    // ---- the aggregate generations (P2 brief D4) ----
    // MGP_NOTE_AGGREGATE lands here. The bump points are on OBJECTS, which have no
    // back-pointer to the state container that owns them, so the note finds the live
    // context - the same shape, and for the same reason, as MGPipeNoteFrontendMutation
    // above. No verb has to be in flight and no field is stamped: an aggregate generation
    // is not a PipeInputs field, it is what the tracker's shutter compares against.
    void MGPipeNoteAggregate(MGPipeAggregate aggregate) {
        auto* ctx = LiveContext();
        if (ctx == nullptr) return;
        switch (aggregate) {
        case MGPipeAggregate::VaoAttribute:
            ctx->NoteVaoAttributeChanged();
            break;
        case MGPipeAggregate::FramebufferAttachment:
            ctx->NoteFramebufferAttachmentChanged();
            break;
        case MGPipeAggregate::TextureContent:
            ctx->NoteTextureContentChanged();
            break;
        case MGPipeAggregate::TextureParams:
            ctx->NoteTextureParamsChanged();
            break;
        case MGPipeAggregate::BufferChange:
            ctx->NoteBufferChanged();
            break;
        case MGPipeAggregate::VertexAttribDefault:
            ctx->NoteVertexAttribDefaultChanged();
            break;
        case MGPipeAggregate::Count:
            break;
        }
    }

    void MGPipeSetPoisonOmission(const char* verb, const char* field) {
        if (verb == nullptr || field == nullptr) {
            g_omission = PoisonOmission{};
            return;
        }
        const auto v = MGPipeFindVerb(verb);
        const auto f = MGPipeFindInputField(field);
        if (!v || !f) BadKnob("MOBILEGL_PIPE_POISON_OMIT", verb, "unknown verb or field");
        g_omission.Armed = true;
        g_omission.Verb = *v;
        g_omission.Field = *f;
#if MOBILEGL_PIPE_POISON
        MGLOG_I("MGPipe: poison omission armed - %s@%s", field, verb);
#else
        MGLOG_W_ONCE("MGPipe: poison omission %s@%s requested but the poison is not compiled in "
                     "(MOBILEGL_PIPE_POISON=0): no stamp exists to omit",
                     field, verb);
#endif
    }

    // ---- liveness ----
    Bool PipeInputs::IsLive() const { return LiveContext() != nullptr; }

    // ---- the seven F-class forwarders ----
    SizeT PipeInputs::GetBufferBindingPointCount(BufferTarget target) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetBufferBindingPointCount(target) : 0;
    }

    const SharedPtr<PipeInputs::ProgramObject>& PipeInputs::GetProgramObject(Uint index) {
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetProgramObject(index) : NullShared<ProgramObject>();
    }

    const SharedPtr<PipeInputs::ITextureObject>& PipeInputs::GetTextureObject(Uint index) {
        auto* ctx = LiveContext();
        return ctx != nullptr ? ctx->GetTextureObject(index) : NullShared<ITextureObject>();
    }

    Bool PipeInputs::HasOpenTransformFeedbackSpan(Uint64 lifetimeId) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->HasOpenTransformFeedbackSpan(lifetimeId);
    }

    void PipeInputs::InvalidateCompileEnv() {
        if (auto* ctx = LiveContext()) ctx->InvalidateCompileEnv();
    }

    Bool PipeInputs::ValidateProgramName(Uint index) const {
        const auto* ctx = LiveContext();
        return ctx != nullptr && ctx->ValidateProgramName(index);
    }

    void PipeInputs::RecordError(ErrorCode code, UniquePtr<ErrorInfo> info) {
        auto* ctx = LiveContext();
        if (ctx == nullptr) {
            MGLOG_E_ONCE("PipeInputs::RecordError: no live context, dropping error %d", static_cast<int>(code));
            return;
        }
        ctx->RecordError(code, Move(info));
    }

    void MGPipeLeaveVerb() {
        PipeInputs& inputs = gPipeInputs;
#if MOBILEGL_PIPE_POISON
        // Same bump the next fill would make, without a verb to fill from: no field is
        // stamped, so every stamp this verb made falls behind the serial.
        ++MGPipeFillAccess::Filled(inputs).CurrentVerbSerial;
#endif
        MGPipeFillAccess::SetVerb(inputs, MGPipeVerb::kVerbCount);
    }


    // ================================================================================
    // The emission step (P2 brief D1 step 3, D5, D6, D7)
    // ================================================================================
    namespace {
        // Which runtime MOBILEGL_PIPE_PUSH subsystem owns a field, through the call that now
        // supplies it. Zero means "still pulled".
        constexpr Uint64 SubsystemForEmitter(MGPipeFieldEmitter emitter) {
            switch (emitter) {
            case MGPipeFieldEmitter::BindRenderState:
            case MGPipeFieldEmitter::CreateRenderState:
            case MGPipeFieldEmitter::SetDynamicState:
                return kMGPipeSubsystemRenderState;
            case MGPipeFieldEmitter::SetPixelPackState:
                return kMGPipeSubsystemPixelPack;
            case MGPipeFieldEmitter::SetPatchState:
                return kMGPipeSubsystemPatchState;
            case MGPipeFieldEmitter::SetVertexAttribDefaults:
                return kMGPipeSubsystemVertexAttribDefaults;
            case MGPipeFieldEmitter::kNone:
                break;
            }
            return 0;
        }

        // Which of those subsystems THIS BUILD actually emits for. It grows one commit at a
        // time, and a field whose emitter is not wired here keeps being pulled - so adding a
        // row to Coverage.def can never silently drop a field on the floor before the call
        // that carries it exists.
        constexpr Uint64 kMGPipeWiredSubsystems = kMGPipeSubsystemRenderState |
                                                  kMGPipeSubsystemPixelPack |
                                                  kMGPipeSubsystemPatchState |
                                                  kMGPipeSubsystemVertexAttribDefaults;

        // A field an emitted call supplies COMPLETELY, so the residual fill may stop pulling
        // it. Two rows of Coverage.def's emitted list do not qualify and each has its reason
        // recorded here rather than a silent absence:
        //
        //   GetPixelStoreParameters is BOTH halves of the pixel store (m_pixelStore[0] pack
        //     and [1] unpack) and set_pixel_pack_state deliberately carries only PACK
        //     (ARCHITECTURE.md 4.6 D5, MGPipeTypes.h). The unpack half has no carrier at all,
        //     so the field keeps being pulled and the verify comparator keeps proving it.
        //
        //   GetCurrentVertexAttribute's three views are NOT bit-identical: GLContext
        //     CONVERTS between them (SetCurrentVertexAttributeFloat writes (Int32)value into
        //     intValue), while MGPipeApplySetVertexAttribDefaults memcpys one Data[4] into
        //     all three and ignores MGPAttribValue::ValueClass, which the wire type carries
        //     precisely so it does not have to. Until that applier reads ValueClass the
        //     carrier cannot reproduce the frontend value, so the field keeps being pulled.
        //     The call is still emitted: the wire shape, the payload bytes and the set-hash
        //     suppressor are all real, and the residual fill runs AFTER emission, so the
        //     mirror ends up with the frontend's value either way.
        constexpr Bool EmittedCallSuppliesTheWholeField(MGPipeInputField field) {
            switch (field) {
            case MGPipeInputField::GetPixelStoreParameters:
            case MGPipeInputField::GetCurrentVertexAttribute:
                return false;
            default:
                return true;
            }
        }

        // The fields the applier writes DIRECTLY, out of the chunk bytes it scattered. Every
        // other emitted field reaches PipeInputs only through
        // MGPipeDeriveRenderStateFields, which is why the probe below exists.
        constexpr Bool AppliedWithoutDerivation(MGPipeInputField field) {
            switch (field) {
            case MGPipeInputField::GetRenderStateParameters:
            case MGPipeInputField::GetRenderStateParametersVersion:
            case MGPipeInputField::GetPipelineStateVersion:
            case MGPipeInputField::GetPixelStoreParameters:
            case MGPipeInputField::GetPatchVertices:
            case MGPipeInputField::GetPatchDefaultOuterLevel:
            case MGPipeInputField::GetPatchDefaultInnerLevel:
            case MGPipeInputField::GetCurrentVertexAttribute:
                return true;
            default:
                return false;
            }
        }

        // DOES THIS TREE'S APPLIER ACTUALLY DERIVE?
        //
        // MGPipeDeriveRenderStateFields is package A's, and on the P2 contract tag it is a
        // declared stub whose body lands in A's follow-on commit. A field that reaches
        // PipeInputs only through that derivation must NOT be skipped by the residual fill
        // while the derivation is a stub: skipping it would leave the mirror unwritten and
        // the backend reading a default.
        //
        // Rather than hard-code which branch this is, the filler asks once: it puts a
        // sentinel in a scratch block's working RenderStateParameters, clears the mirror the
        // derivation is supposed to recompute, runs the derivation, and looks. The answer is
        // latched for the process and costs one compare, once.
        //
        // It stays useful after A lands: if the derivation is ever deleted or gated off, the
        // filler degrades to PULLING those fields instead of rendering a default, which is
        // the safe direction. The verify lane and RenderStateSpansTest are what say the
        // derivation is CORRECT; this only says it is THERE.
        Bool ApplierDerivesRenderStateFields() {
            static const Bool answer = [] {
                static PipeInputs probe;
                constexpr Uint32 kSentinel = 0x5a5a5a5au;
                MGPipeFillAccess::RenderStateOf(probe).ClearStencil = kSentinel;
                MGPipeFillAccess::ClearStencilOf(probe) = 0u;
                MGPipeDeriveRenderStateFields(probe);
                const Bool derives = MGPipeFillAccess::ClearStencilOf(probe) == kSentinel;
                if (!derives) {
                    MGLOG_W_ONCE("MGPipe: MGPipeDeriveRenderStateFields does not derive on this "
                                 "build - the render-state mirrors stay on the pull path");
                }
                return derives;
            }();
            return answer;
        }


        // set_pixel_pack_state. PACK only, deliberately: nothing on the far side of the
        // boundary reads unpack state, and the staged-repack upload path does not even issue
        // glPixelStorei (ARCHITECTURE.md 4.6 D5).
        Uint64 EmitPixelPackState(GLContext& ctx) {
            MGPPixelPackState pack{};
            pack.Pack = ctx.GetPixelStoreParameters(false);
            MGPipeApplySetPixelPackState(pack);
            return sizeof(MGPPixelPackState);
        }

        // set_patch_state. The trio ALSO travels in pipeline chunk P0, and that redundancy is
        // a trip wire rather than waste: the applier asserts under verify that the two
        // carriers agree. 28 bytes on a state that changes about once per program.
        Uint64 EmitPatchState(GLContext& ctx) {
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            MGPPatchState patch{};
            patch.Vertices = live.PatchVertices;
            for (SizeT i = 0; i < 4; ++i) patch.Outer[i] = live.PatchDefaultOuterLevel[i];
            for (SizeT i = 0; i < 2; ++i) patch.Inner[i] = live.PatchDefaultInnerLevel[i];
            MGPipeApplySetPatchState(patch);
            return sizeof(MGPPatchState);
        }

        // set_vertex_attrib_defaults, behind D11's set-hash suppressor: the RESOLVED set - all
        // 32 values, all three views - is hashed on the client and the call does not go out
        // when the hash has not moved. That is coalescing rule 4, and this is its one wired
        // consumer in P2.
        Uint64 EmitVertexAttribDefaults(GLContext& ctx) {
            MGPipeTracker& tracker = MGPipeTrackerInstance();
            auto& staged = tracker.StagedAttribDefaults();
            constexpr SizeT kAttribs = MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS;
            static_assert(kAttribs <= 32, "MGPVertexAttribDefaults::Mask is a Uint32");

            Array<MG_State::GLState::CurrentVertexAttributeValue, kAttribs> resolved;
            for (SizeT i = 0; i < kAttribs; ++i) resolved[i] = ctx.GetCurrentVertexAttribute(static_cast<Uint>(i));

            const Uint64 contentHash = XXH64(resolved.data(), sizeof(resolved), 0);
            if (!MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetVertexAttribDefaults,
                                                             contentHash)) {
                return 0;
            }

            Array<MGPAttribValue, kAttribs> tail{};
            MGPVertexAttribDefaults header{};
            for (SizeT i = 0; i < kAttribs; ++i) {
                if (std::memcmp(&resolved[i], &staged[i], sizeof(resolved[i])) == 0) continue;
                MGPAttribValue& value = tail[header.Count];
                value.Location = static_cast<Uint32>(i);
                // ClassifyVertexAttribType resolves the float/int/uint view on the CLIENT
                // (MGPipeTypes.h); the frontend keeps all three populated, so the class the
                // shader input consumes is what decides which one is authoritative.
                value.ValueClass = 0;
                std::memcpy(value.Data, resolved[i].floatValue.data(), sizeof(value.Data));
                header.Mask |= Uint32{1} << static_cast<Uint32>(i);
                ++header.Count;
                staged[i] = resolved[i];
            }
            if (header.Count == 0) return 0;
            MGPipeApplySetVertexAttribDefaults(header, tail.data());
            return sizeof(MGPVertexAttribDefaults) + header.Count * sizeof(MGPAttribValue);
        }


        // set_residual_value_state (P2 brief D9, ARCHITECTURE.md 9.4).
        //
        // Since P2 the block is one Uint64 of capability bits, and every one of the 35 is
        // ALSO answerable from the assembled working block now that the contract closed the
        // FramebufferSrgb / DepthClamp / TextureCubeMapSeamless storage holes. That
        // redundancy is the whole point: the bits are read HERE from the frontend, and the
        // applier compares them against the assembled answer, so the day a later call takes
        // a capability over and forgets to carry it the block says so on the next draw
        // (Fatal{PipeResidualDiverged, "<Cap>"}).
        //
        // Building the carried bits from the ASSEMBLED block instead would make the trip
        // wire a tautology, which is exactly the failure P1's entry compare had and P2 is
        // paying to remove.
        //
        // Emitted once per context and again whenever the capability set may have moved,
        // which is whenever the pipeline version moved: every SET_CAPABILITY arm calls
        // BumpVersions, so that shutter cannot miss one.
        Uint64 EmitResidualValueState(GLContext& ctx) {
            ResidualValueBlock block{};
            constexpr SizeT kCapabilityCount = static_cast<SizeT>(CapabilityInput::CapabilityInputCount);
            static_assert(kCapabilityCount <= 64, "CapabilityBits is a Uint64");
            for (SizeT i = 0; i < kCapabilityCount; ++i) {
                if (ctx.IsCapabilityEnabled(static_cast<CapabilityInput>(i))) {
                    block.CapabilityBits |= Uint64{1} << i;
                }
            }
            MGPipeApplySetResidualValueState(block);
            if (MG_Util::PipeStats::Enabled()) {
                // ByteClass::ResidualValueBlock has been a placeholder that "stays at 0
                // until P2" since P0. This is what makes it non-zero.
                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::ResidualValueBlock,
                                             sizeof(ResidualValueBlock));
            }
            return sizeof(MGPResidualValueState) + sizeof(ResidualValueBlock);
        }

        // Set when the capability set may have moved, cleared when the block goes out. It is
        // not part of the tracker because it is emission state, not a shutter: the shutter
        // (the pipeline version) has already been consumed by the time this is read.
        Bool g_residualDue = true;

        constexpr Uint32 kAllDynamicChunks =
            static_cast<Uint32>((Uint64{1} << kMGPipeDynamicChunkCount) - 1);

        // create/bind_render_state and set_dynamic_state. Returns the bytes that went on the
        // wire, for the payload histogram.
        Uint64 EmitRenderState(GLContext& ctx, Uint32 dirty, Bool freshlyPrimed) {
            MGPipeTracker& tracker = MGPipeTrackerInstance();
            const RenderStateParameters& live = ctx.GetRenderStateParameters();
            const auto version = static_cast<Uint16>(ctx.GetRenderStateParametersVersion());
            const auto pipelineVersion = static_cast<Uint16>(ctx.GetPipelineStateVersion());
            Uint64 payloadBytes = 0;

            if (freshlyPrimed) {
                // A fresh context is a fresh server: the cache's handles name slots this
                // client's allocator is about to hand out again, so both sides start over
                // together rather than one of them remembering the other's objects.
                MGPipeCsoCacheInstance().Reset();
                MGPipeApplierReset();
            }

            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) {
                const MGPipeHandle cso = MGPipeCsoCacheInstance().Acquire(live, payloadBytes);
                MGPBindRenderState bind{};
                bind.Cso = cso;
                bind.Version = version;
                bind.PipelineVersion = pipelineVersion;
                MGPipeApplyBindRenderState(bind);
                payloadBytes += sizeof(MGPBindRenderState);
                if (MG_Util::PipeStats::Enabled()) {
                    MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::RenderStateCsoBinds, 1);
                }
            }

            if (dirty & MGPipeDirtyBit(MGPipeDirty::NewRenderState)) {
                // The chunk-level suppressor: only the dynamic chunks that differ from what
                // the server has. A glViewport sends chunk D0 and nothing else; a
                // glClearColor sends D2. An EMPTY mask still sends the 32-byte header,
                // because the VERSION is what Magma's dynamic tail gates on and it moved.
                const Uint32 chunkMask =
                    freshlyPrimed ? kAllDynamicChunks
                                  : MGPipeDynamicChunksThatMoved(live, tracker.Staged());
                Array<Uint8, kMGPipeDynamicChunkBytes> blob;
                const SizeT blobBytes = MGPipeDynamicChunkBlobBytes(chunkMask);
                MGPipeGatherDynamicChunks(live, chunkMask, blob.data());
                MGPDynamicState dyn{};
                dyn.ChunkMask = chunkMask;
                dyn.Version = version;
                dyn.Blob.Size = blobBytes;
                MGPipeApplySetDynamicState(dyn, blob.data());
                payloadBytes += sizeof(MGPDynamicState) + blobBytes;
            }

            if (dirty & (MGPipeDirtyBit(MGPipeDirty::NewPipelineState) |
                         MGPipeDirtyBit(MGPipeDirty::NewRenderState))) {
                tracker.Staged() = live;
            }
            return payloadBytes;
        }
    } // namespace

    // ---- the validate point (P2 brief D1) ----
    void MGPipeValidateForVerb(MGPipeVerb verb) {
        PipeInputs& inputs = gPipeInputs;
        ParsePoisonOmissionKnob();
#if MOBILEGL_PIPE_VERIFY
        ArmVerify();
#else
        // The runtime knob without the compiled comparator is a no-op that would look green;
        // this warning is what a lane's arming assertion turns into red.
        if (MG_Config::Features.PipeVerify) {
            MGLOG_W_ONCE("MGPipe: MOBILEGL_PIPE_VERIFY=1 requested but the comparator is not compiled in "
                         "(configure with -DMOBILEGL_PIPE_VERIFY=ON)");
        }
#endif
#if MOBILEGL_PIPE_POISON
        MGPipeFilledState& filled = MGPipeFillAccess::Filled(inputs);
        // Starts at 1: FilledGen == 0 is "never filled", and MGPipeInputFieldIsFresh refuses
        // it on both branches, so a read before this first bump is
        // Fatal{UnmigratedPipeInput, "<Field>@<none>"} rather than default storage.
        ++filled.CurrentVerbSerial;
#endif
        MGPipeFillAccess::SetVerb(inputs, verb);
        auto* ctx = LiveContext();
        MGPipeFillAccess::SetIdentity(inputs, ctx);
        if (ctx == nullptr) return;
        const MGPipeVerbClass verbClass = kMGPipeVerbClass[static_cast<SizeT>(verb)];
        const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(verbClass)];

        // ---- step 2: the dirty walk (P2 brief D1, D4) ----
        // The mask is computed, latched and counted here and nothing is emitted from it
        // yet: this commit is the safety net that says the walk is semantically free
        // before any field stops being pulled. The emission steps land on top of it.
        MGPipeTracker& tracker = MGPipeTrackerInstance();
        const Uint32 dirty = tracker.Update(*ctx, verbClass);

        // ---- step 3: emission ----
        const Uint64 pushMask = MG_Config::Features.PipePush;
        Uint64 payloadBytes = 0;
        if ((pushMask & kMGPipeSubsystemRenderState) != 0 &&
            (dirty & (MGPipeDirtyBit(MGPipeDirty::NewPipelineState) |
                      MGPipeDirtyBit(MGPipeDirty::NewRenderState))) != 0) {
            payloadBytes += EmitRenderState(*ctx, dirty, tracker.FreshlyPrimed());
        }
        if (tracker.FreshlyPrimed()) {
            // A fresh context: what the server has is no longer what any slot last emitted.
            MGPipeSetHashSuppressorInstance().InvalidateAll();
        }
        if ((pushMask & kMGPipeSubsystemPixelPack) != 0 &&
            (dirty & MGPipeDirtyBit(MGPipeDirty::NewPixelPack)) != 0) {
            payloadBytes += EmitPixelPackState(*ctx);
        }
        if ((pushMask & kMGPipeSubsystemPatchState) != 0 &&
            (dirty & MGPipeDirtyBit(MGPipeDirty::NewPatchState)) != 0) {
            payloadBytes += EmitPatchState(*ctx);
        }
        if ((pushMask & kMGPipeSubsystemVertexAttribDefaults) != 0 &&
            (dirty & MGPipeDirtyBit(MGPipeDirty::NewVertexAttribDefaults)) != 0) {
            payloadBytes += EmitVertexAttribDefaults(*ctx);
        }

        // ---- step 4: the residual fill, for what an emitted call did NOT supply ----
        const Bool applierDerives = ApplierDerivesRenderStateFields();
        for (SizeT i = 0; i < kMGPipeInputFieldCount; ++i) {
            const auto field = static_cast<MGPipeInputField>(i);
            if (!MGPipeFieldMaskHas(mask, field)) continue;
#if MOBILEGL_PIPE_POISON
            if (kMGPipeInputFieldSticky[i]) {
                // Stamped once by the first fill that sees a live context; fresh through the
                // Sticky -> FilledGen != 0 branch of MGPipeInputFieldIsFresh from then on.
                if (filled.FilledGen[i] == 0) filled.FilledGen[i] = 1;
                continue;
            }
#else
            if (kMGPipeInputFieldSticky[i]) continue;
#endif
            // A field a P2 call now supplies is not pulled again - that second pull is
            // exactly the cost P2 exists to remove. THE STAMP IS UNCHANGED either way: a
            // stamp says "this verb published this field", which is as true of an emitted
            // field as of a copied one, and withholding it would abort every backend read of
            // the very fields the migration just took over.
            const MGPipeFieldEmitter emitter = kMGPipeFieldEmittedBy[i];
            const Uint64 subsystem = SubsystemForEmitter(emitter);
            const Bool supplied = subsystem != 0 && (subsystem & kMGPipeWiredSubsystems) != 0 &&
                                  (pushMask & subsystem) != 0 &&
                                  EmittedCallSuppliesTheWholeField(field) &&
                                  (applierDerives || AppliedWithoutDerivation(field));
            if (!supplied) MGPipeFillAccess::CopyField(inputs, *ctx, field);
#if MOBILEGL_PIPE_POISON
            // The value is copied either way; only the stamp is withheld for the omitted pair.
            if (!IsOmitted(verb, field)) filled.FilledGen[i] = filled.CurrentVerbSerial;
#endif
        }
        // ---- step 4b: the residual value block, and it goes out HERE ----
        // Its trip wire compares the carried bits against the ASSEMBLED capability mirror,
        // and that mirror is written either by the applier's derivation or by the fill loop
        // above - so the block is only meaningful once step 4 has run. Emitting it with the
        // other calls would compare against the previous verb's answer.
        if ((pushMask & kMGPipeSubsystemResidualValues) != 0) {
            if (tracker.FreshlyPrimed() || (dirty & MGPipeDirtyBit(MGPipeDirty::NewPipelineState)) != 0) {
                g_residualDue = true;
            }
            // The trip wire compares against the ASSEMBLED capability mirror, so it can only
            // run at a verb whose class actually carries that mirror - IsCapabilityEnabled is
            // in seven of the nine class masks and kQuery and kXfbSpan do not read it, so at
            // those verbs the mirror is whatever the last verb that did read it left behind.
            // The change is HELD rather than dropped: dropping it would silently disarm the
            // wire for a capability that moved between two queries.
            if (g_residualDue && MGPipeFieldMaskHas(mask, MGPipeInputField::IsCapabilityEnabled)) {
                payloadBytes += EmitResidualValueState(*ctx);
                g_residualDue = false;
            }
        }
        if (payloadBytes != 0 && MG_Util::PipeStats::Enabled()) {
            // PipeStats::RecordDrawPayloadBytes has been implemented and unit-tested since
            // P0 and called by nothing; this is its first emitter, and the 24-bucket
            // histogram is what answers ROADMAP.md open question 4's chunk-granularity
            // retune with data instead of a guess.
            MG_Util::PipeStats::RecordDrawPayloadBytes(payloadBytes);
        }
#if MOBILEGL_PIPE_VERIFY
        EntryCompare(inputs, mask);
#endif
    }
} // namespace MobileGL::MG_Pipe
