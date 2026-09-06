// MobileGL - MobileGL/MG_State/GLState/RenderState/RenderState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Pipe/MGPipeValueTypes.h>

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            class RenderState {
            public:
                RenderState();

                Uint GetVersion() const;
                // Version of the pipeline-relevant subset only - see m_pipelineStateVersion.
                Uint GetPipelineStateVersion() const;
                const RenderStateParameters& GetAllParameters() const;

                // Rasterization
                // ARB_viewport_array defines glViewport as ViewportIndexedf on EVERY index, so the
                // classic setter broadcasts; GetViewport answers for index 0 (rounded to the
                // integers glGetIntegerv(GL_VIEWPORT) and both backends want) and is BY VALUE for
                // that reason. The indexed pair is the verbatim float state.
                void SetViewport(IntVec4 viewport); // x, y, width, height
                IntVec4 GetViewport() const;        // x, y, width, height, viewport 0, rounded
                void SetViewportIndexed(Uint index, FloatVec4 viewport);
                const FloatVec4& GetViewportIndexed(Uint index) const;
                void SetLineWidth(Float width);
                Float GetLineWidth() const;
                void SetPointSize(Float size);
                Float GetPointSize() const;
                void SetPatchVertices(Uint vertices);
                Uint GetPatchVertices() const;
                void SetPatchDefaultOuterLevel(const FloatVec4& levels);
                const FloatVec4& GetPatchDefaultOuterLevel() const;
                void SetPatchDefaultInnerLevel(const FloatVec2& levels);
                const FloatVec2& GetPatchDefaultInnerLevel() const;
                void SetPolygonOffset(Float factor, Float units);
                // glPolygonOffsetClamp. Writes the same factor/units as glPolygonOffset plus the
                // clamp, because that is what the entry point does - glPolygonOffset is the
                // clamp = 0 case of it (GL 4.6 core 14.6.5).
                void SetPolygonOffsetClamped(Float factor, Float units, Float clamp);
                Float GetPolygonOffsetFactor() const;
                Float GetPolygonOffsetUnits() const;
                Float GetPolygonOffsetClamp() const;
                void SetClipControl(GLenum origin, GLenum depth);
                GLenum GetClipOrigin() const;
                GLenum GetClipDepthMode() const;
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
                // glDepthRange(f) writes every viewport's range (ARB_viewport_array); the indexed
                // pair is glDepthRangeIndexed / glDepthRangeArrayv. GetDepthRange answers index 0.
                void SetDepthRange(FloatVec2 range);
                const FloatVec2& GetDepthRange() const;
                void SetDepthRangeIndexed(Uint index, FloatVec2 range);
                const FloatVec2& GetDepthRangeIndexed(Uint index) const;
                void SetSampleCoverage(Float value, Bool invert);
                Float GetSampleCoverageValue() const;
                Bool GetSampleCoverageInvert() const;
                void SetSampleMaskValue(Uint32 mask);
                Uint32 GetSampleMaskValue() const;
                // glMinSampleShading. `value` is stored as given; the entry point clamps.
                void SetMinSampleShadingValue(Float value);
                Float GetMinSampleShadingValue() const;

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

                // Scissor. glScissor writes every rectangle (ARB_viewport_array); GetScissorBox
                // answers for index 0.
                void SetScissorBox(IntVec4 box);      // x, y, width, height
                const IntVec4& GetScissorBox() const; // x, y, width, height
                void SetScissorBoxIndexed(Uint index, IntVec4 box);
                const IntVec4& GetScissorBoxIndexed(Uint index) const;

            private:
                // Bump both: any state change invalidates the draw snapshot, and this one also
                // changes the VkPipeline (or its DirectGLES equivalent).
                void BumpVersions() {
                    ++m_version;
                    ++m_pipelineStateVersion;
                }

                Uint16 m_version = 0;
                // Only the subset of render state that a backend bakes INTO a pipeline object.
                // Viewport, scissor, depth range, blend colour, line width, polygon offset, stencil
                // write mask, the clear values, hints and the point-size family are all either
                // dynamic pipeline state or not pipeline state at all, so changing one of them must
                // not evict a cached pipeline. Keeping one counter for both made a glViewport call
                // knock the next draw off the pipeline memo AND the draw fast path.
                Uint16 m_pipelineStateVersion = 0;
                RenderStateParameters m_parameters;

                // Pixel Store
                PixelStoreParameters m_pixelStorePackParameters;
                PixelStoreParameters m_pixelStoreUnpackParameters;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
