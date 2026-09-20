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

#if MOBILEGL_BUILD_DISAGGREGATED
// P5c (rv, CONTRACT-P5C.md §5.3): the three texture shutters' server-side answer lives in the
// applier - MGPipeApplierTextureShutterSerial() / MGPipeApplierContextSerial(), declared here
// so the accessors below can answer with them under a server-stamped verb. MG_Pipe is below
// MG_Backend, so this direction is the layering's, and PipeApply.h forward-declares
// PipeInputs rather than including this header, so there is no cycle.
#include <MG_Pipe/PipeApply.h>
#endif

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
// TABLE 2 (CONTRACT-P5.md section 3, R-7): the four ownership classes, one per field, plus
// the seven sticky forwards' own rows. Included HERE rather than from MG_Pipe/MGPipe.h with
// gen_pipe.py's seven outputs, deliberately: MGPipe.h is in the PULL build's include closure
// and G1 admits no symbol motion there, while this header is reached only through
// PipeInputsSwitch.h's MOBILEGL_PIPE_PUSH arm. It is also exactly the header the poison check
// below and the server's verb stamp both already see.
#include <MG_Pipe/generated/PipeFieldOwnership.inc>

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

#if MOBILEGL_BUILD_DISAGGREGATED
    // ---- P5: the split arm of the read check (R-7.2, R-7.3) ------------------------------
    //
    // A stale read stops being one answer and becomes FOUR, keyed on the field's table-2 class
    // - which is what turns the generated table from a document into a runtime mechanism:
    //
    //   RECORD-SUPPLIED / APPLIER-DERIVED  the server could answer it and did not: a real
    //                                      defect. Fatal, exactly as today.
    //   BARRIER-PULLED                     the server is reading the value the client's
    //                                      residual fill left in gPipeInputs while the verb
    //                                      barrier holds both threads apart (R-1). LEGAL, and
    //                                      COUNTED: PipeStats::CallClass::ResidualPulls. Under
    //                                      MOBILEGL_IPC_STRICT_ERRORS=1 it is Fatal instead.
    //   FATAL                              no carrier and the reduced path never reads it.
    //
    // AND IT IS ARMED ONLY INSIDE A SERVER-STAMPED VERB (PipeInputs::ServerStampedVerb).
    // A split BUILD running monolith transport - which is every unit and integration-gpu lane
    // of build-split - has a client that fills and stamps all 63 fields at every verb, so a
    // stale read there is the same defect it is in a verify build and gets the same Fatal.
    // Without that condition the leniency would apply to lanes whose stamps are the client's,
    // and 1842 unit cases would quietly stop being able to go red.
    void MGPipeInputUnfreshRead(MGPipeInputField field, MGPipeVerb verb, Bool serverStamped);
    // The same decision for an accessor that takes an argument the table narrows on
    // (kMGPipeFieldArgumentOwnership). Called BEFORE the freshness test, because the narrowed
    // class is a statement about the argument rather than about the stamp: the pack half of
    // GetPixelStoreParameters is stamped and fresh while the unpack half has no carrier and no
    // backend reader at all.
    //
    // RETURNS TRUE WHEN THE ARGUMENT ROW DECIDED, and the caller then skips the field-level
    // check. Today the only row narrows to FATAL, which aborts, so the return value changes
    // nothing; the day a row narrows to BARRIER-PULLED it is what stops the field-level check
    // from either counting the same read twice or - worse, because the field's own class would
    // not be BARRIER-PULLED - aborting a read the argument row had just declared legal.
    Bool MGPipeInputArgumentRead(MGPipeInputField field, Uint32 arg0, MGPipeVerb verb, Bool serverStamped);
#endif

    // The read-side poison check, on every non-forwarded accessor. Under MOBILEGL_PIPE_POISON
    // a read of a field whose stamp is older than the current verb serial is
    // Fatal{UnmigratedPipeInput, "Field@Verb"}; otherwise the accessor is a plain load.
#if MOBILEGL_PIPE_POISON
#if MOBILEGL_BUILD_DISAGGREGATED
#define MGP_INPUT_CHECK(Field)                                                                                         \
    do {                                                                                                               \
        if ((m_serverStampedVerb && ::MobileGL::MG_Pipe::MGPipeFieldOwnershipOf(Field) ==                            \
                                      ::MobileGL::MG_Pipe::MGPipeFieldOwnership::kFatal) ||                           \
            !::MobileGL::MG_Pipe::MGPipeInputFieldIsFresh(m_filled, (Field))) {                                        \
            ::MobileGL::MG_Pipe::MGPipeInputUnfreshRead((Field), m_currentVerb, m_serverStampedVerb);                  \
        }                                                                                                              \
    } while (0)
#define MGP_INPUT_CHECK_ARG(Field, Arg0)                                                                               \
    do {                                                                                                               \
        if (!::MobileGL::MG_Pipe::MGPipeInputArgumentRead((Field), static_cast<Uint32>(Arg0), m_currentVerb,            \
                                                          m_serverStampedVerb)) {                                      \
            MGP_INPUT_CHECK(Field);                                                                                    \
        }                                                                                                              \
    } while (0)
#else
#define MGP_INPUT_CHECK(Field)                                                                                         \
    do {                                                                                                               \
        if (!::MobileGL::MG_Pipe::MGPipeInputFieldIsFresh(m_filled, (Field))) {                                        \
            ::MobileGL::MG_Pipe::MGPipeInputPoisonFatalForVerb((Field), m_currentVerb);                                \
        }                                                                                                              \
    } while (0)
#define MGP_INPUT_CHECK_ARG(Field, Arg0) MGP_INPUT_CHECK(Field)
#endif
#else
#define MGP_INPUT_CHECK(Field) ((void)0)
#define MGP_INPUT_CHECK_ARG(Field, Arg0) ((void)0)
#endif
    // The compare-at-read hook of the MOBILEGL_PIPE_VERIFY comparator (P1 brief D8), defined
    // in MG_Impl/Pipe/PipeFill.cpp: re-reads the field from the live context and compares it
    // against the stored value, and reports the FIRST divergence as
    // Fatal{PipeVerifyDiffer, "Field@Verb", verb=<serial>, where=read} (the indices go in a
    // preceding MGLOG_E). Only the live block (gPipeInputs) is verified; a snapshot's own
    // accessors are plain loads. Off in every other build.
    struct PipeInputs;
#if MOBILEGL_PIPE_VERIFY
    void MGPipeVerifyReadHook(const PipeInputs& self, MGPipeInputField field, Uint index0, Uint index1);
#define MGP_INPUT_VERIFY_READ(Field, Index0, Index1)                                                                   \
    ::MobileGL::MG_Pipe::MGPipeVerifyReadHook(*this, (Field), static_cast<Uint>(Index0), static_cast<Uint>(Index1))
#else
#define MGP_INPUT_VERIFY_READ(Field, Index0, Index1) ((void)0)
#endif

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
    // The forwarded set IS the sticky set (PipeFields.def marks the same seven rows F and
    // sticky), so an eighth sticky row without a forwarder is refused here, not by a test.
    inline constexpr SizeT kMGPipeForwardedFieldCount = 7;
    static_assert(kMGPipeForwardedFieldCount == kMGPipeInputStickyFieldCount,
                  "the forwarded (F-class) fields and the sticky fields of PipeFields.def are the same seven rows");

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
#if MOBILEGL_BUILD_DISAGGREGATED
        // TRUE between the server's verb-boundary stamp and whoever clears it. It is the
        // arming condition of the whole split read path: only inside a server-stamped verb is
        // a BARRIER-PULLED read counted rather than Fatal, and only there is a sticky forward
        // a residual pull rather than an ordinary monolith call. A split build running
        // monolith transport never sets it, which is why build-split's unit and integration
        // cases behave exactly as a verify build's do.
        //
        // CLEARING IT IS THE APPLIER'S JOB AND NOT THE CLIENT'S, even though the client also
        // does it. MGPipeValidateForVerb and MGPipeLeaveVerb both call
        // MGPipeServerClearVerbBoundary, which is sufficient for inproc, where both roles share
        // one process and one gPipeInputs - and misleading for P6, where MG_Impl is not in the
        // server at all. There this flag would latch TRUE for the life of the server after the
        // first stamp, every later read anywhere would be judged against the last verb's mask,
        // and MGPipeStickyForwardPull would stop being a no-op outside a verb - so
        // InvalidateCompileEnv reached from a later context's backend initialisation, the exact
        // case the sticky exemption was written for, would be counted and, under strict, would
        // abort. So: PipeApplier clears on leaving the applier. Not optional.
        Bool ServerStampedVerb() const { return m_serverStampedVerb; }
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
        // THE ONE FIELD TABLE 2 NARROWS BY ARGUMENT. m_pixelStore[2] is one array indexed by
        // this accessor's own argument, exactly as m_bufferBindingSlot[15] is indexed by a
        // BufferTarget, and Coverage.def:62-69 already rules that such a field stays ONE row.
        // Only [0] (pack) has a carrier - set_pixel_pack_state, which the applier writes
        // (PipeApply.cpp:1373) - so the field is APPLIER-DERIVED and the UNPACK half is FATAL:
        // every MGB_CTX->GetPixelStoreParameters site in the tree passes false
        // (DirectGLES.cpp:7924, :9399, :10893, :11272, Utils.cpp:2302,
        // VulkanRenderer.cpp:10980), and PipeFill.cpp's EmitPixelPackState says the same from
        // the other side: "nothing on the far side of the boundary reads unpack state".
        PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const {
            MGP_INPUT_CHECK_ARG(MGPipeInputField::GetPixelStoreParameters, isUnpack ? 1u : 0u);
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
        // THE THREE TEXTURE SHUTTERS (P5c rv, CONTRACT-P5C.md §5.3). Their FieldOwnership rows
        // have said "a shutter, not a value: the server answers from its own Serial" since P5;
        // rv is the edit that makes the accessor DO it. Under a SERVER-STAMPED verb the answer
        // is the applier's own serial (APPLIER_DERIVED): server-owned, monotone, moved by every
        // applied record that can move what the frontend generation guarded. Everywhere else -
        // monolith, a split build on monolith transport, any read outside a stamped verb - the
        // storage answer is kept, byte for byte (G1).
        Uint64 GetSamplingResolutionGeneration() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetSamplingResolutionGeneration);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetSamplingResolutionGeneration, 0, 0);
#if MOBILEGL_BUILD_DISAGGREGATED
            if (m_serverStampedVerb) return MGPipeApplierTextureShutterSerial();
#endif
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
#if MOBILEGL_BUILD_DISAGGREGATED
            // See GetSamplingResolutionGeneration: the server answers from its own Serial.
            if (m_serverStampedVerb) return MGPipeApplierTextureShutterSerial();
#endif
            return m_textureBindGeneration;
        }
        Uint64 GetTextureContextId() const {
            MGP_INPUT_CHECK(MGPipeInputField::GetTextureContextId);
            MGP_INPUT_VERIFY_READ(MGPipeInputField::GetTextureContextId, 0, 0);
#if MOBILEGL_BUILD_DISAGGREGATED
            // A context IDENTITY rather than a generation: stable within the served context,
            // moved by every MGPipeApplierReset - which is all the backends' per-context memo
            // keys ask of it.
            if (m_serverStampedVerb) return MGPipeApplierContextSerial();
#endif
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
        // They carry no MGP_INPUT_CHECK / MGP_INPUT_VERIFY_READ (the declared exception to
        // P1 brief D4's "every accessor body"): a forward is a live call, not a stored value,
        // and InvalidateCompileEnv is reached from backend initialisation before any verb has
        // filled, where a check would be Fatal{...@<none>} on every start. Their sticky stamp
        // is therefore consulted by no accessor; the tests pin it through
        // MGPipeInputFieldIsFresh directly.
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
        template <class Fn>
        static Bool VisitStorage(MGPipeInputField field, const PipeInputs& a, const PipeInputs& b, Fn&& fn) {
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
        // The one door into the storage from the client side (MG_Impl/Pipe/PipeFill.cpp):
        // the filler's per-field copies and stamps, and the verify snapshot.
        friend struct MGPipeFillAccess;
        // The other door, and the one that exists because of what this block IS after P2:
        // the server's working RenderStateParameters. MG_Pipe/PipeApply.cpp scatters
        // bind_render_state's and set_dynamic_state's chunks straight into m_renderState,
        // which is why DirectGLES' SyncRenderState is not one line changed. It deliberately
        // does NOT stamp the poison generations - a stamp says "the filler published this
        // for THIS verb", which is the walk's statement, not the applier's.
        friend struct MGPipeApplyAccess;
        // THE THIRD DOOR, and the one the split phase needed that neither of the two above
        // could be: the SERVER's verb-boundary stamp (PipeInputs.cpp). MGPipeApplyAccess
        // deliberately does not stamp - see its comment above - and MGPipeFillAccess lives in
        // MG_Impl, which is the role the server does not have. So the stamp gets a door of its
        // own rather than a relaxation of either existing one.
        friend struct MGPipeStampAccess;

        // ---- identity ----
        const void* m_contextIdentity = nullptr;
        Bool m_live = false;
        MGPipeVerb m_currentVerb = MGPipeVerb::kVerbCount;
#if MOBILEGL_PIPE_POISON
        MGPipeFilledState m_filled{};
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
        Bool m_serverStampedVerb = false;
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
    //
    // LEAK-AT-EXIT STORAGE, and it is the same rule Init.cpp and GlobalObjects.cpp state for
    // pGLContext and pActiveBackendObject: "a process that exits without eglTerminate simply
    // leaks the global singletons to the OS instead of running destructors during static
    // teardown". This block breaks that rule if it is a value, because its O-class members
    // are SharedPtrs to FRONTEND objects: a VertexArrayObject that the application deleted
    // while it was bound has its last reference here, and destroying this block from
    // __run_exit_handlers therefore runs ~VertexArrayObject -> ~BufferObject at exit. Those
    // destructors are not exit-safe and cannot be made so - they reach the client's slot
    // allocator, the resource tracker, the vertex-input emitter, the applier AND, through
    // MGPipeApplyResourceDestroy, the backend's own twin tables, deferred-release queue,
    // buffer pool and driver entry points, every one of which is either already destroyed or
    // about to be. So the reference is never dropped: nothing here can start such a chain.
    // A live context releases these SharedPtrs the ordinary way, at the fill point.
    // (P3a; the exit-time heap corruption this closes is p3a-results/exit-order-v1.md.)
    inline PipeInputs& gPipeInputs = *new PipeInputs();

#if MOBILEGL_BUILD_DISAGGREGATED
    // ============================================================================
    // P5f (f1), P5F-WIRE-COMPLETENESS.md §4: THE DUAL-BLOCK REHEARSAL
    // ============================================================================
    //
    // One gPipeInputs served both roles because the verb barrier (R-1) made "the client filled
    // it" and "the server is reading it" mutually exclusive in TIME on the same object. The
    // rehearsal removes the same-object half of that sentence: under
    // MOBILEGL_IPC_ROLE_SPLIT_STATE=1 the client's residual fill writes the CLIENT block below
    // and the backend/applier keep reading gPipeInputs, which is the SERVER block. Every path
    // that worked only because the two were one object - a BARRIER-PULLED field, a sticky
    // forward's read of the fill side's stamps - then has no value to read, and the read
    // becomes a NAMED Fatal{UnmigratedPipeInput, "<field>@<verb>"} (CountBarrierPull's
    // dual-block arm, PipeInputs.cpp) instead of a silent cross-role answer.
    //
    // THE READ SIDE'S SPELLING DOES NOT MOVE. gPipeInputs remains the server-role block, so the
    // 379 MGB_CTX sites and PipeApply.cpp's applier writes are untouched; the only new spelling
    // is on the fill side (MG_Impl/Pipe/PipeFill.cpp), which asks MGPipeClientInputs(). Under
    // monolith transport - every unit and integration-gpu lane of a split build - the selection
    // folds back to the single shared block and behaviour is byte-for-byte the old one.
    //
    // Same leak-at-exit storage as gPipeInputs above, for the same reason.
    inline PipeInputs& gPipeInputsClientBlock = *new PipeInputs();

    // PipeInputs.cpp. Whether the rehearsal is armed: the knob AND a real transport (and not
    // the verify build, which ConfigLoader forces off). Constant for the life of the process.
    Bool MGPipeRoleSplitActive();
    // PipeInputs.cpp. THE FILL SIDE'S ONE NEW SPELLING: the client block when the rehearsal is
    // armed, gPipeInputs otherwise. Everything in MG_Impl/Pipe/PipeFill.cpp that used to spell
    // gPipeInputs spells this instead.
    PipeInputs& MGPipeClientInputs();
    // PipeInputs.cpp. The client-role half of MGPipeServerClearVerbBoundary: clears the stamp
    // flag on the FILL side's block. With the rehearsal off that IS gPipeInputs, so the two
    // client call sites keep their old semantics exactly; with it on the client block's flag is
    // never raised (nothing server-stamps it) and the clear is a no-op - which is the point:
    // the client no longer reaches into the server's block at all.
    void MGPipeClientClearVerbBoundary();
    // PipeInputs.cpp. CONTRACT-P5E §3.2's other half: the server block's identity is
    // SERVER-OWNED. Called from PipeApplier::Attach and from MGPipeServerStampVerbBoundary;
    // with the rehearsal armed it sets m_live and points m_contextIdentity at a token derived
    // from MGPipeApplierContextSerial() - the server's own served-context clock, which is what
    // moves when the served context does. Without it the server block's ContextIdentity() would
    // stay nullptr, and DirectGLES' fb-slot memo cache compares identity FIRST (a nullptr
    // against its own nullptr initialiser reads as a hit and hands out a null slot): an
    // unnamed crash where the rehearsal exists to produce a named one. A no-op with the
    // rehearsal off, so the client's per-verb SetIdentity keeps owning the shared block there.
    void MGPipeServerBlockNoteIdentity();
    // P5f fs: owned by the server's control lifecycle, never inferred from a client
    // GLContext or resurrected by stamping a verb after teardown.
    void MGPipeServerSetContextLive(Bool live);
    Bool MGPipeServerContextIsLive();
#else
    // The push-without-transport build has one role and one block, so the fill side's spelling
    // folds onto gPipeInputs and PipeFill.cpp reads identically in both build flavours. An
    // inline that no caller in such a build ever has a reason to call twice - the disaggregated
    // arm above is the real one.
    inline PipeInputs& MGPipeClientInputs() { return gPipeInputs; }
#endif

    // Every field has storage or is forwarded, and nothing else.
#define MGP_INPUT_COUNT_ONE(Field, Member) +1
    static_assert(0 MGP_INPUT_STORAGE_LIST(MGP_INPUT_COUNT_ONE) + kMGPipeForwardedFieldCount == kMGPipeInputFieldCount,
                  "MGP_INPUT_STORAGE_LIST plus the seven forwarded fields is not the PipeInputs field set");
#undef MGP_INPUT_COUNT_ONE
    // The docs budget ~20 KB; the block is a few KB.
    static_assert(sizeof(PipeInputs) < 20 * 1024, "PipeInputs outgrew its budget");

#if MOBILEGL_BUILD_DISAGGREGATED
    // ============================================================================
    // P5: the server-side verb stamp, and the counter that sizes what it leaves behind
    // ============================================================================
    //
    // THE PREREQUISITE NOBODY ELSE OWNS (CONTRACT-P5.md section 3). Nothing stamps the poison
    // generations on the applier side today and that is deliberate (see MGPipeApplyAccess'
    // comment above: a stamp is the filler's statement, not the applier's). Under split the
    // filler is in the other role, so without this every FilledGen[] would stay 0,
    // MGPipeInputFieldIsFresh would answer false for EVERYTHING, and a purely server-side read
    // would abort on the first field inside SyncRenderState - before any interesting case.
    //
    // THE RULE, in three lines, and the third one is the load-bearing one:
    //
    //   1. bump CurrentVerbSerial and set the verb, so a Fatal names it instead of "<none>";
    //   2. stamp every RECORD-SUPPLIED and APPLIER-DERIVED field with the new serial - those
    //      are exactly the fields the records this verb carried can answer;
    //   3. ZERO every BARRIER-PULLED and FATAL field's stamp.
    //
    // (3) is what makes the instrumentation real. The client's residual fill stamps ALL 63
    // fields at its own verb boundary (PipeFill.cpp step 4), so without the zeroing every
    // field would read fresh on the server, `rsp` would be identically 0, and the gate would
    // be decoration - the precise "an inproc implementation proves nothing" failure R-2
    // exists to prevent. Zeroing also cancels the sticky exemption for free: generated/
    // PipeFilled.inc tests "never filled" BEFORE it tests sticky, so gen == 0 wins.
    //
    // The value a BARRIER-PULLED read then gets is still the client's residual fill's, and it
    // is still CORRECT - because the verb barrier (R-1) leaves exactly one of the two threads
    // runnable. That is the debt, not a bug; `rsp` is its size.
    //
    // v1 calls this from Server/PipeApplier::StampVerbBoundary. MGPipeVerbForWireOp maps the
    // record's op onto a verb and answers kVerbCount for an op that is not verb-shaped, which
    // is the case the applier must NOT stamp on: a set_dynamic_state between two draws is not
    // a new verb, and stamping there would retire the previous verb's answers early.
    //
    // TWO THINGS THE CALLER INHERITS AND SHOULD NOT REDISCOVER:
    //
    //   (a) The records BETWEEN two boundaries apply under the earlier boundary's stamp - its
    //       serial, its class mask and its verb NAME. That is correct today because every
    //       MGPipeApply* entry point touches gPipeInputs through MGPipeApplyAccess, which
    //       carries no MGP_INPUT_CHECK; the day one of them calls back into the backend, its
    //       reads will be judged against a verb they do not belong to.
    //   (b) The verb a draw record stamps is MGPipeVerb::DrawArrays for ALL TWENTY draw verbs.
    //       The class mask is right (FillPoints.def puts all twenty in kDraw) and the NAME in a
    //       Fatal is not: a glDrawElements that aborts will say "@DrawArrays". draw_vbo carries
    //       no verb id, so fixing it means either a field on the record or a second argument
    //       here; it is cosmetic for the verdict and misleading for the reader, and it belongs
    //       with whatever phase widens draw_vbo to the multi-draw family (P8).
    void MGPipeServerStampVerbBoundary(MGPipeVerb verb);
    // MANDATORY for the applier when it leaves the verb. The client's own MGPipeValidateForVerb
    // and MGPipeLeaveVerb call it too, which is enough for inproc and NOT enough for a spawned
    // server, where MG_Impl is not in the process - see ServerStampedVerb() above for what
    // latching TRUE would do to the sticky forwards.
    void MGPipeServerClearVerbBoundary();

    // `rsp`. Also published per frame through PipeStats::CallClass::ResidualPulls; this is the
    // raw count, which exists because PipeStats can be switched off and the exit gate may not
    // be. Its value at the end of P5 IS the size of the P6/P7/P8 debt.
    //
    // PLAIN, NOT ATOMIC, AND THAT IS THE SAME RULING TABLE 3 MAKES FOR gPipeInputS ITSELF
    // (CONTRACT-P5.md section 4): the verb barrier leaves at most one of {GL thread, apply
    // thread} runnable, so there is one writer at any instant. This counter, m_serverStampedVerb
    // and FilledGen[] all rest on that and on nothing else - so MOBILEGL_IPC_VERB_BARRIER=0,
    // R-1's negative control, is a data race on all three as well as the correctness failure it
    // is there to show. It is expected to be red; it is not expected to be meaningful.
    Uint64 MGPipeResidualPullCount();
    void MGPipeResetResidualPullCountForTesting();

    // The sticky forwards' hook, called from each of the seven bodies in
    // MG_Impl/Pipe/PipeFill.cpp. They carry no MGP_INPUT_CHECK at all - the declared exception
    // argued at the F-class block above - so freshness can never reach them and the exit gate
    // would be structurally blind on the seven fields that hand the server a raw frontend
    // object or write into the frontend. This is what puts them in `rsp` and, under
    // MOBILEGL_IPC_STRICT_ERRORS=1, makes them Fatal like any other BARRIER-PULLED row.
    void MGPipeStickyForwardPull(MGPipeInputField field);
#endif

#if MOBILEGL_PIPE_VERIFY
    // PipeInputs.cpp. Per-field equality for the entry compare (P1 brief D8): V by value
    // through G4's MGPipeFieldEqual (bitwise floats, field-wise structs), O by identity, F
    // always equal (no storage).
    Bool MGPipeInputsFieldEqual(MGPipeInputField field, const PipeInputs& a, const PipeInputs& b);
    // PipeInputs.cpp. The entry compare: every field in `mask` of the pushed block against the
    // snapshot, first differing field out. Exported from the shared library on purpose - the
    // retrace-verify CI job proves it swapped in a verify build by finding this symbol with
    // nm -D, so a "green" run against a library without the comparator cannot happen.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((visibility("default")))
#endif
    Bool MGPipeVerifyInputs(const PipeInputs& pushed, const PipeInputs& snapshot, const MGPipeFieldMask& mask,
                            MGPipeInputField* outField);
    // PipeInputs.cpp. Negative control A: perturbs one field's storage (flip a Bool, +1 a
    // scalar, ^0x5A the first byte of a struct, flip a pointer's low bits - never
    // dereferenced, the snapshot is only ever compared). Returns false for a forwarded field,
    // which has nothing to corrupt.
    Bool MGPipeApplyVerifyCorruption(PipeInputs& snapshot, MGPipeInputField field);
#endif
} // namespace MobileGL::MG_Pipe
