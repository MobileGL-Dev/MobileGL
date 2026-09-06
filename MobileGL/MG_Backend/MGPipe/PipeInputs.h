// MobileGL - MobileGL/MG_Backend/MGPipe/PipeInputs.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <MG_Pipe/MGPipe.h>
// The frontend types the accessors return. Allowed here: P13 keeps this include for the
// verify arm (ARCHITECTURE.md 9.5). This header spells no MG_State global - every read of
// the live context happens on the client side, in MG_Impl/Pipe/PipeFill.cpp.
#include <MG_State/GLState/Core.h>

// MOBILEGL_PIPE_POISON: the per-verb generation stamps and the read-side
// Fatal{UnmigratedPipeInput} check. Derived here, once. The repository's debug gate is
// MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG (Defines.h); the verify CI build is
// Release/INFO with MOBILEGL_BUILD_DISAGGREGATED=OFF, so the third arm is what arms the poison
// there without dragging MG_Remote in.
#if MOBILEGL_PIPE_PUSH && (MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED || \
                           MOBILEGL_PIPE_VERIFY)
#define MOBILEGL_PIPE_POISON 1
#else
#define MOBILEGL_PIPE_POISON 0
#endif

namespace MobileGL::MG_Pipe {
    // PipeInputs.cpp. The poison Fatal with the verb's name ("<none>" before the first
    // verb): MGLOG_F + std::abort(), live at every log level on purpose - this is not
    // MOBILEGL_ASSERT, which is inert in INFO builds.
    [[noreturn]] void MGPipeInputPoisonFatalForVerb(MGPipeInputField field, MGPipeVerb verb);
    // kMGPipeVerbNames[verb], or "<none>" for kVerbCount (no verb has been filled yet).
    const char* MGPipeVerbName(MGPipeVerb verb);
    // Name lookups for the runtime knobs (MOBILEGL_PIPE_VERIFY_CORRUPT names a field,
    // MOBILEGL_PIPE_POISON_OMIT a Verb:Field pair). Empty on an unknown name.
    Optional<MGPipeInputField> MGPipeFindInputField(const char* name);
    Optional<MGPipeVerb> MGPipeFindVerb(const char* name);

    // The read-side poison check, on every non-forwarded accessor. Under MOBILEGL_PIPE_POISON
    // a read of a field whose stamp is older than the current verb serial is
    // Fatal{UnmigratedPipeInput, "Field@Verb"}; otherwise the accessor is a plain load.
#if MOBILEGL_PIPE_POISON
#define MGP_INPUT_CHECK(Field)                                                                                         \
    do {                                                                                                               \
        if (!::MobileGL::MG_Pipe::MGPipeInputFieldIsFresh(m_filled, (Field))) {                                        \
            ::MobileGL::MG_Pipe::MGPipeInputPoisonFatalForVerb((Field), m_currentVerb);                                \
        }                                                                                                              \
    } while (0)
#else
#define MGP_INPUT_CHECK(Field) ((void)0)
#endif
    // The compare-at-read hook of the MOBILEGL_PIPE_VERIFY comparator (P1 brief D8): re-reads
    // the same accessor with the same indices from the live context and compares. Armed by
    // the comparator commit; until then every build's accessor is a load.
#define MGP_INPUT_VERIFY_READ(Field, Index0, Index1) ((void)0)

    // The V/O storage of every field that has storage, by field id. The seven F-class
    // (forwarded) fields have none. PipeInputs::VisitStorage dispatches on this list, which
    // is what keeps the comparator and the corruption injector one function each instead of
    // two sixty-way switches.
    // clang-format off
#define MGP_INPUT_STORAGE_LIST(X)                                                    \
    X(GetActiveTextureUnit,                        m_activeTextureUnit)              \
    X(GetBlendColor,                               m_blendColor)                     \
    X(GetBlendEquationIndexed,                     m_blendEquation)                  \
    X(GetBlendFuncIndexed,                         m_blendFunc)                      \
    X(GetBoundTransformFeedbackName,               m_boundTransformFeedbackName)     \
    X(GetBoundVertexArray,                         m_boundVertexArray)               \
    X(GetBufferBindingSlot,                        m_bufferBindingSlot)              \
    X(GetBufferBindingPoint,                       m_bufferBindingPointBase)         \
    X(GetTouchedBufferBindingPointCount,           m_touchedBindingPointCount)       \
    X(GetClampReadColor,                           m_clampReadColor)                 \
    X(GetClearColor,                               m_clearColor)                     \
    X(GetClearDepth,                               m_clearDepth)                     \
    X(GetClearStencil,                             m_clearStencil)                   \
    X(GetColorMaskIndexed,                         m_colorMask)                      \
    X(GetCullFaceMode,                             m_cullFaceMode)                   \
    X(GetCurrentVertexAttribute,                   m_currentVertexAttribute)         \
    X(GetDepthFunc,                                m_depthFunc)                      \
    X(GetDepthMask,                                m_depthMask)                      \
    X(GetDepthRangeIndexed,                        m_depthRange)                     \
    X(GetFramebufferBindingSlot,                   m_framebufferBindingSlot)         \
    X(GetImageTextureBinding,                      m_imageTextureBindingBase)        \
    X(GetLineWidth,                                m_lineWidth)                      \
    X(GetLogicOp,                                  m_logicOp)                        \
    X(GetMaxTouchedTextureUnit,                    m_maxTouchedTextureUnit)          \
    X(GetMinSampleShadingValue,                    m_minSampleShadingValue)          \
    X(GetPatchDefaultInnerLevel,                   m_patchDefaultInnerLevel)         \
    X(GetPatchDefaultOuterLevel,                   m_patchDefaultOuterLevel)         \
    X(GetPatchVertices,                            m_patchVertices)                  \
    X(GetPipelineStateVersion,                     m_pipelineStateVersion)           \
    X(GetPixelStoreParameters,                     m_pixelStore)                     \
    X(GetPolygonModeFront,                         m_polygonModeFront)               \
    X(GetPolygonOffsetFactor,                      m_polygonOffsetFactor)            \
    X(GetPolygonOffsetUnits,                       m_polygonOffsetUnits)             \
    X(GetPrimitiveRestartIndex,                    m_primitiveRestartIndex)          \
    X(GetProgramForDispatch,                       m_programForDispatch)             \
    X(GetProgramForDraw,                           m_programForDraw)                 \
    X(GetProvokingVertexMode,                      m_provokingVertexMode)            \
    X(GetRenderStateParameters,                    m_renderState)                    \
    X(GetRenderStateParametersVersion,             m_renderStateParametersVersion)   \
    X(GetSamplingResolutionGeneration,             m_samplingResolutionGeneration)   \
    X(GetScissorBox,                               m_scissorBox)                     \
    X(GetStencilState,                             m_stencil)                        \
    X(GetTextureBindGeneration,                    m_textureBindGeneration)          \
    X(GetTextureContextId,                         m_textureContextId)               \
    X(GetTextureUnitObject,                        m_textureUnitBase)                \
    X(GetTransformFeedbackCapturedVertices,        m_transformFeedbackCapturedVertices) \
    X(GetTransformFeedbackGeneration,              m_transformFeedbackGeneration)    \
    X(GetTransformFeedbackPausedPrimitiveCounter,  m_transformFeedbackPausedPrimitiveCounter) \
    X(GetTransformFeedbackProgram,                 m_transformFeedbackProgram)       \
    X(GetViewport,                                 m_viewport)                       \
    X(GetViewportIndexed,                          m_viewportIndexed)                \
    X(IsCapabilityEnabled,                         m_capability)                     \
    X(IsCapabilityEnabledIndexed,                  m_capabilityIndexed)              \
    X(IsTransformFeedbackActive,                   m_transformFeedbackActive)        \
    X(IsTransformFeedbackPaused,                   m_transformFeedbackPaused)        \
    X(GetBoundTransformFeedbackLifetimeId,         m_boundTransformFeedbackLifetimeId)
    // clang-format on

    // The seven F-class fields, for the arithmetic below and for the sticky table's proof.
    inline constexpr SizeT kMGPipeForwardedFieldCount = 7;

    // The block the backends read instead of GLContext (ARCHITECTURE.md 9.2 phase A, P1 brief
    // D4). One struct, three storage classes, and every accessor keeps the NAME, PARAMETERS
    // and RETURN TYPE of its GLContext counterpart (MG_State/GLState/Core.h) so the strangler
    // sed is type-neutral:
    //
    //   V (value)            copied out of GLContext at fill time by calling the same accessor;
    //                        no derivation logic is re-implemented here, which is what keeps the
    //                        copy semantically identical by construction.
    //   O (object reference) a SharedPtr copy, or a raw pointer to the live GLContext-owned
    //                        slot/array for the accessors that return a non-const reference into
    //                        the context. Identity is what phase C turns into a handle.
    //   F (forwarded)        argument-keyed lookups and reverse-channel calls, defined out of
    //                        line in MG_Impl/Pipe/PipeFill.cpp (the client side, where the live
    //                        context may be spelled). Sticky: stamped once by the first fill that
    //                        sees a live context.
    //
    // Every non-forwarded accessor is MGP_INPUT_CHECK (poison) -> MGP_INPUT_VERIFY_READ
    // (compare-at-read) -> the storage. Both macros expand to nothing when their switch is
    // off, so a plain MOBILEGL_PIPE_PUSH build's accessor is a load.
    struct PipeInputs {
        using GLContext = MG_State::GLState::GLContext;
        using BufferObject = MG_State::GLState::BufferObject;
        using BufferTarget = ::MobileGL::BufferTarget;
        using FramebufferObject = MG_State::GLState::FramebufferObject;
        using FramebufferTarget = ::MobileGL::FramebufferTarget;
        using VertexArrayObject = MG_State::GLState::VertexArrayObject;
        using ProgramObject = MG_State::GLState::ProgramObject;
        using ITextureObject = MG_State::GLState::ITextureObject;
        using TextureUnit = MG_State::GLState::TextureUnit;
        using ImageTextureBinding = MG_State::GLState::ImageTextureBinding;
        using CurrentVertexAttributeValue = MG_State::GLState::CurrentVertexAttributeValue;

        static constexpr SizeT kBufferTargetCount = static_cast<SizeT>(BufferTarget::BufferTargetCount);
        static constexpr SizeT kFramebufferTargetCount = static_cast<SizeT>(FramebufferTarget::FramebufferTargetCount);
        static constexpr SizeT kCapabilityCount = static_cast<SizeT>(CapabilityInput::CapabilityInputCount);
        static constexpr SizeT kMaxViewports = RenderStateParameters::MAX_VIEWPORTS;
        static constexpr SizeT kMaxVertexAttribs = VertexArrayObject::MAX_VERTEX_ATTRIBS;
        static constexpr SizeT kStencilFaceCount = static_cast<SizeT>(StencilFace::StencilFaceCount);

        // IsCapabilityEnabledIndexed's two indexed capabilities, the only ones GLContext keeps
        // indexed state for (RenderState::IsCapabilityEnabledIndexed).
        struct IndexedCapabilities {
            Bool Blend[kMGMaxDrawBuffers];
            Bool ScissorTest[kMaxViewports];
        };

        // ---- identity / liveness (not fields) ----
        // Whether a live GLContext exists. Forwarded (PipeFill.cpp): under push MGB_CTX_LIVE
        // must be true as soon as a context exists, fill or no fill, which is what today's
        // null-context guards test.
        Bool IsLive() const;
        // The live GLContext's address at the last fill; serves MGB_CTX_IDENTITY.
        const void* ContextIdentity() const { return m_contextIdentity; }
        // The verb of the last fill, kVerbCount before the first one.
        MGPipeVerb CurrentVerb() const { return m_currentVerb; }
#if MOBILEGL_PIPE_POISON
        const MGPipeFilledState& FilledState() const { return m_filled; }
#endif

        // ---- V: values ----
        Int GetActiveTextureUnit() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetActiveTextureUnit);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetActiveTextureUnit, 0, 0);
            return m_activeTextureUnit;
        }
        const FloatVec4& GetBlendColor() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetBlendColor);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBlendColor, 0, 0);
            return m_blendColor;
        }
        void GetBlendEquationIndexed(Uint index, BlendEquation& color, BlendEquation& alpha) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetBlendEquationIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBlendEquationIndexed, index, 0);
            if (index >= kMGMaxDrawBuffers) {
                MOBILEGL_ASSERT(false, "Blend equation index out of range: %u", index);
                return;
            }
            color = m_blendEquation[index][0];
            alpha = m_blendEquation[index][1];
        }
        void GetBlendFuncIndexed(Uint index, BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                 BlendFactor& dstAlpha) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetBlendFuncIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBlendFuncIndexed, index, 0);
            if (index >= kMGMaxDrawBuffers) {
                MOBILEGL_ASSERT(false, "Blend func index out of range: %u", index);
                return;
            }
            srcRGB = m_blendFunc[index][0];
            dstRGB = m_blendFunc[index][1];
            srcAlpha = m_blendFunc[index][2];
            dstAlpha = m_blendFunc[index][3];
        }
        // Dead field: filled, read by no backend since the D21 XFB counter-slot rekey; kept so
        // the vendored inventory row keeps its mapping (Coverage.def).
        Uint GetBoundTransformFeedbackName() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetBoundTransformFeedbackName);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBoundTransformFeedbackName, 0, 0);
            return m_boundTransformFeedbackName;
        }
        SizeT GetTouchedBufferBindingPointCount(BufferTarget target) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTouchedBufferBindingPointCount);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTouchedBufferBindingPointCount, static_cast<Uint>(target), 0);
            return m_touchedBindingPointCount[static_cast<SizeT>(target)];
        }
        GLenum GetClampReadColor() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetClampReadColor);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetClampReadColor, 0, 0);
            return m_clampReadColor;
        }
        const FloatVec4& GetClearColor() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetClearColor);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetClearColor, 0, 0);
            return m_clearColor;
        }
        Float GetClearDepth() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetClearDepth);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetClearDepth, 0, 0);
            return m_clearDepth;
        }
        Uint32 GetClearStencil() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetClearStencil);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetClearStencil, 0, 0);
            return m_clearStencil;
        }
        BoolVec4 GetColorMaskIndexed(Uint index) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetColorMaskIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetColorMaskIndexed, index, 0);
            return m_colorMask[index];
        }
        CullFaceMode GetCullFaceMode() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetCullFaceMode);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetCullFaceMode, 0, 0);
            return m_cullFaceMode;
        }
        const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint index) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetCurrentVertexAttribute);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetCurrentVertexAttribute, index, 0);
            if (index >= kMaxVertexAttribs) {
                static const CurrentVertexAttributeValue defaultValue{};
                MGLOG_E_ONCE("PipeInputs::GetCurrentVertexAttribute: index %u is out of range", index);
                return defaultValue;
            }
            return m_currentVertexAttribute[index];
        }
        DepthTestFunc GetDepthFunc() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetDepthFunc);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetDepthFunc, 0, 0);
            return m_depthFunc;
        }
        Bool GetDepthMask() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetDepthMask);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetDepthMask, 0, 0);
            return m_depthMask;
        }
        const FloatVec2& GetDepthRangeIndexed(Uint index) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetDepthRangeIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetDepthRangeIndexed, index, 0);
            if (index >= kMaxViewports) {
                MOBILEGL_ASSERT(false, "Depth range index out of range: %u", index);
                return m_depthRange[0];
            }
            return m_depthRange[index];
        }
        Float GetLineWidth() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetLineWidth);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetLineWidth, 0, 0);
            return m_lineWidth;
        }
        LogicOperation GetLogicOp() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetLogicOp);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetLogicOp, 0, 0);
            return m_logicOp;
        }
        Int GetMaxTouchedTextureUnit() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetMaxTouchedTextureUnit);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetMaxTouchedTextureUnit, 0, 0);
            return m_maxTouchedTextureUnit;
        }
        Float GetMinSampleShadingValue() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetMinSampleShadingValue);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetMinSampleShadingValue, 0, 0);
            return m_minSampleShadingValue;
        }
        const FloatVec2& GetPatchDefaultInnerLevel() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPatchDefaultInnerLevel);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPatchDefaultInnerLevel, 0, 0);
            return m_patchDefaultInnerLevel;
        }
        const FloatVec4& GetPatchDefaultOuterLevel() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPatchDefaultOuterLevel);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPatchDefaultOuterLevel, 0, 0);
            return m_patchDefaultOuterLevel;
        }
        Uint GetPatchVertices() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPatchVertices);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPatchVertices, 0, 0);
            return m_patchVertices;
        }
        Uint GetPipelineStateVersion() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPipelineStateVersion);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPipelineStateVersion, 0, 0);
            return m_pipelineStateVersion;
        }
        Uint GetRenderStateParametersVersion() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetRenderStateParametersVersion);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetRenderStateParametersVersion, 0, 0);
            return m_renderStateParametersVersion;
        }
        PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPixelStoreParameters);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPixelStoreParameters, isUnpack ? 1u : 0u, 0);
            return m_pixelStore[isUnpack ? 1 : 0];
        }
        GLenum GetPolygonModeFront() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPolygonModeFront);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPolygonModeFront, 0, 0);
            return m_polygonModeFront;
        }
        Float GetPolygonOffsetFactor() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPolygonOffsetFactor);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPolygonOffsetFactor, 0, 0);
            return m_polygonOffsetFactor;
        }
        Float GetPolygonOffsetUnits() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPolygonOffsetUnits);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPolygonOffsetUnits, 0, 0);
            return m_polygonOffsetUnits;
        }
        Uint32 GetPrimitiveRestartIndex() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetPrimitiveRestartIndex);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetPrimitiveRestartIndex, 0, 0);
            return m_primitiveRestartIndex;
        }
        ProvokingVertexMode GetProvokingVertexMode() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetProvokingVertexMode);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetProvokingVertexMode, 0, 0);
            return m_provokingVertexMode;
        }
        const RenderStateParameters& GetRenderStateParameters() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetRenderStateParameters);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetRenderStateParameters, 0, 0);
            return m_renderState;
        }
        Uint64 GetSamplingResolutionGeneration() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetSamplingResolutionGeneration);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetSamplingResolutionGeneration, 0, 0);
            return m_samplingResolutionGeneration;
        }
        const IntVec4& GetScissorBox() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetScissorBox);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetScissorBox, 0, 0);
            return m_scissorBox;
        }
        const StencilFaceState& GetStencilState(StencilFace face) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetStencilState);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetStencilState, static_cast<Uint>(face), 0);
            return m_stencil[face == StencilFace::Back ? 1 : 0];
        }
        Uint64 GetTextureBindGeneration() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTextureBindGeneration);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTextureBindGeneration, 0, 0);
            return m_textureBindGeneration;
        }
        Uint64 GetTextureContextId() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTextureContextId);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTextureContextId, 0, 0);
            return m_textureContextId;
        }
        Uint64 GetTransformFeedbackCapturedVertices() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTransformFeedbackCapturedVertices);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTransformFeedbackCapturedVertices, 0, 0);
            return m_transformFeedbackCapturedVertices;
        }
        Uint64 GetTransformFeedbackGeneration() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTransformFeedbackGeneration);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTransformFeedbackGeneration, 0, 0);
            return m_transformFeedbackGeneration;
        }
        Uint64 GetTransformFeedbackPausedPrimitiveCounter() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTransformFeedbackPausedPrimitiveCounter);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTransformFeedbackPausedPrimitiveCounter, 0, 0);
            return m_transformFeedbackPausedPrimitiveCounter;
        }
        Uint64 GetBoundTransformFeedbackLifetimeId() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetBoundTransformFeedbackLifetimeId);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBoundTransformFeedbackLifetimeId, 0, 0);
            return m_boundTransformFeedbackLifetimeId;
        }
        IntVec4 GetViewport() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetViewport);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetViewport, 0, 0);
            return m_viewport;
        }
        const FloatVec4& GetViewportIndexed(Uint index) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetViewportIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetViewportIndexed, index, 0);
            if (index >= kMaxViewports) {
                MOBILEGL_ASSERT(false, "Viewport index out of range: %u", index);
                return m_viewportIndexed[0];
            }
            return m_viewportIndexed[index];
        }
        Bool IsCapabilityEnabled(CapabilityInput cap) const {
            MGP_INPUT_CHECK(MGPipeInputField::IsCapabilityEnabled);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::IsCapabilityEnabled, static_cast<Uint>(cap), 0);
            const auto index = static_cast<SizeT>(cap);
            return index < kCapabilityCount ? m_capability[index] : false;
        }
        // Blend and ScissorTest are the only indexed capabilities GLContext keeps; no backend
        // asks for another (VulkanRenderer asks Blend). Any other cap is a read the fill cannot
        // have served: Fatal{UnmigratedPipeInput} naming the field and the verb, the cap in a
        // preceding MGLOG_E.
        Bool IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const {
            MGP_INPUT_CHECK(MGPipeInputField::IsCapabilityEnabledIndexed);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::IsCapabilityEnabledIndexed, static_cast<Uint>(cap), index);
            if (cap == CapabilityInput::Blend) {
                return index < kMGMaxDrawBuffers ? m_capabilityIndexed.Blend[index] : false;
            }
            if (cap == CapabilityInput::ScissorTest) {
                return index < kMaxViewports ? m_capabilityIndexed.ScissorTest[index] : false;
            }
            MGLOG_E("PipeInputs::IsCapabilityEnabledIndexed: no indexed storage for cap=%d (index=%u)",
                    static_cast<int>(cap), index);
            MGPipeInputPoisonFatalForVerb(MGPipeInputField::IsCapabilityEnabledIndexed, m_currentVerb);
        }
        Bool IsTransformFeedbackActive() const {
            MGP_INPUT_CHECK(MGPipeInputField::IsTransformFeedbackActive);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::IsTransformFeedbackActive, 0, 0);
            return m_transformFeedbackActive;
        }
        Bool IsTransformFeedbackPaused() const {
            MGP_INPUT_CHECK(MGPipeInputField::IsTransformFeedbackPaused);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::IsTransformFeedbackPaused, 0, 0);
            return m_transformFeedbackPaused;
        }

        // ---- O: object references ----
        const SharedPtr<VertexArrayObject>& GetBoundVertexArray() {
            MGP_INPUT_CHECK(MGPipeInputField::GetBoundVertexArray);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBoundVertexArray, 0, 0);
            return m_boundVertexArray;
        }
        // A target the fill left null (one outside GlobalBufferTargets / BufferBindPointTargets,
        // or a read before any fill) is a read the fill cannot have served: the poison Fatal,
        // the target in a preceding MGLOG_E.
        BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget target) {
            MGP_INPUT_CHECK(MGPipeInputField::GetBufferBindingSlot);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBufferBindingSlot, static_cast<Uint>(target), 0);
            const auto index = static_cast<SizeT>(target);
            if (index >= kBufferTargetCount || m_bufferBindingSlot[index] == nullptr) {
                MGLOG_E("PipeInputs::GetBufferBindingSlot: no slot for target=%d", static_cast<int>(target));
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetBufferBindingSlot, m_currentVerb);
            }
            return *m_bufferBindingSlot[index];
        }
        BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget target, Uint index) {
            MGP_INPUT_CHECK(MGPipeInputField::GetBufferBindingPoint);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetBufferBindingPoint, static_cast<Uint>(target), index);
            const auto targetIndex = static_cast<SizeT>(target);
            if (targetIndex >= kBufferTargetCount || m_bufferBindingPointBase[targetIndex] == nullptr) {
                MGLOG_E("PipeInputs::GetBufferBindingPoint: no binding points for target=%d (index=%u)",
                        static_cast<int>(target), index);
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetBufferBindingPoint, m_currentVerb);
            }
            // The live storage is Array<Array<BindingSlotRange1D, BufferBindingPointCount>, N>
            // (BufferState.h), so base[index] is the live slot GLContext would hand out.
            return m_bufferBindingPointBase[targetIndex][index];
        }
        BindingSlot<FramebufferObject>& GetFramebufferBindingSlot(FramebufferTarget target) {
            MGP_INPUT_CHECK(MGPipeInputField::GetFramebufferBindingSlot);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetFramebufferBindingSlot, static_cast<Uint>(target), 0);
            const auto index = static_cast<SizeT>(target);
            if (index >= kFramebufferTargetCount || m_framebufferBindingSlot[index] == nullptr) {
                MGLOG_E("PipeInputs::GetFramebufferBindingSlot: no slot for target=%d", static_cast<int>(target));
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetFramebufferBindingSlot, m_currentVerb);
            }
            return *m_framebufferBindingSlot[index];
        }
        ImageTextureBinding& GetImageTextureBinding(Int unit) {
            MGP_INPUT_CHECK(MGPipeInputField::GetImageTextureBinding);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetImageTextureBinding, static_cast<Uint>(unit), 0);
            if (m_imageTextureBindingBase == nullptr) {
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetImageTextureBinding, m_currentVerb);
            }
            return m_imageTextureBindingBase[unit];
        }
        const ImageTextureBinding& GetImageTextureBinding(Int unit) const {
            MGP_INPUT_CHECK(MGPipeInputField::GetImageTextureBinding);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetImageTextureBinding, static_cast<Uint>(unit), 0);
            if (m_imageTextureBindingBase == nullptr) {
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetImageTextureBinding, m_currentVerb);
            }
            return m_imageTextureBindingBase[unit];
        }
        const SharedPtr<ProgramObject>& GetProgramForDispatch() {
            MGP_INPUT_CHECK(MGPipeInputField::GetProgramForDispatch);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetProgramForDispatch, 0, 0);
            return m_programForDispatch;
        }
        const SharedPtr<ProgramObject>& GetProgramForDraw() {
            MGP_INPUT_CHECK(MGPipeInputField::GetProgramForDraw);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetProgramForDraw, 0, 0);
            return m_programForDraw;
        }
        const SharedPtr<ProgramObject>& GetTransformFeedbackProgram() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTransformFeedbackProgram);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTransformFeedbackProgram, 0, 0);
            return m_transformFeedbackProgram;
        }
        TextureUnit& GetTextureUnitObject(Int unit) {
            MGP_INPUT_CHECK(MGPipeInputField::GetTextureUnitObject);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTextureUnitObject, static_cast<Uint>(unit), 0);
            if (m_textureUnitBase == nullptr) {
                MGPipeInputPoisonFatalForVerb(MGPipeInputField::GetTextureUnitObject, m_currentVerb);
            }
            return m_textureUnitBase[unit];
        }

        // ---- F: forwarded to the live context (MG_Impl/Pipe/PipeFill.cpp); sticky ----
        // Each takes an argument that is not verb state - a GL name, a lifetime id, a target -
        // i.e. it is a lookup or a reverse-channel write, not a state read; there is no value
        // the filler could copy and no verb whose fill could make it stale. Phase C replaces
        // them with handle tables and callbacks.
        SizeT GetBufferBindingPointCount(BufferTarget target) const;
        const SharedPtr<ProgramObject>& GetProgramObject(Uint index);
        const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
        Bool HasOpenTransformFeedbackSpan(Uint64 lifetimeId) const;
        void InvalidateCompileEnv();
        Bool ValidateProgramName(Uint index) const;
        // Dropped with an MGLOG_E_ONCE when no context is live; today's guarded sites never
        // reach it without one.
        void RecordError(ErrorCode code, UniquePtr<ErrorInfo> info);

        // ---- the storage visitor ----
        // Calls fn(a.<member>, b.<member>) for the field's storage and returns its result; returns
        // false without calling fn for a forwarded field, which has none. The comparator's
        // per-field equality and the verify corruption injector are both one call of this.
        template <class Fn>
        static Bool VisitStorage(MGPipeInputField field, PipeInputs& a, PipeInputs& b, Fn&& fn) {
            switch (field) {
#define MGP_INPUT_VISIT(Field, Member)                                                                                 \
    case MGPipeInputField::Field:                                                                                      \
        return fn(a.Member, b.Member);
                MGP_INPUT_STORAGE_LIST(MGP_INPUT_VISIT)
#undef MGP_INPUT_VISIT
            default:
                return false;
            }
        }

    private:
        friend void MGPipeFillForVerb(MGPipeVerb verb);
        friend void SnapshotFromGLContext(PipeInputs& snapshot, const MGPipeFieldMask& mask);

        // ---- identity ----
        const void* m_contextIdentity = nullptr;
        Bool m_live = false;
        MGPipeVerb m_currentVerb = MGPipeVerb::kVerbCount;
#if MOBILEGL_PIPE_POISON
        MGPipeFilledState m_filled{};
#endif

        // ---- V ----
        Int m_activeTextureUnit = 0;
        FloatVec4 m_blendColor{};
        BlendEquation m_blendEquation[kMGMaxDrawBuffers][2]{};
        BlendFactor m_blendFunc[kMGMaxDrawBuffers][4]{};
        Uint m_boundTransformFeedbackName = 0;
        SizeT m_touchedBindingPointCount[kBufferTargetCount]{};
        GLenum m_clampReadColor = 0;
        FloatVec4 m_clearColor{};
        Float m_clearDepth = 0.f;
        Uint32 m_clearStencil = 0;
        BoolVec4 m_colorMask[kMGMaxDrawBuffers]{};
        CullFaceMode m_cullFaceMode{};
        CurrentVertexAttributeValue m_currentVertexAttribute[kMaxVertexAttribs]{};
        DepthTestFunc m_depthFunc{};
        Bool m_depthMask = false;
        FloatVec2 m_depthRange[kMaxViewports]{};
        Float m_lineWidth = 0.f;
        LogicOperation m_logicOp{};
        Int m_maxTouchedTextureUnit = -1;
        Float m_minSampleShadingValue = 0.f;
        FloatVec2 m_patchDefaultInnerLevel{};
        FloatVec4 m_patchDefaultOuterLevel{};
        Uint m_patchVertices = 0;
        Uint m_pipelineStateVersion = 0;
        Uint m_renderStateParametersVersion = 0;
        PixelStoreParameters m_pixelStore[2]{}; // [0] = pack, [1] = unpack
        GLenum m_polygonModeFront = 0;
        Float m_polygonOffsetFactor = 0.f;
        Float m_polygonOffsetUnits = 0.f;
        Uint32 m_primitiveRestartIndex = 0;
        ProvokingVertexMode m_provokingVertexMode{};
        RenderStateParameters m_renderState{};
        Uint64 m_samplingResolutionGeneration = 0;
        Uint64 m_textureBindGeneration = 0;
        Uint64 m_textureContextId = 0;
        IntVec4 m_scissorBox{};
        StencilFaceState m_stencil[kStencilFaceCount]{};
        Uint64 m_transformFeedbackCapturedVertices = 0;
        Uint64 m_transformFeedbackGeneration = 0;
        Uint64 m_transformFeedbackPausedPrimitiveCounter = 0;
        Uint64 m_boundTransformFeedbackLifetimeId = 0;
        IntVec4 m_viewport{};
        FloatVec4 m_viewportIndexed[kMaxViewports]{};
        Bool m_capability[kCapabilityCount]{};
        IndexedCapabilities m_capabilityIndexed{};
        Bool m_transformFeedbackActive = false;
        Bool m_transformFeedbackPaused = false;

        // ---- O ----
        SharedPtr<VertexArrayObject> m_boundVertexArray;
        BindingSlot<BufferObject>* m_bufferBindingSlot[kBufferTargetCount]{};
        BindingSlotRange1D<BufferObject>* m_bufferBindingPointBase[kBufferTargetCount]{};
        BindingSlot<FramebufferObject>* m_framebufferBindingSlot[kFramebufferTargetCount]{};
        ImageTextureBinding* m_imageTextureBindingBase = nullptr;
        SharedPtr<ProgramObject> m_programForDispatch;
        SharedPtr<ProgramObject> m_programForDraw;
        SharedPtr<ProgramObject> m_transformFeedbackProgram;
        TextureUnit* m_textureUnitBase = nullptr;
    };

    // The single global the backends read through MGB_CTX (ARCHITECTURE.md 9.2). An inline
    // variable: no .cpp is needed for the definition.
    inline PipeInputs gPipeInputs{};

    // Every field has storage or is forwarded, and nothing else.
#define MGP_INPUT_COUNT_ONE(Field, Member) +1
    static_assert(0 MGP_INPUT_STORAGE_LIST(MGP_INPUT_COUNT_ONE) + kMGPipeForwardedFieldCount == kMGPipeInputFieldCount,
                  "MGP_INPUT_STORAGE_LIST plus the seven forwarded fields is not the PipeInputs field set");
#undef MGP_INPUT_COUNT_ONE
    // The docs budget ~20 KB; the block is a few KB.
    static_assert(sizeof(PipeInputs) < 20 * 1024, "PipeInputs outgrew its budget");

#if MOBILEGL_PIPE_VERIFY
    // PipeInputs.cpp. Per-field equality for the entry compare (P1 brief D8): V by value
    // through G4's MGPipeFieldEqual (bitwise floats, field-wise structs), O by identity, F
    // always equal (no storage).
    Bool MGPipeInputsFieldEqual(MGPipeInputField field, PipeInputs& a, PipeInputs& b);
    // PipeInputs.cpp. Negative control A: perturbs one field's storage (flip a Bool, +1 a
    // scalar, ^0x5A the first byte of a struct, null a pointer). Returns false for a forwarded
    // field, which has nothing to corrupt.
    Bool MGPipeApplyVerifyCorruption(PipeInputs& snapshot, MGPipeInputField field);
#endif
} // namespace MobileGL::MG_Pipe
