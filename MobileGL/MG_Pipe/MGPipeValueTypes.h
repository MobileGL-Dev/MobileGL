// MobileGL - MobileGL/MG_Pipe/MGPipeValueTypes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#ifndef MOBILEGL_MG_PIPE_VALUE_TYPES_H // belt and braces: this file is reachable both as
#define MOBILEGL_MG_PIPE_VALUE_TYPES_H // <MG_Pipe/...> and <...> (CMakeLists.txt:531,535)
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h> // includes only <Includes.h> + <cstring>
#include <cstddef>                    // offsetof
#include <type_traits>

// The value types MG_Pipe payloads embed (plan B section 6.3; ARCHITECTURE.md section on
// the value header): the render-state, pixel-store, sampler and vertex-attribute value
// structs and the enums they are made of. They lived in MG_State::GLState until P0.5;
// the MG_State headers that used to define them now include this file, so every existing
// spelling (namespace and name) compiles unchanged.
//
// PURITY: nothing from MG_State, MG_Impl, MG_Backend or MG_Remote -
// scripts/check_include_closure.py probe "value-header" (ROADMAP P0.5; ARCHITECTURE.md
// section 10.3 gate A). Adding one turns CI red. MG_Pipe never includes MG_State back.

namespace MobileGL {
    // GL_MAX_DRAW_BUFFERS as MobileGL advertises it. FramebufferObject::MAX_DRAW_BUFFERS is
    // defined from this constant, so the two cannot drift.
    inline constexpr Uint kMGMaxDrawBuffers = 8;

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
        // ARB_viewport_array / GL 4.6 core 13.6.1: the viewport, the scissor rectangle, the depth
        // range and the scissor-test enable are all arrays indexed by gl_ViewportIndex, and the
        // spec floor for MAX_VIEWPORTS is 16. MobileGL advertises exactly 16 on both backends, so
        // this is also what GL_MAX_VIEWPORTS reports (see the backend loaders' caps.MaxViewports).
        static constexpr Uint MAX_VIEWPORTS = 16;

        // Rasterization
        // The viewport rectangle is FLOAT state as of GL 4.1 - ViewportIndexedf writes fractional
        // values and GetFloati_v(GL_VIEWPORT) must hand them back bit-exact
        // (KHR-GL43.viewport_array.viewport_api compares with ==, no tolerance). glViewport's
        // integers are simply one way to write it. Index 0 is what a program that never assigns
        // gl_ViewportIndex rasterizes against, and what the classic glViewport /
        // glGetIntegerv(GL_VIEWPORT) pair addresses. Both backends rasterize the rectangle
        // rounded back to integers; the STATE stays exact, which is the half the conformance
        // suite checks (see the KNOWN INFIDELITY note in AdvertisedLimitsScenario.cpp).
        Array<FloatVec4, MAX_VIEWPORTS> Viewports{}; // x, y, width, height
        Float LineWidth = 1.0f;
        Float PointSize = 1.0f;
        // GL_PATCH_VERTICES: how many vertices one tessellation patch consumes.
        Uint PatchVertices = 3;
        // GL_PATCH_DEFAULT_OUTER_LEVEL / GL_PATCH_DEFAULT_INNER_LEVEL (glPatchParameterfv). The
        // tessellation levels used when a program has an evaluation stage and NO control stage -
        // GL's fixed-function pass-through (4.6 core 11.2.2). Both backends have to synthesize
        // that stage, and they bake these numbers into it, so a change here makes an already-built
        // one stale exactly as PATCH_VERTICES does. Default 1.0, per table 23.44.
        FloatVec4 PatchDefaultOuterLevel = FloatVec4(1.0f, 1.0f, 1.0f, 1.0f);
        FloatVec2 PatchDefaultInnerLevel = FloatVec2(1.0f, 1.0f);
        Float PolygonOffsetFactor = 0.0f;
        Float PolygonOffsetUnits = 0.0f;
        // GL_POLYGON_OFFSET_CLAMP (GL 4.6 core 14.6.5 / GL_EXT_polygon_offset_clamp): the maximum
        // magnitude of the offset glPolygonOffsetClamp's third argument allows. Zero - the default
        // - means "no clamp", which is exactly the behaviour glPolygonOffset leaves behind.
        Float PolygonOffsetClamp = 0.0f;

        // glClipControl (GL 4.5 core 13.5). Defaults per table 23.7 are the pre-4.5 fixed
        // behaviour: origin at the lower left, depth mapped from -1..1.
        GLenum ClipOrigin = GL_LOWER_LEFT;
        GLenum ClipDepthMode = GL_NEGATIVE_ONE_TO_ONE;

        // Blending
        Array<PerBufferBlendState, kMGMaxDrawBuffers> BlendStates;
        LogicOperation LogicOp = LogicOperation::Copy;

        // Depth
        Bool DepthTestEnabled = false;
        DepthTestFunc DepthFunc = DepthTestFunc::Less;
        Bool DepthMask = true;

        // Color Mask. Per-draw-buffer state (glColorMaski); glColorMask broadcasts to all buffers.
        // Every entry is initialized to all-true in RenderState's constructor.
        Array<BoolVec4, kMGMaxDrawBuffers> ColorMasks;

        // Clear State
        FloatVec4 ClearColor = FloatVec4(0.0f, 0.0f, 0.0f, 1.0f);
        Float ClearDepth = 1.0f;
        Uint32 ClearStencil = 0;
        FloatVec4 BlendColor = FloatVec4(0.0f, 0.0f, 0.0f, 0.0f);
        // Per-viewport depth range (glDepthRangeIndexed / glDepthRangeArrayv). Every entry is
        // initialized to (0, 1) in RenderState's constructor - a default member initializer would
        // not survive the Array<> aggregate. Kept float rather than double: DepthRangeArrayv takes
        // GLdouble, but the value reaches the hardware as VkViewport::minDepth/maxDepth (float) on
        // Magma and glDepthRangef on Espryt, so a double store would only widen the readback and
        // then lose it again at the same place.
        Array<FloatVec2, MAX_VIEWPORTS> DepthRanges{};
        Float SampleCoverageValue = 1.0f;
        Bool SampleCoverageInvert = false;
        Uint32 SampleMaskValue = 0xffffffffu;
        // glMinSampleShading (ARB_sample_shading / GL 4.0 core 14.3.1). The fraction of samples
        // that get their own independent shading when GL_SAMPLE_SHADING is enabled; the initial
        // value is 0, and the value is clamped to [0, 1] on the way in.
        Float MinSampleShadingValue = 0.0f;
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
        Bool SampleShadingEnabled = false;
        Bool StencilTestEnabled = false;
        Bool ProgramPointSizeEnabled = false;
        // glEnable(GL_SCISSOR_TEST) enables the test for EVERY viewport, glEnablei for one
        // (GL 4.6 core 17.3.2), so this is 16 bits and not a bool. Bit 0 is what the classic
        // glIsEnabled(GL_SCISSOR_TEST) reports and what both backends currently consume. Unlike
        // ClipDistanceEnabledMask below it DOES bump the pipeline version, because DirectGLES
        // turns it into a real glEnable/glDisable.
        Uint32 ScissorTestEnabledMask = 0;
        Array<IntVec4, MAX_VIEWPORTS> ScissorBoxes{}; // x, y, width, height
        // One bit per viewport, set the first time the application writes that index's scissor
        // rectangle - glScissor broadcasts and sets all 16, glScissorIndexed/glScissorArrayv set
        // the indices they name. It exists because the RECTANGLE cannot answer "has the
        // application spoken?": ScissorBoxes starts all-zero (its spec initial value is the size
        // of a window the frontend does not know yet, see the RenderState constructor), and
        // glScissor(0, 0, 0, 0) is a legal GL state meaning "the scissor test rejects every
        // fragment". A backend that reads an empty rectangle as the never-written sentinel
        // therefore INVERTS that request into "accept every fragment"; DirectGLES did exactly
        // that and KHR-GL43.viewport_array.scissor_zero_dimension caught it. Deliberately beside
        // ScissorBoxes so it shares their tail span (after LogicOp) and DirectGLES' span memcmp
        // picks a transition up like any other state.
        Uint32 ScissorBoxWrittenMask = 0;
        // glEnable(GL_CLIP_DISTANCE0 + i) for i in [0, 8), one bit each. A bitmask rather than
        // eight bools because every consumer wants the set, not an individual flag, and because
        // the SYNC_CAPABILITY/SET_CAPABILITY macros key off a "<Name>Enabled" field name that
        // eight numbered capabilities cannot share. Lives in the tail span (after LogicOp), so
        // DirectGLES' span memcmp picks a change up like any other capability.
        Uint32 ClipDistanceEnabledMask = 0;
    };

    enum class SamplerFilterMode {
        Nearest,
        Linear,
        SamplerFilterCount,
        Unknown = -1
    };

    enum class SamplerMipmapMode {
        None,
        Nearest,
        Linear,
        SamplerMipmapModeCount,
        Unknown = -1
    };

    enum class SamplerWrapMode {
        ClampToEdge,
        MirroredRepeat,
        Repeat,
        ClampToBorder,
        MirrorClampToEdge,
        SamplerWrapModeCount,
        Unknown = -1
    };

    enum class SamplerCompareMode {
        None,
        CompareToTexture,
        SamplerCompareModeCount,
        Unknown = -1
    };

    enum class SamplerCompareFunc {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
        SamplerCompareFuncCount,
        Unknown = -1
    };

    // Which of the three GL_TEXTURE_BORDER_COLOR entry-point families last wrote the border colour,
    // and therefore which of the three stored representations is AUTHORITATIVE. GL 4.6 core 8.10:
    // TexParameterIiv/Iuiv store an integer border colour "unmodified, with an internal data type of
    // integer", TexParameterfv stores a floating-point one, and the derived forms are only a
    // convenience for a getter of the other spelling. A backend cannot pick the right driver entry
    // point (glSamplerParameterIiv vs fv) or the right VkBorderColor family without this: numerically
    // the three representations are always populated, so the value alone says nothing about the form.
    enum class BorderColorForm : Uint8 {
        Float,
        Int,
        Uint
    };

    struct SamplerParameters {
        SamplerWrapMode wrapS = SamplerWrapMode::Repeat;
        SamplerWrapMode wrapT = SamplerWrapMode::Repeat;
        SamplerWrapMode wrapR = SamplerWrapMode::Repeat;
        SamplerFilterMode minFilter = SamplerFilterMode::Nearest;
        SamplerFilterMode magFilter = SamplerFilterMode::Linear;
        SamplerMipmapMode mipmapMode = SamplerMipmapMode::Linear;
        Float minLod = -1000.0f;
        Float maxLod = 1000.0f;
        Float lodBias = 0.0f;
        Float maxAnisotropy = 1.0f;
        // GL 4.6 core table 23.18 / GLES 3.2 table 21.16: TEXTURE_COMPARE_FUNC starts at LEQUAL,
        // for both sampler objects and the sampler state a texture object carries.
        SamplerCompareFunc compareFunc = SamplerCompareFunc::LessEqual;
        SamplerCompareMode compareMode = SamplerCompareMode::None;
        // TEXTURE_BORDER_COLOR is sampler state (GL 4.6 core table 23.18), so it belongs here and
        // not on the texture - a texture object reaches it through the sampler object it owns. The
        // three representations are the float, integer and unsigned-integer forms glSamplerParameterfv,
        // glSamplerParameterIiv and glSamplerParameterIuiv set; whichever is written last defines
        // the colour and the other two follow it, so a getter always has an answer.
        FloatVec4 borderColor = {0.0f, 0.0f, 0.0f, 0.0f};
        IntVec4 borderColorI = {0, 0, 0, 0};
        UintVec4 borderColorUI = {0, 0, 0, 0};
        BorderColorForm borderColorForm = BorderColorForm::Float;
    };

    namespace MG_State::GLState {
        class BufferObject;

        struct VertexAttribute {
            Bool Enabled = false;
            int Size = 4;
            DataType Type = DataType::Float32;
            Bool Normalized = false;
            // The RESOLVED byte distance between consecutive elements, never the raw
            // glVertexAttrib*Pointer argument: a pointer call's stride 0 means "tightly
            // packed" and is resolved to the element size here, so a zero that survives
            // into this field can only have come from the binding model, where a zero
            // VERTEX_BINDING_STRIDE means the opposite - every vertex reads the SAME
            // element and the fetch address never advances (GL 4.6 core 10.3.1). Backends
            // consume this verbatim; collapsing 0 back into the element size is what made
            // KHR-GL43.vertex_attrib_binding.basic-input-case7/8 read past the buffer.
            int Stride = 0;
            SizeT Offset = 0;
            Bool IsInteger = false;
            // GL_BGRA vertex size: four components in reversed (B,G,R,A) memory order. Size stays 4.
            // Set only by the long (L) format entry points. It is NOT implied by
            // Type == Float64: VertexAttribFormat(GL_DOUBLE) also reads doubles from memory but
            // asks for them *converted to float*, while VertexAttribLFormat keeps all 64 bits
            // (GL 4.6 core 10.3.2). Backends have to tell the two apart, and it is what
            // GL_VERTEX_ATTRIB_ARRAY_LONG reports.
            Bool IsLong = false;
            Bool IsBgra = false;
            Uint Divisor = 0;
            SharedPtr<BufferObject> Buffer;

            // GL 4.6 core table 23.3: VERTEX_ATTRIB_ARRAY_STRIDE and _POINTER are the
            // arguments of the last glVertexAttrib*Pointer call on this attribute,
            // reported verbatim, and NOTHING else writes them - not glVertexAttribFormat,
            // not glBindVertexBuffer. Stride/Offset above are the *resolved* draw inputs
            // and the binding model does overwrite those, so the two views have to be
            // stored apart or the binding-model sequence reports a legacy state it never
            // set (KHR-GL4x.vertex_attrib_binding.basic-state3).
            int LegacyStride = 0;
            SizeT LegacyPointer = 0;
        };

        // ARB_vertex_attrib_binding separate binding point. Attributes configured through the
        // binding-point API are resolved eagerly into the flat VertexAttribute view above, so
        // backends keep consuming resolved attributes and never see binding points.
        struct VertexBufferBindingPoint {
            SharedPtr<BufferObject> Buffer;
            SizeT Offset = 0;
            // GL 4.6 core table 23.4: the initial VERTEX_BINDING_STRIDE is 16, not 0.
            int Stride = 16;
            Uint Divisor = 0;
        };

        struct VertexAttributeVersion {
            Uint16 FormatVersion = 0;
            Uint16 BufferVersion = 0;
            Uint16 SwitchVersion = 0;
        };
    } // namespace MG_State::GLState

    // ---- trip wires (P0.5). Sizes are what every ABI MobileGL ships on produces: every
    // member is a fixed-width scalar, an enum of one, or an array of those - no pointer, no
    // SizeT - except the vertex types, which carry SharedPtr<BufferObject> by design and are
    // therefore not trivially copyable (MGPipeTypes.h carries them as a blob).
    static_assert(std::is_trivially_copyable_v<PixelStoreParameters> && sizeof(PixelStoreParameters) == 28);
    static_assert(std::is_trivially_copyable_v<PerBufferBlendState> && sizeof(PerBufferBlendState) == 28);
    static_assert(std::is_trivially_copyable_v<StencilFaceState> && sizeof(StencilFaceState) == 28);
    static_assert(std::is_trivially_copyable_v<RenderStateParameters>);
    static_assert(std::is_standard_layout_v<RenderStateParameters>); // offsetof legality
    static_assert(sizeof(RenderStateParameters) == 1168,
                  "RenderStateParameters changed size; MGL_RESIDUAL_BLOCK_SIZE and the Espryt spans depend on it");
    static_assert(offsetof(RenderStateParameters, BlendStates) < offsetof(RenderStateParameters, LogicOp));
    static_assert(std::tuple_size_v<decltype(RenderStateParameters::BlendStates)> == kMGMaxDrawBuffers);
    static_assert(std::is_trivially_copyable_v<SamplerParameters> && sizeof(SamplerParameters) == 100);
    static_assert(std::is_trivially_copyable_v<MG_State::GLState::VertexAttributeVersion> &&
                  sizeof(MG_State::GLState::VertexAttributeVersion) == 6);
} // namespace MobileGL
#endif // MOBILEGL_MG_PIPE_VALUE_TYPES_H
