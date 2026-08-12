// MobileGL - MobileGL/MG_State/GLState/RenderState/RenderState.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderState.h"
#include "MG_Util/Debug/Log.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            namespace {
                SizeT GetStencilFaceIndex(StencilFace face) {
                    switch (face) {
                    case StencilFace::Front:
                        return 0;
                    case StencilFace::Back:
                        return 1;
                    default:
                        MOBILEGL_ASSERT(false, "Invalid stencil face enum: %d", static_cast<int>(face));
                        return 0;
                    }
                }
            } // namespace

            RenderState::RenderState() {
                // The color writemask defaults to all-true for every draw buffer.
                for (auto& mask : m_parameters.ColorMasks) {
                    mask = BoolVec4(true, true, true, true);
                }
            }

            Uint RenderState::GetVersion() const {
                return m_version;
            }

            Uint RenderState::GetPipelineStateVersion() const {
                return m_pipelineStateVersion;
            }

            const RenderStateParameters& RenderState::GetAllParameters() const {
                return m_parameters;
            }

            // -------------------- Rasterization --------------------
            void RenderState::SetViewport(IntVec4 viewport) {
                if (m_parameters.Viewport == viewport) return;

                m_parameters.Viewport = viewport;
                ++m_version;
            }

            const IntVec4& RenderState::GetViewport() const {
                return m_parameters.Viewport;
            }

            void RenderState::SetLineWidth(Float width) {
                if (m_parameters.LineWidth == width) return;

                m_parameters.LineWidth = width;
                ++m_version;
            }

            Float RenderState::GetLineWidth() const {
                return m_parameters.LineWidth;
            }

            void RenderState::SetHint(GLenum target, GLenum mode) {
                GLenum* slot = nullptr;
                switch (target) {
                case GL_LINE_SMOOTH_HINT: slot = &m_parameters.LineSmoothHint; break;
                case GL_POLYGON_SMOOTH_HINT: slot = &m_parameters.PolygonSmoothHint; break;
                case GL_TEXTURE_COMPRESSION_HINT: slot = &m_parameters.TextureCompressionHint; break;
                case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: slot = &m_parameters.FragmentShaderDerivativeHint; break;
                default: return;
                }
                if (*slot == mode) return;
                *slot = mode;
                ++m_version;
            }

            GLenum RenderState::GetHint(GLenum target) const {
                switch (target) {
                case GL_LINE_SMOOTH_HINT: return m_parameters.LineSmoothHint;
                case GL_POLYGON_SMOOTH_HINT: return m_parameters.PolygonSmoothHint;
                case GL_TEXTURE_COMPRESSION_HINT: return m_parameters.TextureCompressionHint;
                case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: return m_parameters.FragmentShaderDerivativeHint;
                default: return GL_DONT_CARE;
                }
            }

            void RenderState::SetPointFadeThresholdSize(Float size) {
                if (m_parameters.PointFadeThresholdSize == size) return;
                m_parameters.PointFadeThresholdSize = size;
                ++m_version;
            }

            Float RenderState::GetPointFadeThresholdSize() const {
                return m_parameters.PointFadeThresholdSize;
            }

            void RenderState::SetPointSpriteCoordOrigin(GLenum origin) {
                if (m_parameters.PointSpriteCoordOrigin == origin) return;
                m_parameters.PointSpriteCoordOrigin = origin;
                ++m_version;
            }

            GLenum RenderState::GetPointSpriteCoordOrigin() const {
                return m_parameters.PointSpriteCoordOrigin;
            }

            void RenderState::SetClampReadColor(GLenum clamp) {
                if (m_parameters.ClampReadColor == clamp) return;
                m_parameters.ClampReadColor = clamp;
                ++m_version;
            }

            GLenum RenderState::GetClampReadColor() const {
                return m_parameters.ClampReadColor;
            }

            void RenderState::SetPolygonMode(GLenum front, GLenum back) {
                if (m_parameters.PolygonModeFront == front && m_parameters.PolygonModeBack == back) return;
                m_parameters.PolygonModeFront = front;
                m_parameters.PolygonModeBack = back;
                BumpVersions();
            }

            GLenum RenderState::GetPolygonModeFront() const {
                return m_parameters.PolygonModeFront;
            }

            GLenum RenderState::GetPolygonModeBack() const {
                return m_parameters.PolygonModeBack;
            }

            void RenderState::SetPrimitiveRestartIndex(Uint32 index) {
                if (m_parameters.PrimitiveRestartIndex == index) return;
                m_parameters.PrimitiveRestartIndex = index;
                ++m_version;
            }

            Uint32 RenderState::GetPrimitiveRestartIndex() const {
                return m_parameters.PrimitiveRestartIndex;
            }

            void RenderState::SetPointSize(Float size) {
                if (m_parameters.PointSize == size) return;

                m_parameters.PointSize = size;
                ++m_version;
            }

            Float RenderState::GetPointSize() const {
                return m_parameters.PointSize;
            }

            void RenderState::SetPatchVertices(Uint vertices) {
                if (m_parameters.PatchVertices == vertices) return;

                m_parameters.PatchVertices = vertices;
                BumpVersions();
            }

            Uint RenderState::GetPatchVertices() const {
                return m_parameters.PatchVertices;
            }

            void RenderState::SetPolygonOffset(Float factor, Float units) {
                if (m_parameters.PolygonOffsetFactor == factor && m_parameters.PolygonOffsetUnits == units) return;

                m_parameters.PolygonOffsetFactor = factor;
                m_parameters.PolygonOffsetUnits = units;
                ++m_version;
            }

            Float RenderState::GetPolygonOffsetFactor() const {
                return m_parameters.PolygonOffsetFactor;
            }

            Float RenderState::GetPolygonOffsetUnits() const {
                return m_parameters.PolygonOffsetUnits;
            }

            // -------------------- Capabilities --------------------
            namespace {
                // CapabilityInput lists ClipDistance0..7 contiguously (RenderState.h); the caller
                // has already rejected anything outside that run, so the subtraction is in range.
                Uint32 ClipDistanceBit(CapabilityInput cap) {
                    return 1u << (static_cast<Uint>(cap) - static_cast<Uint>(CapabilityInput::ClipDistance0));
                }
            } // namespace

            void RenderState::SetCapability(CapabilityInput cap, Bool enabled) {
#define SET_CAPABILITY(capability, flag)                                                                               \
    case CapabilityInput::capability:                                                                                  \
        if (m_parameters.capability##Enabled == (flag)) break;                                                         \
        m_parameters.capability##Enabled = (flag);                                                                     \
        BumpVersions();                                                                                                   \
        break;

                switch (cap) {
                    SET_CAPABILITY(ColorLogicOp, enabled);
                    SET_CAPABILITY(DebugOutput, enabled);
                    SET_CAPABILITY(DebugOutputSynchronous, enabled);
                    SET_CAPABILITY(DepthTest, enabled);
                    SET_CAPABILITY(CullFace, enabled);
                    SET_CAPABILITY(Dither, enabled);
                    SET_CAPABILITY(LineSmooth, enabled);
                    SET_CAPABILITY(Multisample, enabled);
                    SET_CAPABILITY(PolygonOffsetFill, enabled);
                    SET_CAPABILITY(PolygonOffsetLine, enabled);
                    SET_CAPABILITY(PolygonOffsetPoint, enabled);
                    SET_CAPABILITY(PolygonSmooth, enabled);
                    SET_CAPABILITY(PrimitiveRestart, enabled);
                    SET_CAPABILITY(PrimitiveRestartFixedIndex, enabled);
                    SET_CAPABILITY(RasterizerDiscard, enabled);
                    SET_CAPABILITY(SampleAlphaToCoverage, enabled);
                    SET_CAPABILITY(SampleAlphaToOne, enabled);
                    SET_CAPABILITY(SampleCoverage, enabled);
                    SET_CAPABILITY(SampleMask, enabled);
                    SET_CAPABILITY(ScissorTest, enabled);
                    SET_CAPABILITY(StencilTest, enabled);
                    SET_CAPABILITY(ProgramPointSize, enabled);
                case CapabilityInput::Blend: {
                    Bool stateChanged = false;
                    for (auto& blendState : m_parameters.BlendStates) {
                        if (blendState.Enabled == enabled) continue;
                        blendState.Enabled = enabled;
                        stateChanged = true;
                    }
                    if (stateChanged) BumpVersions();
                    break;
                }
                case CapabilityInput::ClipDistance0:
                case CapabilityInput::ClipDistance1:
                case CapabilityInput::ClipDistance2:
                case CapabilityInput::ClipDistance3:
                case CapabilityInput::ClipDistance4:
                case CapabilityInput::ClipDistance5:
                case CapabilityInput::ClipDistance6:
                case CapabilityInput::ClipDistance7: {
                    const Uint32 bit = ClipDistanceBit(cap);
                    const Uint32 updated =
                        enabled ? (m_parameters.ClipDistanceEnabledMask | bit)
                                : (m_parameters.ClipDistanceEnabledMask & ~bit);
                    if (updated == m_parameters.ClipDistanceEnabledMask) break;
                    m_parameters.ClipDistanceEnabledMask = updated;
                    // Deliberately NOT BumpVersions(): no backend bakes a clip-distance enable
                    // into a pipeline object (DirectGLES issues glEnable, DirectVulkan takes the
                    // set from the shader's declared array), so bumping the pipeline version here
                    // would evict cached pipelines for state they do not contain.
                    ++m_version;
                    break;
                }
                default: // not supported currently
                    break;
                }
#undef SET_CAPABILITY
            }

            Bool RenderState::IsCapabilityEnabled(CapabilityInput cap) const {
#define RETURN_CAPABILITY(capability)                                                                                  \
    case CapabilityInput::capability:                                                                                  \
        return m_parameters.capability##Enabled;
                switch (cap) {
                    RETURN_CAPABILITY(ColorLogicOp);
                    RETURN_CAPABILITY(DebugOutput);
                    RETURN_CAPABILITY(DebugOutputSynchronous);
                    RETURN_CAPABILITY(DepthTest);
                    RETURN_CAPABILITY(CullFace);
                    RETURN_CAPABILITY(Dither);
                    RETURN_CAPABILITY(LineSmooth);
                    RETURN_CAPABILITY(Multisample);
                    RETURN_CAPABILITY(PolygonOffsetFill);
                    RETURN_CAPABILITY(PolygonOffsetLine);
                    RETURN_CAPABILITY(PolygonOffsetPoint);
                    RETURN_CAPABILITY(PolygonSmooth);
                    RETURN_CAPABILITY(PrimitiveRestart);
                    RETURN_CAPABILITY(PrimitiveRestartFixedIndex);
                    RETURN_CAPABILITY(RasterizerDiscard);
                    RETURN_CAPABILITY(SampleAlphaToCoverage);
                    RETURN_CAPABILITY(SampleAlphaToOne);
                    RETURN_CAPABILITY(SampleCoverage);
                    RETURN_CAPABILITY(SampleMask);
                    RETURN_CAPABILITY(ScissorTest);
                    RETURN_CAPABILITY(StencilTest);
                    RETURN_CAPABILITY(ProgramPointSize);
                case CapabilityInput::Blend:
                    return m_parameters.BlendStates[0].Enabled;
                case CapabilityInput::ClipDistance0:
                case CapabilityInput::ClipDistance1:
                case CapabilityInput::ClipDistance2:
                case CapabilityInput::ClipDistance3:
                case CapabilityInput::ClipDistance4:
                case CapabilityInput::ClipDistance5:
                case CapabilityInput::ClipDistance6:
                case CapabilityInput::ClipDistance7:
                    return (m_parameters.ClipDistanceEnabledMask & ClipDistanceBit(cap)) != 0;
                default:
                    return false;
                }
            }

            void RenderState::SetCapabilityIndexed(CapabilityInput cap, Uint index, Bool enabled) {
                // Only for BlendState currently. The GL entry points (glEnablei/glDisablei) already
                // reject every non-GL_BLEND target with GL_INVALID_ENUM before reaching here, so this
                // is a backstop - but it must stay a backstop: THROW_UNIMPL_EXCEPTION unwinds a C++
                // exception through the C GL ABI and terminates the process.
                if (cap != CapabilityInput::Blend) {
                    MGLOG_I("RenderState::SetCapabilityIndexed: indexed capability state exists only for "
                            "GL_BLEND (cap=%d, index=%u); ignoring",
                            static_cast<int>(cap), index);
                    return;
                }
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend capability index out of range: %d", index);
                    return;
                }
                if (m_parameters.BlendStates[index].Enabled == enabled) return;

                m_parameters.BlendStates[index].Enabled = enabled;
                BumpVersions();
            }

            Bool RenderState::IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const {
                // Only for BlendState currently - same backstop reasoning as SetCapabilityIndexed:
                // glIsEnabledi has already answered GL_INVALID_ENUM/GL_FALSE for anything else, and a
                // query must never be able to terminate the process.
                if (cap != CapabilityInput::Blend) {
                    MGLOG_I("RenderState::IsCapabilityEnabledIndexed: indexed capability state exists only "
                            "for GL_BLEND (cap=%d, index=%u); reporting disabled",
                            static_cast<int>(cap), index);
                    return false;
                }
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend capability index out of range: %d", index);
                    return false;
                }
                return m_parameters.BlendStates[index].Enabled;
            }

            // -------------------- Blending --------------------
            void RenderState::SetBlendFunc(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha,
                                           BlendFactor dstAlpha) {
                Bool stateChanged = false;
                for (auto& blendState : m_parameters.BlendStates) {
                    if (blendState.SrcFactorRGB == srcRGB && blendState.DstFactorRGB == dstRGB &&
                        blendState.SrcFactorAlpha == srcAlpha && blendState.DstFactorAlpha == dstAlpha) {
                        continue;
                    }
                    blendState.SrcFactorRGB = srcRGB;
                    blendState.DstFactorRGB = dstRGB;
                    blendState.SrcFactorAlpha = srcAlpha;
                    blendState.DstFactorAlpha = dstAlpha;
                    stateChanged = true;
                }
                if (!stateChanged) return;
                BumpVersions();
            }

            void RenderState::GetBlendFunc(BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                           BlendFactor& dstAlpha) const {
                srcRGB = m_parameters.BlendStates[0].SrcFactorRGB;
                dstRGB = m_parameters.BlendStates[0].DstFactorRGB;
                srcAlpha = m_parameters.BlendStates[0].SrcFactorAlpha;
                dstAlpha = m_parameters.BlendStates[0].DstFactorAlpha;
            }

            void RenderState::SetBlendFuncIndexed(Uint index, BlendFactor srcRGB, BlendFactor dstRGB,
                                                  BlendFactor srcAlpha, BlendFactor dstAlpha) {
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend function index out of range: %d", index);
                    return;
                }
                PerBufferBlendState& blendState = m_parameters.BlendStates[index];
                if (blendState.SrcFactorRGB == srcRGB && blendState.DstFactorRGB == dstRGB &&
                    blendState.SrcFactorAlpha == srcAlpha && blendState.DstFactorAlpha == dstAlpha) {
                    return;
                }
                blendState.SrcFactorRGB = srcRGB;
                blendState.DstFactorRGB = dstRGB;
                blendState.SrcFactorAlpha = srcAlpha;
                blendState.DstFactorAlpha = dstAlpha;
                BumpVersions();
            }

            void RenderState::GetBlendFuncIndexed(Uint index, BlendFactor& srcRGB, BlendFactor& dstRGB,
                                                  BlendFactor& srcAlpha, BlendFactor& dstAlpha) const {
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend function index out of range: %d", index);
                    return;
                }
                srcRGB = m_parameters.BlendStates[index].SrcFactorRGB;
                dstRGB = m_parameters.BlendStates[index].DstFactorRGB;
                srcAlpha = m_parameters.BlendStates[index].SrcFactorAlpha;
                dstAlpha = m_parameters.BlendStates[index].DstFactorAlpha;
            }

            void RenderState::SetBlendEquation(BlendEquation color, BlendEquation alpha) {
                Bool stateChanged = false;
                for (auto& blendState : m_parameters.BlendStates) {
                    if (blendState.ColorEquation == color && blendState.AlphaEquation == alpha) {
                        continue;
                    }
                    blendState.ColorEquation = color;
                    blendState.AlphaEquation = alpha;
                    stateChanged = true;
                }
                if (!stateChanged) return;
                BumpVersions();
            }

            void RenderState::GetBlendEquation(BlendEquation& color, BlendEquation& alpha) const {
                color = m_parameters.BlendStates[0].ColorEquation;
                alpha = m_parameters.BlendStates[0].AlphaEquation;
            }

            void RenderState::SetBlendEquationIndexed(Uint index, BlendEquation color, BlendEquation alpha) {
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend equation index out of range: %d", index);
                    return;
                }
                PerBufferBlendState& blendState = m_parameters.BlendStates[index];
                if (blendState.ColorEquation == color && blendState.AlphaEquation == alpha) {
                    return;
                }
                blendState.ColorEquation = color;
                blendState.AlphaEquation = alpha;
                BumpVersions();
            }

            void RenderState::GetBlendEquationIndexed(Uint index, BlendEquation& color, BlendEquation& alpha) const {
                if (index >= MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                    MOBILEGL_ASSERT(false, "Blend equation index out of range: %d", index);
                    return;
                }
                color = m_parameters.BlendStates[index].ColorEquation;
                alpha = m_parameters.BlendStates[index].AlphaEquation;
            }

            void RenderState::SetLogicOp(LogicOperation logicOp) {
                if (m_parameters.LogicOp == logicOp) return;

                m_parameters.LogicOp = logicOp;
                BumpVersions();
            }

            LogicOperation RenderState::GetLogicOp() const {
                return m_parameters.LogicOp;
            }

            // -------------------- Depth --------------------
            void RenderState::SetDepthFunc(DepthTestFunc func) {
                if (m_parameters.DepthFunc == func) return;

                m_parameters.DepthFunc = func;
                BumpVersions();
            }

            DepthTestFunc RenderState::GetDepthFunc() const {
                return m_parameters.DepthFunc;
            }

            void RenderState::SetDepthMask(Bool flag) {
                if (m_parameters.DepthMask == flag) return;

                m_parameters.DepthMask = flag;
                BumpVersions();
            }

            Bool RenderState::GetDepthMask() const {
                return m_parameters.DepthMask;
            }

            void RenderState::SetStencilFunc(StencilFace face, DepthTestFunc func, Int ref, Uint32 mask) {
                StencilFaceState& state = m_parameters.StencilStates[GetStencilFaceIndex(face)];
                if (state.Func == func && state.Ref == ref && state.ValueMask == mask) return;

                // Only Func is baked into the pipeline; Ref and ValueMask are dynamic state
                // (VK_DYNAMIC_STATE_STENCIL_REFERENCE / _COMPARE_MASK), so glStencilFunc changing
                // only the reference must not evict a cached pipeline.
                const Bool pipelineRelevantChange = state.Func != func;
                state.Func = func;
                state.Ref = ref;
                state.ValueMask = mask;
                ++m_version;
                if (pipelineRelevantChange) ++m_pipelineStateVersion;
            }

            void RenderState::SetStencilMask(StencilFace face, Uint32 mask) {
                StencilFaceState& state = m_parameters.StencilStates[GetStencilFaceIndex(face)];
                if (state.WriteMask == mask) return;

                state.WriteMask = mask;
                ++m_version;
            }

            void RenderState::SetStencilOp(StencilFace face, StencilOperation fail, StencilOperation depthFail,
                                           StencilOperation depthPass) {
                StencilFaceState& state = m_parameters.StencilStates[GetStencilFaceIndex(face)];
                if (state.FailOp == fail && state.PassDepthFailOp == depthFail &&
                    state.PassDepthPassOp == depthPass) {
                    return;
                }

                state.FailOp = fail;
                state.PassDepthFailOp = depthFail;
                state.PassDepthPassOp = depthPass;
                BumpVersions();
            }

            const StencilFaceState& RenderState::GetStencilState(StencilFace face) const {
                return m_parameters.StencilStates[GetStencilFaceIndex(face)];
            }

            // -------------------- Color Mask --------------------
            void RenderState::SetColorMask(BoolVec4 mask) {
                // glColorMask broadcasts the same mask to every draw buffer.
                Bool changed = false;
                for (auto& slot : m_parameters.ColorMasks) {
                    if (!(slot == mask)) {
                        slot = mask;
                        changed = true;
                    }
                }
                if (changed) BumpVersions();
            }

            BoolVec4 RenderState::GetColorMask() const {
                // Non-indexed query reports draw buffer 0.
                return m_parameters.ColorMasks[0];
            }

            void RenderState::SetColorMaskIndexed(Uint index, BoolVec4 mask) {
                if (m_parameters.ColorMasks[index] == mask) return;
                m_parameters.ColorMasks[index] = mask;
                BumpVersions();
            }

            BoolVec4 RenderState::GetColorMaskIndexed(Uint index) const {
                return m_parameters.ColorMasks[index];
            }

            // -------------------- Clear State --------------------
            void RenderState::SetClearColor(FloatVec4 color) {
                if (m_parameters.ClearColor == color) return;

                m_parameters.ClearColor = color;
                ++m_version;
            }

            const FloatVec4& RenderState::GetClearColor() const {
                return m_parameters.ClearColor;
            }

            void RenderState::SetClearDepth(Float depth) {
                if (m_parameters.ClearDepth == depth) return;

                m_parameters.ClearDepth = depth;
                ++m_version;
            }

            Float RenderState::GetClearDepth() const {
                return m_parameters.ClearDepth;
            }

            void RenderState::SetClearStencil(Int stencil) {
                if (m_parameters.ClearStencil == stencil) return;

                m_parameters.ClearStencil = stencil;
                ++m_version;
            }

            Uint32 RenderState::GetClearStencil() const {
                return m_parameters.ClearStencil;
            }

            void RenderState::SetBlendColor(FloatVec4 color) {
                if (m_parameters.BlendColor == color) return;

                m_parameters.BlendColor = color;
                ++m_version;
            }

            const FloatVec4& RenderState::GetBlendColor() const {
                return m_parameters.BlendColor;
            }

            void RenderState::SetDepthRange(FloatVec2 range) {
                if (m_parameters.DepthRange == range) return;

                m_parameters.DepthRange = range;
                ++m_version;
            }

            const FloatVec2& RenderState::GetDepthRange() const {
                return m_parameters.DepthRange;
            }

            void RenderState::SetSampleCoverage(Float value, Bool invert) {
                if (m_parameters.SampleCoverageValue == value && m_parameters.SampleCoverageInvert == invert) return;

                m_parameters.SampleCoverageValue = value;
                m_parameters.SampleCoverageInvert = invert;
                BumpVersions();
            }

            Float RenderState::GetSampleCoverageValue() const {
                return m_parameters.SampleCoverageValue;
            }

            Bool RenderState::GetSampleCoverageInvert() const {
                return m_parameters.SampleCoverageInvert;
            }

            void RenderState::SetSampleMaskValue(Uint32 mask) {
                if (m_parameters.SampleMaskValue == mask) return;

                m_parameters.SampleMaskValue = mask;
                BumpVersions();
            }

            Uint32 RenderState::GetSampleMaskValue() const {
                return m_parameters.SampleMaskValue;
            }

            // -------------------- Pixel Store --------------------
            void RenderState::SetPixelStoreParam(PixelStoreParam param, Int value) {
#define SET_PIXEL_STORE_PARAM(paramNameHead, paramNameTail, val)                                                       \
    case PixelStoreParam::paramNameHead##paramNameTail:                                                                \
        if (m_pixelStore##paramNameHead##Parameters.paramNameTail == (val)) break;                                     \
        m_pixelStore##paramNameHead##Parameters.paramNameTail = (val);                                                 \
        break;

                switch (param) {
                    SET_PIXEL_STORE_PARAM(Pack, Alignment, value);
                    SET_PIXEL_STORE_PARAM(Pack, RowLength, value);
                    SET_PIXEL_STORE_PARAM(Pack, ImageHeight, value);
                    SET_PIXEL_STORE_PARAM(Pack, SkipPixels, value);
                    SET_PIXEL_STORE_PARAM(Pack, SkipRows, value);
                    SET_PIXEL_STORE_PARAM(Pack, SkipImages, value);
                    SET_PIXEL_STORE_PARAM(Pack, SwapBytes, value != 0);
                    SET_PIXEL_STORE_PARAM(Pack, LSBFirst, value != 0);
                    SET_PIXEL_STORE_PARAM(Unpack, Alignment, value);
                    SET_PIXEL_STORE_PARAM(Unpack, RowLength, value);
                    SET_PIXEL_STORE_PARAM(Unpack, ImageHeight, value);
                    SET_PIXEL_STORE_PARAM(Unpack, SkipPixels, value);
                    SET_PIXEL_STORE_PARAM(Unpack, SkipRows, value);
                    SET_PIXEL_STORE_PARAM(Unpack, SkipImages, value);
                    SET_PIXEL_STORE_PARAM(Unpack, SwapBytes, value != 0);
                    SET_PIXEL_STORE_PARAM(Unpack, LSBFirst, value != 0);
                default:
                    MOBILEGL_ASSERT(false, "Invalid PixelStoreParam enum: %d", static_cast<int>(param));
                    return;
                }
            }

            Int RenderState::GetPixelStoreParam(PixelStoreParam param) const {
#define RETURN_PIXEL_STORE_PARAM(paramNameHead, paramNameTail)                                                         \
    case PixelStoreParam::paramNameHead##paramNameTail:                                                                \
        return m_pixelStore##paramNameHead##Parameters.paramNameTail;
                switch (param) {
                    RETURN_PIXEL_STORE_PARAM(Pack, Alignment);
                    RETURN_PIXEL_STORE_PARAM(Pack, RowLength);
                    RETURN_PIXEL_STORE_PARAM(Pack, ImageHeight);
                    RETURN_PIXEL_STORE_PARAM(Pack, SkipPixels);
                    RETURN_PIXEL_STORE_PARAM(Pack, SkipRows);
                    RETURN_PIXEL_STORE_PARAM(Pack, SkipImages);
                    RETURN_PIXEL_STORE_PARAM(Pack, SwapBytes);
                    RETURN_PIXEL_STORE_PARAM(Pack, LSBFirst);
                    RETURN_PIXEL_STORE_PARAM(Unpack, Alignment);
                    RETURN_PIXEL_STORE_PARAM(Unpack, RowLength);
                    RETURN_PIXEL_STORE_PARAM(Unpack, ImageHeight);
                    RETURN_PIXEL_STORE_PARAM(Unpack, SkipPixels);
                    RETURN_PIXEL_STORE_PARAM(Unpack, SkipRows);
                    RETURN_PIXEL_STORE_PARAM(Unpack, SkipImages);
                    RETURN_PIXEL_STORE_PARAM(Unpack, SwapBytes);
                    RETURN_PIXEL_STORE_PARAM(Unpack, LSBFirst);
                default:
                    MOBILEGL_ASSERT(false, "Invalid PixelStoreParam enum: %d", static_cast<int>(param));
                    return 0;
                }
            }

            PixelStoreParameters RenderState::GetPixelStoreParameters(Bool isUnpack) const {
                return isUnpack ? m_pixelStoreUnpackParameters : m_pixelStorePackParameters;
            }

            // -------------------- Cull Face --------------------
            void RenderState::SetCullFaceMode(CullFaceMode mode) {
                if (m_parameters.CullFaceModeSetting == mode) return;

                m_parameters.CullFaceModeSetting = mode;
                BumpVersions();
            }

            CullFaceMode RenderState::GetCullFaceMode() const {
                return m_parameters.CullFaceModeSetting;
            }

            void RenderState::SetFrontFaceMode(FrontFaceMode mode) {
                if (m_parameters.FrontFaceModeSetting == mode) return;

                m_parameters.FrontFaceModeSetting = mode;
                BumpVersions();
            }

            FrontFaceMode RenderState::GetFrontFaceMode() const {
                return m_parameters.FrontFaceModeSetting;
            }

            void RenderState::SetProvokingVertexMode(ProvokingVertexMode mode) {
                if (m_parameters.ProvokingVertexModeSetting == mode) return;

                m_parameters.ProvokingVertexModeSetting = mode;
                BumpVersions();
            }

            ProvokingVertexMode RenderState::GetProvokingVertexMode() const {
                return m_parameters.ProvokingVertexModeSetting;
            }

            // --------------------- Scissor ---------------------
            void RenderState::SetScissorBox(IntVec4 box) {
                if (m_parameters.ScissorBox == box) return;

                m_parameters.ScissorBox = box;
                ++m_version;
            }

            const IntVec4& RenderState::GetScissorBox() const {
                return m_parameters.ScissorBox;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
