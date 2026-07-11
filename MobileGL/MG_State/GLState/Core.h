// MobileGL - MobileGL/MG_State/GLState/Core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "ErrorState/Error.h"
#include "BufferState/BufferState.h"
#include "MG_State/GLState/RenderbufferState/RenderbufferObject.h"
#include "MG_State/GLState/RenderbufferState/RenderbufferState.h"
#include "RenderState/RenderState.h"
#include "ProgramState/ProgramState.h"
#include "SamplerState/SamplerState.h"
#include "TextureState/TextureState.h"
#include "FramebufferState/FramebufferState.h"
#include "VertexArrayState/VertexArrayState.h"
#include "RenderbufferState/RenderbufferState.h"

namespace MobileGL {
    namespace MG_State {
        void Init();

        namespace GLState {
            struct CurrentVertexAttributeValue {
                Array<Float, 4> floatValue{0.f, 0.f, 0.f, 1.f};
                Array<Int32, 4> intValue{0, 0, 0, 1};
                Array<Uint32, 4> uintValue{0u, 0u, 0u, 1u};
            };

            // Which of the three views above a shader input of a given GLSL type consumes.
            enum class VertexAttribBaseType { Unsupported, Float, Int, Uint };

            struct VertexAttribTypeInfo {
                VertexAttribBaseType baseType = VertexAttribBaseType::Unsupported;
                Uint componentCount = 0;
            };

            // Maps a shader vertex-input type (GL_FLOAT_VEC3, GL_INT_VEC2, ...) onto the current-value
            // view that feeds it. Shared by every backend so that "a disabled array reads the current
            // value" resolves identically regardless of which backend is active; each backend only
            // translates the result into its own API call.
            VertexAttribTypeInfo ClassifyVertexAttribType(GLenum glType);

            class GLContext {
            public:
                GLContext() = default;

                // Error
                void RecordError(ErrorCode code, UniquePtr<ErrorInfo> info);
                Bool HasGLError() const;
                Optional<const Error*> PeekNonGLError() const;
                Optional<UniquePtr<Error>> PopNonGLError();
                Bool HasNonGLError() const;
                Optional<const Error*> PeekGLError() const;
                Optional<UniquePtr<Error>> PopGLError();
                void ClearErrors();

                // Buffer
                void GenBufferNames(Uint number, Vector<Uint>& buffers);
                const SharedPtr<BufferObject>& GetBufferObject(Uint index);
                BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget target);
                BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget target, Uint index);
                constexpr SizeT GetBufferBindingPointCount(BufferTarget target) const {
                    return m_bufferState.GetBindingPointCount(target);
                }
                const SharedPtr<BufferObject>& CreateBufferObject(Uint index);
                void MarkBufferObjectForDeletion(Uint index);
                Bool ValidateBufferName(Uint index) const;
                Bool ValidateBufferObject(Uint index) const;

                // VertexArray
                void GenVertexArrayNames(Uint number, Vector<Uint>& vertexArrays);
                const SharedPtr<VertexArrayObject>& GetVertexArrayObject(Uint index);
                void BindVertexArray(Uint index);
                const SharedPtr<VertexArrayObject>& CreateVertexArrayObject(Uint index);
                void MarkVertexArrayForDeletion(Uint index);
                Bool ValidateVertexArrayName(Uint index) const;
                Bool ValidateVertexArrayObject(Uint index) const;
                const SharedPtr<VertexArrayObject>& GetBoundVertexArray();
                void SetCurrentVertexAttributeFloat(Uint index, const Array<Float, 4>& value);
                void SetCurrentVertexAttributeInt(Uint index, const Array<Int32, 4>& value);
                void SetCurrentVertexAttributeUint(Uint index, const Array<Uint32, 4>& value);
                const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint index) const;

                // Texture
                void GenTextureNames(Uint number, Vector<Uint>& textures);
                const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
                const SharedPtr<ITextureObject>& CreateTextureObject(Uint index, TextureTarget target);
                void MarkTextureObjectForDeletion(Uint index);
                TextureUnit& GetTextureUnitObject(Int unit);
                ImageTextureBinding& GetImageTextureBinding(Int unit);
                const ImageTextureBinding& GetImageTextureBinding(Int unit) const;
                void NoteTextureUnitTouched(Int unit) { m_textureState.NoteUnitTouched(unit); }
                Int GetMaxTouchedTextureUnit() const { return m_textureState.GetMaxTouchedUnit(); }
                Bool ValidateTextureName(Uint index) const;
                Bool ValidateTextureObject(Uint index) const;
                Int GetActiveTextureUnit() const;
                void SetActiveTextureUnit(Int unit);

                // Program
                Uint CreateProgram();
                Uint CreateShader(ShaderStage stage);
                void MarkProgramForDeletion(Uint index);
                void MarkShaderForDeletion(Uint index);
                Bool ValidateProgramName(Uint index) const;
                Bool ValidateShaderName(Uint index) const;
                const SharedPtr<ProgramObject>& GetProgramObject(Uint index);
                const SharedPtr<ShaderObject>& GetShaderObject(Uint index);
                void UseProgram(Uint program);
                const SharedPtr<ProgramObject>& GetCurrentProgram();

                // RenderState
                Uint GetRenderStateParametersVersion() const;
                const RenderStateParameters& GetRenderStateParameters() const;
                void SetViewport(IntVec4 viewport); // x, y, width, height
                const IntVec4& GetViewport() const; // x, y, width, height
                void SetLineWidth(Float width);
                Float GetLineWidth() const;
                void SetPointSize(Float size);
                Float GetPointSize() const;
                void SetPolygonOffset(Float factor, Float units);
                Float GetPolygonOffsetFactor() const;
                Float GetPolygonOffsetUnits() const;
                void SetHint(GLenum target, GLenum mode);
                GLenum GetHint(GLenum target) const;
                void SetPointFadeThresholdSize(Float size);
                Float GetPointFadeThresholdSize() const;
                void SetPointSpriteCoordOrigin(GLenum origin);
                GLenum GetPointSpriteCoordOrigin() const;
                void SetClampReadColor(GLenum clamp);
                GLenum GetClampReadColor() const;
                void SetPolygonMode(GLenum front, GLenum back);
                GLenum GetPolygonModeFront() const;
                GLenum GetPolygonModeBack() const;
                void SetCapability(CapabilityInput cap, Bool enabled);
                Bool IsCapabilityEnabled(CapabilityInput cap) const;
                void SetCapabilityIndexed(CapabilityInput cap, Uint index, Bool enabled);
                Bool IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const;
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
                void SetDepthFunc(DepthTestFunc func);
                DepthTestFunc GetDepthFunc() const;
                void SetDepthMask(Bool flag);
                Bool GetDepthMask() const;
                void SetStencilFunc(StencilFace face, DepthTestFunc func, Int ref, Uint32 mask);
                void SetStencilMask(StencilFace face, Uint32 mask);
                void SetStencilOp(StencilFace face, StencilOperation fail, StencilOperation depthFail,
                                  StencilOperation depthPass);
                const StencilFaceState& GetStencilState(StencilFace face) const;
                void SetColorMask(BoolVec4 mask);
                BoolVec4 GetColorMask() const;
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
                void SetPixelStoreParam(PixelStoreParam param, Int value);
                Int GetPixelStoreParam(PixelStoreParam param) const;
                PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const;
                void SetCullFaceMode(CullFaceMode mode);
                CullFaceMode GetCullFaceMode() const;
                void SetFrontFaceMode(FrontFaceMode mode);
                FrontFaceMode GetFrontFaceMode() const;
                void SetProvokingVertexMode(ProvokingVertexMode mode);
                ProvokingVertexMode GetProvokingVertexMode() const;
                void SetScissorBox(IntVec4 box);      // x, y, width, height
                const IntVec4& GetScissorBox() const; // x, y, width, height

                // Framebuffer
                void GenFramebufferNames(Uint number, Vector<Uint>& framebuffers);
                const SharedPtr<FramebufferObject>& GetFramebufferObject(Uint index);
                BindingSlot<FramebufferObject>& GetFramebufferBindingSlot(FramebufferTarget target);
                const SharedPtr<FramebufferObject>& CreateFramebufferObject(Uint index);
                void MarkFramebufferObjectForDeletion(Uint index);
                Bool ValidateFramebufferName(Uint index) const;
                Bool ValidateFramebufferObject(Uint index) const;

                // Sampler
                void GenSamplerNames(Uint number, Vector<Uint>& samplers);
                const SharedPtr<SamplerObject>& GetSamplerObject(Uint index);
                const SharedPtr<SamplerObject>& CreateSamplerObject(Uint index);
                void MarkSamplerObjectForDeletion(Uint index);
                Bool ValidateSamplerName(Uint index) const;
                Bool ValidateSamplerObject(Uint index) const;

                // Renderbuffer
                void GenRenderbufferNames(Uint number, Vector<Uint>& renderbuffers);
                const SharedPtr<RenderbufferObject>& GetRenderbufferObject(Uint index);
                BindingSlot<RenderbufferObject>& GetRenderbufferBindingSlot(RenderbufferTarget target);
                const SharedPtr<RenderbufferObject>& CreateRenderbufferObject(Uint index);
                void MarkRenderbufferObjectForDeletion(Uint index);
                Bool ValidateRenderbufferName(Uint index) const;
                Bool ValidateRenderbufferObject(Uint index) const;

            private:
                // State Components
                ErrorState m_errorState;
                BufferState m_bufferState;
                VertexArrayState m_vertexArrayState;
                Array<CurrentVertexAttributeValue, VertexArrayObject::MAX_VERTEX_ATTRIBS> m_currentVertexAttributes{};
                TextureState m_textureState;
                ProgramState m_programState;
                RenderState m_renderState;
                FramebufferState m_framebufferState;
                SamplerState m_samplerState;
                RenderbufferState m_renderbufferState;
            };
        } // namespace GLState

        extern UniquePtr<GLState::GLContext> pGLContext;
    } // namespace MG_State
} // namespace MobileGL
