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
#include <MG_Impl/Pipe/PipeFill.h>
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
                // Every global target has a slot; the others (Index) stay null and a read
                // of one is the poison Fatal in the accessor.
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

        void ParsePoisonOmissionKnob() {
            if (g_omissionKnobParsed) return;
            g_omissionKnobParsed = true;
            const String& knob = MG_Config::Features.PipePoisonOmit;
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
    } // namespace

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

    // ---- the filler ----
    void MGPipeFillForVerb(MGPipeVerb verb) {
        PipeInputs& inputs = gPipeInputs;
        ParsePoisonOmissionKnob();
#if MOBILEGL_PIPE_POISON
        MGPipeFilledState& filled = MGPipeFillAccess::Filled(inputs);
        // Starts at 1, so FilledGen == 0 means "never filled".
        ++filled.CurrentVerbSerial;
#endif
        MGPipeFillAccess::SetVerb(inputs, verb);
        auto* ctx = LiveContext();
        MGPipeFillAccess::SetIdentity(inputs, ctx);
        if (ctx == nullptr) return;
        const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(kMGPipeVerbClass[static_cast<SizeT>(verb)])];
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
            MGPipeFillAccess::CopyField(inputs, *ctx, field);
#if MOBILEGL_PIPE_POISON
            // The value is copied either way; only the stamp is withheld for the omitted pair.
            if (!IsOmitted(verb, field)) filled.FilledGen[i] = filled.CurrentVerbSerial;
#endif
        }
    }
} // namespace MobileGL::MG_Pipe
