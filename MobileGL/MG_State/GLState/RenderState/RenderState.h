// MobileGL - MobileGL/MG_State/GLState/RenderState/RenderState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>

namespace MobileGL {
    enum class BlendFactor {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha,
        ConstantColor,
        OneMinusConstantColor,
        ConstantAlpha,
        OneMinusConstantAlpha,
        // Dual-source blend factors (GL_SRC1_*, glBindFragDataLocationIndexed); require the
        // dualSrcBlend device feature.
        Src1Color,
        OneMinusSrc1Color,
        Src1Alpha,
        OneMinusSrc1Alpha,
        BlendFactorCount,
        Unknown = -1
    };

    enum class BlendEquation {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max,
        BlendEquationCount,
        Unknown = -1
    };

    enum class LogicOperation {
        Clear,
        And,
        AndReverse,
        Copy,
        AndInverted,
        Noop,
        Xor,
        Or,
        Nor,
        Equiv,
        Invert,
        OrReverse,
        CopyInverted,
        OrInverted,
        Nand,
        Set,
        LogicOperationCount,
        Unknown = -1
    };

    enum class DepthTestFunc {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
        DepthTestFuncCount,
        Unknown = -1
    };

    enum class StencilOperation {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap,
        StencilOperationCount,
        Unknown = -1
    };

    enum class StencilFace {
        Front,
        Back,
        StencilFaceCount,
        Unknown = -1
    };

    enum class PixelStoreParam {
        // Pack Parameters
        PackAlignment,
        PackRowLength,
        PackImageHeight,
        PackSkipRows,
        PackSkipPixels,
        PackSkipImages,
        PackSwapBytes,
        PackLSBFirst,

        // Unpack Parameters
        UnpackAlignment,
        UnpackRowLength,
        UnpackImageHeight,
        UnpackSkipRows,
        UnpackSkipPixels,
        UnpackSkipImages,
        UnpackSwapBytes,
        UnpackLSBFirst,

        PixelStoreParamCount,
        Unknown = -1
    };

    enum class CullFaceMode {
        Front,
        Back,
        FrontAndBack,
        CullFaceModeCount,
        Unknown = -1
    };

    enum class FrontFaceMode {
        CounterClockwise,
        Clockwise,
        FrontFaceModeCount,
        Unknown = -1
    };

    enum class ProvokingVertexMode {
        FirstVertex,
        LastVertex,
        ProvokingVertexModeCount,
        Unknown = -1
    };

    enum class CapabilityInput {
        Blend,
        ClipDistance0,
        ClipDistance1,
        ClipDistance2,
        ClipDistance3,
        ClipDistance4,
        ClipDistance5,
        ClipDistance6,
        ClipDistance7,
        ColorLogicOp,
        CullFace,
        DebugOutput,
        DebugOutputSynchronous,
        DepthClamp,
        DepthTest,
        Dither,
        FramebufferSrgb,
        LineSmooth,
        Multisample,
        PolygonOffsetFill,
        PolygonOffsetLine,
        PolygonOffsetPoint,
        PolygonSmooth,
        PrimitiveRestart,
        PrimitiveRestartFixedIndex,
        RasterizerDiscard,
        SampleAlphaToCoverage,
        SampleAlphaToOne,
        SampleCoverage,
        SampleShading,
        SampleMask,
        ScissorTest,
        StencilTest,
        TextureCubeMapSeamless,
        ProgramPointSize,
        CapabilityInputCount,
        Unknown = -1
    };

    struct PixelStoreParameters {
        Bool SwapBytes = false;
        Bool LSBFirst = false;
        Int RowLength = 0;
        Int ImageHeight = 0;
        Int SkipPixels = 0;
        Int SkipRows = 0;
        Int SkipImages = 0;
        Int Alignment = 4;
    };

    struct PerBufferBlendState {
        Bool Enabled = false;
        BlendFactor SrcFactorRGB = BlendFactor::One;
        BlendFactor DstFactorRGB = BlendFactor::Zero;
        BlendFactor SrcFactorAlpha = BlendFactor::One;
        BlendFactor DstFactorAlpha = BlendFactor::Zero;
        BlendEquation ColorEquation = BlendEquation::Add;
        BlendEquation AlphaEquation = BlendEquation::Add;
    };

    struct StencilFaceState {
        DepthTestFunc Func = DepthTestFunc::Always;
        Int Ref = 0;
        Uint32 ValueMask = 0xffffffffu;
        Uint32 WriteMask = 0xffffffffu;
        StencilOperation FailOp = StencilOperation::Keep;
        StencilOperation PassDepthFailOp = StencilOperation::Keep;
        StencilOperation PassDepthPassOp = StencilOperation::Keep;
    };

    struct RenderStateParameters {
        // Rasterization
        IntVec4 Viewport = IntVec4(0, 0, 0, 0); // x, y, width, height
        Float LineWidth = 1.0f;
        Float PointSize = 1.0f;
        // GL_PATCH_VERTICES: how many vertices one tessellation patch consumes.
        Uint PatchVertices = 3;
        Float PolygonOffsetFactor = 0.0f;
        Float PolygonOffsetUnits = 0.0f;

        // Blending
        Array<PerBufferBlendState, MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS> BlendStates;
        LogicOperation LogicOp = LogicOperation::Copy;

        // Depth
        Bool DepthTestEnabled = false;
        DepthTestFunc DepthFunc = DepthTestFunc::Less;
        Bool DepthMask = true;

        // Color Mask. Per-draw-buffer state (glColorMaski); glColorMask broadcasts to all buffers.
        // Every entry is initialized to all-true in RenderState's constructor.
        Array<BoolVec4, MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS> ColorMasks;

        // Clear State
        FloatVec4 ClearColor = FloatVec4(0.0f, 0.0f, 0.0f, 1.0f);
        Float ClearDepth = 1.0f;
        Uint32 ClearStencil = 0;
        FloatVec4 BlendColor = FloatVec4(0.0f, 0.0f, 0.0f, 0.0f);
        FloatVec2 DepthRange = FloatVec2(0.0f, 1.0f);
        Float SampleCoverageValue = 1.0f;
        Bool SampleCoverageInvert = false;
        Uint32 SampleMaskValue = 0xffffffffu;
        Array<StencilFaceState, 2> StencilStates{};

        // Cull Face
        Bool CullFaceEnabled = false;
        CullFaceMode CullFaceModeSetting = CullFaceMode::Back;
        FrontFaceMode FrontFaceModeSetting = FrontFaceMode::CounterClockwise;
        ProvokingVertexMode ProvokingVertexModeSetting = ProvokingVertexMode::LastVertex;

        // Hints (glHint). All GL 3.3 core hint targets default to GL_DONT_CARE.
        GLenum LineSmoothHint = GL_DONT_CARE;
        GLenum PolygonSmoothHint = GL_DONT_CARE;
        GLenum TextureCompressionHint = GL_DONT_CARE;
        GLenum FragmentShaderDerivativeHint = GL_DONT_CARE;

        // Point parameters (glPointParameter). Only the two GL 3.3 core pnames.
        Float PointFadeThresholdSize = 1.0f;
        GLenum PointSpriteCoordOrigin = GL_UPPER_LEFT;

        // Color clamping (glClampColor). Core profile exposes only GL_CLAMP_READ_COLOR.
        GLenum ClampReadColor = GL_FIXED_ONLY;

        // Polygon rasterization mode (glPolygonMode). Core profile sets front and back together,
        // but GL_POLYGON_MODE still reports both slots, so keep them separate for a faithful query.
        GLenum PolygonModeFront = GL_FILL;
        GLenum PolygonModeBack = GL_FILL;

        // Primitive restart index (glPrimitiveRestartIndex); consumed when GL_PRIMITIVE_RESTART is
        // enabled during an indexed draw. Default 0.
        Uint32 PrimitiveRestartIndex = 0;

        // Scissor
        Bool ColorLogicOpEnabled = false;
        Bool DebugOutputEnabled = false;
        Bool DebugOutputSynchronousEnabled = false;
        Bool DitherEnabled = true;
        Bool LineSmoothEnabled = false;
        Bool MultisampleEnabled = true;
        Bool PolygonOffsetFillEnabled = false;
        Bool PolygonOffsetLineEnabled = false;
        Bool PolygonOffsetPointEnabled = false;
        Bool PolygonSmoothEnabled = false;
        Bool PrimitiveRestartEnabled = false;
        Bool PrimitiveRestartFixedIndexEnabled = false;
        Bool RasterizerDiscardEnabled = false;
        Bool SampleAlphaToCoverageEnabled = false;
        Bool SampleAlphaToOneEnabled = false;
        Bool SampleCoverageEnabled = false;
        Bool SampleMaskEnabled = false;
        Bool ScissorTestEnabled = false;
        Bool StencilTestEnabled = false;
        Bool ProgramPointSizeEnabled = false;
        IntVec4 ScissorBox = IntVec4(0, 0, 0, 0); // x, y, width, height
    };

    namespace MG_State {
        namespace GLState {
            class RenderState {
            public:
                RenderState();

                Uint GetVersion() const;
                const RenderStateParameters& GetAllParameters() const;

                // Rasterization
                void SetViewport(IntVec4 viewport); // x, y, width, height
                const IntVec4& GetViewport() const; // x, y, width, height
                void SetLineWidth(Float width);
                Float GetLineWidth() const;
                void SetPointSize(Float size);
                Float GetPointSize() const;
                void SetPatchVertices(Uint vertices);
                Uint GetPatchVertices() const;
                void SetPolygonOffset(Float factor, Float units);
                Float GetPolygonOffsetFactor() const;
                Float GetPolygonOffsetUnits() const;
                // Hints. target must be one of the 4 GL 3.3 core hint targets (validated by the caller).
                void SetHint(GLenum target, GLenum mode);
                GLenum GetHint(GLenum target) const;
                void SetPointFadeThresholdSize(Float size);
                Float GetPointFadeThresholdSize() const;
                void SetPointSpriteCoordOrigin(GLenum origin);
                GLenum GetPointSpriteCoordOrigin() const;
                // Color clamping (glClampColor). Core profile has only GL_CLAMP_READ_COLOR.
                void SetClampReadColor(GLenum clamp);
                GLenum GetClampReadColor() const;
                // Polygon mode (glPolygonMode). Core sets both faces together; the query reports both.
                void SetPolygonMode(GLenum front, GLenum back);
                GLenum GetPolygonModeFront() const;
                GLenum GetPolygonModeBack() const;
                void SetPrimitiveRestartIndex(Uint32 index);
                Uint32 GetPrimitiveRestartIndex() const;

                // Capabilities
                void SetCapability(CapabilityInput cap, Bool enabled);
                Bool IsCapabilityEnabled(CapabilityInput cap) const;
                void SetCapabilityIndexed(CapabilityInput cap, Uint index, Bool enabled);
                Bool IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const;

                // Blending
                void SetBlendFunc(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha, BlendFactor dstAlpha);
                void GetBlendFunc(BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                  BlendFactor& dstAlpha) const;
                void SetBlendFuncIndexed(Uint index, BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha,
                                         BlendFactor dstAlpha);
                void GetBlendFuncIndexed(Uint index, BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                         BlendFactor& dstAlpha) const;
                void SetBlendEquation(BlendEquation color, BlendEquation alpha);
                void GetBlendEquation(BlendEquation& color, BlendEquation& alpha) const;
                void SetBlendEquationIndexed(Uint index, BlendEquation color, BlendEquation alpha);
                void GetBlendEquationIndexed(Uint index, BlendEquation& color, BlendEquation& alpha) const;
                void SetLogicOp(LogicOperation logicOp);
                LogicOperation GetLogicOp() const;

                // Depth
                void SetDepthFunc(DepthTestFunc func);
                DepthTestFunc GetDepthFunc() const;
                void SetDepthMask(Bool flag);
                Bool GetDepthMask() const;
                void SetStencilFunc(StencilFace face, DepthTestFunc func, Int ref, Uint32 mask);
                void SetStencilMask(StencilFace face, Uint32 mask);
                void SetStencilOp(StencilFace face, StencilOperation fail, StencilOperation depthFail,
                                  StencilOperation depthPass);
                const StencilFaceState& GetStencilState(StencilFace face) const;

                // Color Mask. SetColorMask broadcasts to every draw buffer and GetColorMask returns
                // draw buffer 0; the indexed forms address a single draw buffer (glColorMaski). The
                // caller is responsible for validating index against MAX_DRAW_BUFFERS.
                void SetColorMask(BoolVec4 mask);
                BoolVec4 GetColorMask() const;
                void SetColorMaskIndexed(Uint index, BoolVec4 mask);
                BoolVec4 GetColorMaskIndexed(Uint index) const;

                // Clear State
                void SetClearColor(FloatVec4 color);
                const FloatVec4& GetClearColor() const;
                void SetClearDepth(Float depth);
                Float GetClearDepth() const;
                void SetClearStencil(Int stencil);
                Uint32 GetClearStencil() const;
                void SetBlendColor(FloatVec4 color);
                const FloatVec4& GetBlendColor() const;
                void SetDepthRange(FloatVec2 range);
                const FloatVec2& GetDepthRange() const;
                void SetSampleCoverage(Float value, Bool invert);
                Float GetSampleCoverageValue() const;
                Bool GetSampleCoverageInvert() const;
                void SetSampleMaskValue(Uint32 mask);
                Uint32 GetSampleMaskValue() const;

                // Pixel Store
                void SetPixelStoreParam(PixelStoreParam param, Int value);
                Int GetPixelStoreParam(PixelStoreParam param) const;
                PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const;

                // Cull Face
                void SetCullFaceMode(CullFaceMode mode);
                CullFaceMode GetCullFaceMode() const;
                void SetFrontFaceMode(FrontFaceMode mode);
                FrontFaceMode GetFrontFaceMode() const;
                void SetProvokingVertexMode(ProvokingVertexMode mode);
                ProvokingVertexMode GetProvokingVertexMode() const;

                // Scissor
                void SetScissorBox(IntVec4 box);      // x, y, width, height
                const IntVec4& GetScissorBox() const; // x, y, width, height

            private:
                Uint16 m_version = 0;
                RenderStateParameters m_parameters;

                // Pixel Store
                PixelStoreParameters m_pixelStorePackParameters;
                PixelStoreParameters m_pixelStoreUnpackParameters;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
