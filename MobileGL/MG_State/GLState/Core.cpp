// MobileGL - MobileGL/MG_State/GLState/Core.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Core.h"
#include "MG_State/GLState/RenderbufferState/RenderbufferObject.h"
#include "MG_State/EGLState/Core.h"
#include <Config.h>

namespace MobileGL::MG_State {
    void Init() {
        MGLOG_D("Initializing MobileGL State...");
        pGLContext = MakeUnique<GLState::GLContext>();
        pEGLContext = MakeUnique<EGLState::EGLContext>();
    }

    Bool IsRelaxedSemanticsActive() {
        return MG_Config::Features.RelaxedSemantics ||
               !(pEGLContext && pEGLContext->IsCurrentContextOpenGLCoreProfile());
    }

    namespace GLState {
        // Error
        void GLContext::RecordError(ErrorCode code, UniquePtr<ErrorInfo> info) {
            m_errorState.RecordError(code, Move(info));
        }

        Bool GLContext::HasGLError() const {
            return m_errorState.HasGLError();
        }

        Optional<const Error*> GLContext::PeekGLError() const {
            return m_errorState.PeekGLError();
        }

        Optional<UniquePtr<Error>> GLContext::PopGLError() {
            return Move(m_errorState.PopGLError());
        }

        Bool GLContext::HasNonGLError() const {
            return m_errorState.HasNonGLError();
        }

        Optional<const Error*> GLContext::PeekNonGLError() const {
            return m_errorState.PeekNonGLError();
        }

        Optional<UniquePtr<Error>> GLContext::PopNonGLError() {
            return Move(m_errorState.PopNonGLError());
        }

        void GLContext::ClearErrors() {
            m_errorState.Clear();
        }

        // Buffer
        void GLContext::GenBufferNames(Uint number, Vector<Uint>& buffers) {
            m_bufferState.GenerateNames(number, buffers);
        }

        const SharedPtr<BufferObject>& GLContext::GetBufferObject(Uint index) {
            return m_bufferState.GetBufferObject(index);
        }

        BindingSlot<BufferObject>& GLContext::GetBufferBindingSlot(BufferTarget target) {
            if (target == BufferTarget::Index) {
                const auto& vao = m_vertexArrayState.GetBoundVertexArray();
                MOBILEGL_ASSERT(vao != nullptr, "No VAO is currently bound when accessing index buffer binding slot.");
                return vao->GetIndexBufferBindingSlot();
            }

            return m_bufferState.GetBindingSlot(target);
        }

        BindingSlotRange1D<BufferObject>& GLContext::GetBufferBindingPoint(BufferTarget target, Uint index) {
            return m_bufferState.GetBindingPoint(target, index);
        }

        const SharedPtr<BufferObject>& GLContext::CreateBufferObject(Uint index) {
            return m_bufferState.CreateBufferObject(index);
        }

        void GLContext::MarkBufferObjectForDeletion(Uint index) {
            if (ValidateBufferObject(index)) {
                // GL semantics: deleting a buffer detaches it only from the CURRENT
                // context's bindings, including the currently bound VAO's attachment
                // points; attachments in other VAOs must survive (the shared_ptr keeps
                // the data object alive, matching the spec's deferred deletion). The
                // previous every-VAO scan was wrong per spec and O(VAOs) per delete —
                // with one VAO per chunk section, vanilla's steady buffer churn made it
                // dominate the render thread and FPS decay over session time.
                auto bufferObject = m_bufferState.GetBufferObject(index);
                const auto& vao = m_vertexArrayState.GetBoundVertexArray();
                if (vao != nullptr) {
                    if (vao->GetIndexBufferBindingSlot().GetBoundObject() == bufferObject) {
                        vao->GetIndexBufferBindingSlot().Bind(nullptr);
                    }
                    for (SizeT j = 0; j < VertexArrayObject::MAX_VERTEX_ATTRIBS; ++j) {
                        if (vao->GetAttribute(j).Buffer == bufferObject) {
                            vao->BindAttributeBuffer(j, nullptr);
                        }
                    }
                }
            }

            m_bufferState.MarkBufferObjectForDeletion(index);
        }

        Bool GLContext::ValidateBufferName(Uint index) const {
            return m_bufferState.ValidateName(index);
        }

        Bool GLContext::ValidateBufferObject(Uint index) const {
            return m_bufferState.ValidateBufferObject(index);
        }

        // VertexArray
        void GLContext::GenVertexArrayNames(Uint number, Vector<Uint>& vertexArrays) {
            m_vertexArrayState.GenerateNames(number, vertexArrays);
        }

        const SharedPtr<VertexArrayObject>& GLContext::GetVertexArrayObject(Uint index) {
            return m_vertexArrayState.GetVertexArrayObject(index);
        }

        void GLContext::BindVertexArray(Uint index) {
            m_vertexArrayState.Bind(index);
        }

        const SharedPtr<VertexArrayObject>& GLContext::CreateVertexArrayObject(Uint index) {
            return m_vertexArrayState.CreateVertexArrayObject(index);
        }

        void GLContext::MarkVertexArrayForDeletion(Uint index) {
            m_vertexArrayState.MarkVertexArrayForDeletion(index);
        }

        Bool GLContext::ValidateVertexArrayName(Uint index) const {
            return m_vertexArrayState.ValidateName(index);
        }

        Bool GLContext::ValidateVertexArrayObject(Uint index) const {
            return m_vertexArrayState.ValidateVertexArrayObject(index);
        }

        const SharedPtr<VertexArrayObject>& GLContext::GetBoundVertexArray() {
            return m_vertexArrayState.GetBoundVertexArray();
        }

        VertexAttribTypeInfo ClassifyVertexAttribType(GLenum glType) {
            switch (glType) {
            case GL_FLOAT: return {VertexAttribBaseType::Float, 1};
            case GL_FLOAT_VEC2: return {VertexAttribBaseType::Float, 2};
            case GL_FLOAT_VEC3: return {VertexAttribBaseType::Float, 3};
            case GL_FLOAT_VEC4: return {VertexAttribBaseType::Float, 4};
            case GL_INT: return {VertexAttribBaseType::Int, 1};
            case GL_INT_VEC2: return {VertexAttribBaseType::Int, 2};
            case GL_INT_VEC3: return {VertexAttribBaseType::Int, 3};
            case GL_INT_VEC4: return {VertexAttribBaseType::Int, 4};
            case GL_UNSIGNED_INT: return {VertexAttribBaseType::Uint, 1};
            case GL_UNSIGNED_INT_VEC2: return {VertexAttribBaseType::Uint, 2};
            case GL_UNSIGNED_INT_VEC3: return {VertexAttribBaseType::Uint, 3};
            case GL_UNSIGNED_INT_VEC4: return {VertexAttribBaseType::Uint, 4};
            default: return {};
            }
        }

        // The three accessors below are reachable from backend draw paths with a location taken from
        // shader reflection, so the bound must be enforced at runtime rather than by MOBILEGL_ASSERT
        // (which expands to nothing outside debug builds).
        void GLContext::SetCurrentVertexAttributeFloat(Uint index, const Array<Float, 4>& value) {
            if (index >= m_currentVertexAttributes.size()) {
                MGLOG_E("SetCurrentVertexAttributeFloat: index %u is out of range", index);
                return;
            }

            auto& current = m_currentVertexAttributes[index];
            current.floatValue = value;
            for (SizeT component = 0; component < value.size(); ++component) {
                current.intValue[component] = static_cast<Int32>(value[component]);
                current.uintValue[component] = static_cast<Uint32>(value[component]);
            }
        }

        void GLContext::SetCurrentVertexAttributeInt(Uint index, const Array<Int32, 4>& value) {
            if (index >= m_currentVertexAttributes.size()) {
                MGLOG_E("SetCurrentVertexAttributeInt: index %u is out of range", index);
                return;
            }

            auto& current = m_currentVertexAttributes[index];
            current.intValue = value;
            for (SizeT component = 0; component < value.size(); ++component) {
                current.floatValue[component] = static_cast<Float>(value[component]);
                current.uintValue[component] = static_cast<Uint32>(value[component]);
            }
        }

        void GLContext::SetCurrentVertexAttributeUint(Uint index, const Array<Uint32, 4>& value) {
            if (index >= m_currentVertexAttributes.size()) {
                MGLOG_E("SetCurrentVertexAttributeUint: index %u is out of range", index);
                return;
            }

            auto& current = m_currentVertexAttributes[index];
            current.uintValue = value;
            for (SizeT component = 0; component < value.size(); ++component) {
                current.floatValue[component] = static_cast<Float>(value[component]);
                current.intValue[component] = static_cast<Int32>(value[component]);
            }
        }

        const CurrentVertexAttributeValue& GLContext::GetCurrentVertexAttribute(Uint index) const {
            static const CurrentVertexAttributeValue defaultValue{};
            if (index >= m_currentVertexAttributes.size()) {
                MGLOG_E("GetCurrentVertexAttribute: index %u is out of range", index);
                return defaultValue;
            }
            return m_currentVertexAttributes[index];
        }

        // Texture
        void GLContext::GenTextureNames(Uint number, Vector<Uint>& textures) {
            m_textureState.GenerateNames(number, textures);
        }

        const SharedPtr<ITextureObject>& GLContext::GetTextureObject(Uint index) {
            return m_textureState.GetTextureObject(index);
        }

        const SharedPtr<ITextureObject>& GLContext::GetDefaultTextureObject(TextureTarget target) const {
            return m_textureState.GetDefaultTextureObject(target);
        }

        const SharedPtr<ITextureObject>& GLContext::CreateTextureObject(Uint index, TextureTarget target) {
            return m_textureState.CreateTextureObject(index, target);
        }

        void GLContext::MarkTextureObjectForDeletion(Uint index) {
            // GL 3.3 core 4.4.2: deleting a texture whose image is attached to the framebuffer
            // that is currently bound acts as if FramebufferTexture* had been called with texture
            // zero for every attachment point it occupied there. Framebuffers that are NOT bound
            // keep the orphaned attachment, so only the bound ones are touched.
            //
            // Without this the framebuffer object goes on holding the deleted texture alive as its
            // attachment, and a later read through that framebuffer returns the dead texture's
            // contents rather than those of whatever the application put in its place - the name
            // it deleted usually comes straight back from the next glGenTextures, so the two are
            // indistinguishable from the outside (KHR-GL32.packed_pixels read a stale gradient).
            if (const auto& textureObject = m_textureState.GetTextureObject(index)) {
                for (SizeT targetIndex = 0; targetIndex < SizeT(FramebufferTarget::FramebufferTargetCount);
                     ++targetIndex) {
                    const auto& framebuffer =
                        GetFramebufferBindingSlot(static_cast<FramebufferTarget>(targetIndex)).GetBoundObject();
                    if (!framebuffer || framebuffer->IsDefaultFramebuffer()) {
                        continue;
                    }
                    const auto& attachments = framebuffer->GetAllAttachmentObjects();
                    for (SizeT i = 0; i < attachments.size(); ++i) {
                        if (attachments[i].IsTexture() && attachments[i].GetTexture() == textureObject) {
                            framebuffer->Detach(static_cast<FramebufferAttachmentType>(i));
                        }
                    }
                }
            }
            m_textureState.MarkTextureObjectForDeletion(index, IsRelaxedSemanticsActive());
        }

        TextureUnit& GLContext::GetTextureUnitObject(Int unit) {
            return m_textureState.GetUnitObject(unit);
        }

        ImageTextureBinding& GLContext::GetImageTextureBinding(Int unit) {
            return m_textureState.GetImageTextureBinding(unit);
        }

        const ImageTextureBinding& GLContext::GetImageTextureBinding(Int unit) const {
            return m_textureState.GetImageTextureBinding(unit);
        }

        Bool GLContext::ValidateTextureName(Uint index) const {
            return m_textureState.ValidateName(index);
        }

        Bool GLContext::ValidateTextureObject(Uint index) const {
            return m_textureState.ValidateTextureObject(index);
        }

        Int GLContext::GetActiveTextureUnit() const {
            return m_textureState.GetActiveTextureUnit();
        }

        void GLContext::SetActiveTextureUnit(Int unit) {
            m_textureState.SetActiveTextureUnit(unit);
        }

        // Program
        Uint GLContext::CreateProgram() {
            return m_programState.CreateProgram();
        }

        Uint GLContext::CreateShader(const ShaderStage stage) {
            return m_programState.CreateShader(stage);
        }

        void GLContext::MarkProgramForDeletion(const Uint index) {
            return m_programState.MarkProgramObjectForDeletion(index);
        }

        void GLContext::MarkShaderForDeletion(const Uint index) {
            return m_programState.MarkShaderObjectForDeletion(index);
        }

        void GLContext::ReleaseShaderNameIfOrphaned(const Uint index) {
            return m_programState.ReleaseShaderNameIfOrphaned(index);
        }

        Bool GLContext::ValidateProgramName(const Uint index) const {
            return m_programState.ValidateProgramObject(index);
        }

        Bool GLContext::ValidateShaderName(const Uint index) const {
            return m_programState.ValidateShaderObject(index);
        }

        const SharedPtr<ProgramObject>& GLContext::GetProgramObject(const Uint index) {
            return m_programState.GetProgramObject(index);
        }

        const SharedPtr<ShaderObject>& GLContext::GetShaderObject(const Uint index) {
            return m_programState.GetShaderObject(index);
        }

        void GLContext::UseProgram(Uint program) {
            return m_programState.UseProgram(program);
        }

        const SharedPtr<ProgramObject>& GLContext::GetCurrentProgram() {
            return m_programState.GetCurrentProgram();
        }

        // RenderState
        Uint GLContext::GetRenderStateParametersVersion() const {
            return m_renderState.GetVersion();
        }

        const RenderStateParameters& GLContext::GetRenderStateParameters() const {
            return m_renderState.GetAllParameters();
        }

        void GLContext::SetViewport(IntVec4 viewport) {
            m_renderState.SetViewport(viewport);
        }

        const IntVec4& GLContext::GetViewport() const {
            return m_renderState.GetViewport();
        }

        void GLContext::SetLineWidth(Float width) {
            m_renderState.SetLineWidth(width);
        }

        Float GLContext::GetLineWidth() const {
            return m_renderState.GetLineWidth();
        }

        void GLContext::SetHint(GLenum target, GLenum mode) {
            m_renderState.SetHint(target, mode);
        }

        GLenum GLContext::GetHint(GLenum target) const {
            return m_renderState.GetHint(target);
        }

        void GLContext::SetPointFadeThresholdSize(Float size) {
            m_renderState.SetPointFadeThresholdSize(size);
        }

        Float GLContext::GetPointFadeThresholdSize() const {
            return m_renderState.GetPointFadeThresholdSize();
        }

        void GLContext::SetPointSpriteCoordOrigin(GLenum origin) {
            m_renderState.SetPointSpriteCoordOrigin(origin);
        }

        GLenum GLContext::GetPointSpriteCoordOrigin() const {
            return m_renderState.GetPointSpriteCoordOrigin();
        }

        void GLContext::SetClampReadColor(GLenum clamp) {
            m_renderState.SetClampReadColor(clamp);
        }

        GLenum GLContext::GetClampReadColor() const {
            return m_renderState.GetClampReadColor();
        }

        void GLContext::SetPolygonMode(GLenum front, GLenum back) {
            m_renderState.SetPolygonMode(front, back);
        }

        GLenum GLContext::GetPolygonModeFront() const {
            return m_renderState.GetPolygonModeFront();
        }

        GLenum GLContext::GetPolygonModeBack() const {
            return m_renderState.GetPolygonModeBack();
        }

        void GLContext::SetPrimitiveRestartIndex(Uint32 index) {
            m_renderState.SetPrimitiveRestartIndex(index);
        }

        Uint32 GLContext::GetPrimitiveRestartIndex() const {
            return m_renderState.GetPrimitiveRestartIndex();
        }

        void GLContext::SetPointSize(Float size) {
            m_renderState.SetPointSize(size);
        }

        void GLContext::SetPatchVertices(Uint vertices) {
            m_renderState.SetPatchVertices(vertices);
        }

        Uint GLContext::GetPatchVertices() const {
            return m_renderState.GetPatchVertices();
        }

        Float GLContext::GetPointSize() const {
            return m_renderState.GetPointSize();
        }

        void GLContext::SetPolygonOffset(Float factor, Float units) {
            m_renderState.SetPolygonOffset(factor, units);
        }

        Float GLContext::GetPolygonOffsetFactor() const {
            return m_renderState.GetPolygonOffsetFactor();
        }

        Float GLContext::GetPolygonOffsetUnits() const {
            return m_renderState.GetPolygonOffsetUnits();
        }

        void GLContext::SetCapability(CapabilityInput cap, Bool enabled) {
            m_renderState.SetCapability(cap, enabled);
        }

        Bool GLContext::IsCapabilityEnabled(CapabilityInput cap) const {
            return m_renderState.IsCapabilityEnabled(cap);
        }

        void GLContext::SetCapabilityIndexed(CapabilityInput cap, Uint index, Bool enabled) {
            m_renderState.SetCapabilityIndexed(cap, index, enabled);
        }

        Bool GLContext::IsCapabilityEnabledIndexed(CapabilityInput cap, Uint index) const {
            return m_renderState.IsCapabilityEnabledIndexed(cap, index);
        }

        void GLContext::SetBlendFunc(BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha,
                                     BlendFactor dstAlpha) {
            m_renderState.SetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
        }

        void GLContext::GetBlendFunc(BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                     BlendFactor& dstAlpha) const {
            m_renderState.GetBlendFunc(srcRGB, dstRGB, srcAlpha, dstAlpha);
        }

        void GLContext::SetBlendFuncIndexed(Uint index, BlendFactor srcRGB, BlendFactor dstRGB, BlendFactor srcAlpha,
                                            BlendFactor dstAlpha) {
            m_renderState.SetBlendFuncIndexed(index, srcRGB, dstRGB, srcAlpha, dstAlpha);
        }

        void GLContext::GetBlendFuncIndexed(Uint index, BlendFactor& srcRGB, BlendFactor& dstRGB, BlendFactor& srcAlpha,
                                            BlendFactor& dstAlpha) const {
            m_renderState.GetBlendFuncIndexed(index, srcRGB, dstRGB, srcAlpha, dstAlpha);
        }

        void GLContext::SetBlendEquation(BlendEquation color, BlendEquation alpha) {
            m_renderState.SetBlendEquation(color, alpha);
        }

        void GLContext::GetBlendEquation(BlendEquation& color, BlendEquation& alpha) const {
            m_renderState.GetBlendEquation(color, alpha);
        }

        void GLContext::SetBlendEquationIndexed(Uint index, BlendEquation color, BlendEquation alpha) {
            m_renderState.SetBlendEquationIndexed(index, color, alpha);
        }

        void GLContext::GetBlendEquationIndexed(Uint index, BlendEquation& color, BlendEquation& alpha) const {
            m_renderState.GetBlendEquationIndexed(index, color, alpha);
        }

        void GLContext::SetLogicOp(LogicOperation logicOp) {
            m_renderState.SetLogicOp(logicOp);
        }

        LogicOperation GLContext::GetLogicOp() const {
            return m_renderState.GetLogicOp();
        }

        void GLContext::SetDepthFunc(DepthTestFunc func) {
            m_renderState.SetDepthFunc(func);
        }

        DepthTestFunc GLContext::GetDepthFunc() const {
            return m_renderState.GetDepthFunc();
        }

        void GLContext::SetDepthMask(Bool flag) {
            m_renderState.SetDepthMask(flag);
        }

        Bool GLContext::GetDepthMask() const {
            return m_renderState.GetDepthMask();
        }

        void GLContext::SetStencilFunc(StencilFace face, DepthTestFunc func, Int ref, Uint32 mask) {
            m_renderState.SetStencilFunc(face, func, ref, mask);
        }

        void GLContext::SetStencilMask(StencilFace face, Uint32 mask) {
            m_renderState.SetStencilMask(face, mask);
        }

        void GLContext::SetStencilOp(StencilFace face, StencilOperation fail, StencilOperation depthFail,
                                     StencilOperation depthPass) {
            m_renderState.SetStencilOp(face, fail, depthFail, depthPass);
        }

        const StencilFaceState& GLContext::GetStencilState(StencilFace face) const {
            return m_renderState.GetStencilState(face);
        }

        void GLContext::SetColorMask(BoolVec4 mask) {
            m_renderState.SetColorMask(mask);
        }

        BoolVec4 GLContext::GetColorMask() const {
            return m_renderState.GetColorMask();
        }

        void GLContext::SetColorMaskIndexed(Uint index, BoolVec4 mask) {
            m_renderState.SetColorMaskIndexed(index, mask);
        }

        BoolVec4 GLContext::GetColorMaskIndexed(Uint index) const {
            return m_renderState.GetColorMaskIndexed(index);
        }

        void GLContext::SetClearColor(FloatVec4 color) {
            m_renderState.SetClearColor(color);
        }

        const FloatVec4& GLContext::GetClearColor() const {
            return m_renderState.GetClearColor();
        }

        void GLContext::SetClearDepth(Float depth) {
            m_renderState.SetClearDepth(depth);
        }

        Float GLContext::GetClearDepth() const {
            return m_renderState.GetClearDepth();
        }

        void GLContext::SetClearStencil(Int stencil) {
            m_renderState.SetClearStencil(stencil);
        }

        Uint32 GLContext::GetClearStencil() const {
            return m_renderState.GetClearStencil();
        }

        void GLContext::SetBlendColor(FloatVec4 color) {
            m_renderState.SetBlendColor(color);
        }

        const FloatVec4& GLContext::GetBlendColor() const {
            return m_renderState.GetBlendColor();
        }

        void GLContext::SetDepthRange(FloatVec2 range) {
            m_renderState.SetDepthRange(range);
        }

        const FloatVec2& GLContext::GetDepthRange() const {
            return m_renderState.GetDepthRange();
        }

        void GLContext::SetSampleCoverage(Float value, Bool invert) {
            m_renderState.SetSampleCoverage(value, invert);
        }

        Float GLContext::GetSampleCoverageValue() const {
            return m_renderState.GetSampleCoverageValue();
        }

        Bool GLContext::GetSampleCoverageInvert() const {
            return m_renderState.GetSampleCoverageInvert();
        }

        void GLContext::SetSampleMaskValue(Uint32 mask) {
            m_renderState.SetSampleMaskValue(mask);
        }

        Uint32 GLContext::GetSampleMaskValue() const {
            return m_renderState.GetSampleMaskValue();
        }

        void GLContext::SetPixelStoreParam(PixelStoreParam param, Int value) {
            m_renderState.SetPixelStoreParam(param, value);
        }

        Int GLContext::GetPixelStoreParam(PixelStoreParam param) const {
            return m_renderState.GetPixelStoreParam(param);
        }

        PixelStoreParameters GLContext::GetPixelStoreParameters(Bool isUnpack) const {
            return m_renderState.GetPixelStoreParameters(isUnpack);
        }

        void GLContext::SetCullFaceMode(CullFaceMode mode) {
            m_renderState.SetCullFaceMode(mode);
        }

        CullFaceMode GLContext::GetCullFaceMode() const {
            return m_renderState.GetCullFaceMode();
        }

        void GLContext::SetFrontFaceMode(FrontFaceMode mode) {
            m_renderState.SetFrontFaceMode(mode);
        }

        FrontFaceMode GLContext::GetFrontFaceMode() const {
            return m_renderState.GetFrontFaceMode();
        }

        void GLContext::SetProvokingVertexMode(ProvokingVertexMode mode) {
            m_renderState.SetProvokingVertexMode(mode);
        }

        ProvokingVertexMode GLContext::GetProvokingVertexMode() const {
            return m_renderState.GetProvokingVertexMode();
        }

        void GLContext::SetScissorBox(IntVec4 box) {
            m_renderState.SetScissorBox(box);
        }

        const IntVec4& GLContext::GetScissorBox() const {
            return m_renderState.GetScissorBox();
        }

        // Framebuffer
        void GLContext::GenFramebufferNames(Uint number, Vector<Uint>& framebuffers) {
            m_framebufferState.GenerateNames(number, framebuffers);
        }

        const SharedPtr<FramebufferObject>& GLContext::GetFramebufferObject(Uint index) {
            return m_framebufferState.GetFramebufferObject(index);
        }

        BindingSlot<FramebufferObject>& GLContext::GetFramebufferBindingSlot(FramebufferTarget target) {
            return m_framebufferState.GetBindingSlot(target);
        }

        const SharedPtr<FramebufferObject>& GLContext::CreateFramebufferObject(Uint index) {
            return m_framebufferState.CreateFramebufferObject(index);
        }

        void GLContext::MarkFramebufferObjectForDeletion(Uint index) {
            m_framebufferState.MarkFramebufferObjectForDeletion(index);
        }

        Bool GLContext::ValidateFramebufferName(Uint index) const {
            return m_framebufferState.ValidateName(index);
        }

        Bool GLContext::ValidateFramebufferObject(Uint index) const {
            return m_framebufferState.ValidateFramebufferObject(index);
        }

        // Sampler
        void GLContext::GenSamplerNames(Uint number, Vector<Uint>& samplers) {
            m_samplerState.GenerateNames(number, samplers);
        }

        const SharedPtr<SamplerObject>& GLContext::GetSamplerObject(Uint index) {
            return m_samplerState.GetSamplerObject(index);
        }

        const SharedPtr<SamplerObject>& GLContext::CreateSamplerObject(Uint index) {
            return m_samplerState.CreateSamplerObject(index);
        }

        void GLContext::MarkSamplerObjectForDeletion(Uint index) {
            // Unbind the sampler from all texture units
            if (ValidateSamplerObject(index)) {
                auto sampler = m_samplerState.GetSamplerObject(index);
                for (Int unit = 0; unit < TextureState::MAX_TEXTURE_IMAGE_UNITS; ++unit) {
                    auto& textureUnit = m_textureState.GetUnitObject(unit);
                    if (textureUnit.GetSamplerObject() == sampler) {
                        textureUnit.SetSamplerObject(nullptr);
                    }
                }
            }
            m_samplerState.MarkSamplerObjectForDeletion(index);
        }

        Bool GLContext::ValidateSamplerName(Uint index) const {
            return m_samplerState.ValidateName(index);
        }

        Bool GLContext::ValidateSamplerObject(Uint index) const {
            return m_samplerState.ValidateSamplerObject(index);
        }

        // Renderbuffer
        void GLContext::GenRenderbufferNames(Uint number, Vector<Uint>& renderbuffers) {
            m_renderbufferState.GenerateNames(number, renderbuffers);
        }

        const SharedPtr<RenderbufferObject>& GLContext::GetRenderbufferObject(Uint index) {
            return m_renderbufferState.GetRenderbufferObject(index);
        }

        BindingSlot<RenderbufferObject>& GLContext::GetRenderbufferBindingSlot(RenderbufferTarget target) {
            return m_renderbufferState.GetBindingSlot(target);
        }

        const SharedPtr<RenderbufferObject>& GLContext::CreateRenderbufferObject(Uint index) {
            return m_renderbufferState.CreateRenderbufferObject(index);
        }

        void GLContext::MarkRenderbufferObjectForDeletion(Uint index) {
            m_renderbufferState.MarkRenderbufferObjectForDeletion(index);
        }

        Bool GLContext::ValidateRenderbufferName(Uint index) const {
            return m_renderbufferState.ValidateName(index);
        }

        Bool GLContext::ValidateRenderbufferObject(Uint index) const {
            return m_renderbufferState.ValidateRenderbufferObject(index);
        }

        void GLContext::SaveBoundTransformFeedbackState() {
            auto& object = m_transformFeedbackObjects[m_boundTransformFeedback];
            for (Uint i = 0; i < MAX_TRANSFORM_FEEDBACK_BUFFERS; ++i) {
                const auto& point = m_bufferState.GetBindingPoint(BufferTarget::TransformFeedback, i);
                object.bindings[i] = {point.GetBoundObject(), point.GetRange(), point.HasExplicitRange()};
            }
            object.active = m_transformFeedbackActive;
            object.paused = m_transformFeedbackPaused;
            object.primitiveMode = m_transformFeedbackPrimitiveMode;
            object.program = m_transformFeedbackProgram;
            object.generation = m_transformFeedbackGeneration;
            object.capturedVertices = m_transformFeedbackCapturedVertices;
            object.inputPrimitives = m_transformFeedbackInputPrimitives;
        }

        void GLContext::RestoreBoundTransformFeedbackState() {
            const auto& object = m_transformFeedbackObjects[m_boundTransformFeedback];
            for (Uint i = 0; i < MAX_TRANSFORM_FEEDBACK_BUFFERS; ++i) {
                auto& point = m_bufferState.GetBindingPoint(BufferTarget::TransformFeedback, i);
                point.Bind(object.bindings[i].buffer);
                if (object.bindings[i].buffer) {
                    point.SetRange(object.bindings[i].range, object.bindings[i].hasExplicitRange);
                } else {
                    point.ClearRange();
                }
            }
            m_transformFeedbackActive = object.active;
            m_transformFeedbackPaused = object.paused;
            m_transformFeedbackPrimitiveMode = object.primitiveMode;
            m_transformFeedbackProgram = object.program;
            // The generation identifies one capture span, and a span belongs to the object
            // that opened it - a backend keys its append state on it, so switching objects
            // has to bring the right one back.
            m_transformFeedbackGeneration = object.generation;
            m_transformFeedbackCapturedVertices = object.capturedVertices;
            m_transformFeedbackInputPrimitives = object.inputPrimitives;
        }

        void GLContext::GenTransformFeedbackNames(Uint number, Vector<Uint>& ids) {
            ids.resize(number);
            if (number == 0) return;
            m_transformFeedbackNames.Generate(number, ids.data());
            // A generated name already denotes an object with the default state, so that a
            // bind never has to distinguish "first use" from any later one.
            for (const Uint id : ids) {
                m_transformFeedbackObjects[id] = {};
            }
        }
        // Program pipeline
        void GLContext::GenProgramPipelineNames(Uint number, Vector<Uint>& pipelines) {
            pipelines.resize(number);
            // Names only: glIsProgramPipeline must answer GL_FALSE until one is bound or created.
            m_programPipelineNames.Generate(number, pipelines.data());
        }

        void GLContext::CreateProgramPipelineObject(Uint index) {
            m_programPipelines[index] = MakeShared<ProgramPipelineObject>(index);
        }

        Bool GLContext::ValidateProgramPipelineName(Uint index) const {
            return index == 0 || m_programPipelineNames.IsValid(index);
        }

        Bool GLContext::IsProgramPipelineObject(Uint index) const {
            if (index == 0 || !m_programPipelineNames.IsValid(index)) return false;
            return m_programPipelines.find(index) != m_programPipelines.end();
        }

        void GLContext::BindProgramPipelineObject(Uint index) {
            if (index != 0 && m_programPipelines.find(index) == m_programPipelines.end()) {
                // First bind is what turns a reserved name into an object.
                m_programPipelines[index] = MakeShared<ProgramPipelineObject>(index);
            }
            m_boundProgramPipeline = index;
        }

        void GLContext::MarkProgramPipelineForDeletion(Uint index) {
            if (index == 0 || !m_programPipelineNames.IsValid(index)) return;
            if (index == m_boundProgramPipeline) {
                m_boundProgramPipeline = 0;
            }
            m_programPipelines.erase(index);
            m_programPipelineNames.Delete(index);
        }

        const SharedPtr<ProgramPipelineObject>& GLContext::GetProgramPipelineObject(Uint index) const {
            static const SharedPtr<ProgramPipelineObject> kNone;
            const auto it = m_programPipelines.find(index);
            return it == m_programPipelines.end() ? kNone : it->second;
        }

        const SharedPtr<ProgramPipelineObject>& GLContext::GetBoundProgramPipeline() const {
            return GetProgramPipelineObject(m_boundProgramPipeline);
        }


        Bool GLContext::ValidateTransformFeedbackName(Uint index) const {
            return index == 0 || m_transformFeedbackNames.IsValid(index);
        }

        void GLContext::BindTransformFeedbackObject(Uint index) {
            if (index == m_boundTransformFeedback) return;
            SaveBoundTransformFeedbackState();
            m_boundTransformFeedback = index;
            m_transformFeedbackObjects[index].everBound = true;
            RestoreBoundTransformFeedbackState();
        }

        Bool GLContext::IsTransformFeedbackObject(Uint index) const {
            if (index == 0 || !m_transformFeedbackNames.IsValid(index)) return false;
            const auto it = m_transformFeedbackObjects.find(index);
            return it != m_transformFeedbackObjects.end() && it->second.everBound;
        }

        void GLContext::MarkTransformFeedbackObjectForDeletion(Uint index) {
            if (index == 0 || !m_transformFeedbackNames.IsValid(index)) return;
            // Deleting the bound object reverts to the default one (GL 4.6 core 13.2.1);
            // its state is dropped rather than saved back into the dying object.
            if (index == m_boundTransformFeedback) {
                m_boundTransformFeedback = 0;
                RestoreBoundTransformFeedbackState();
            }
            m_transformFeedbackObjects.erase(index);
            m_transformFeedbackNames.Delete(index);
        }

        Uint64 GLContext::GetTransformFeedbackRecordedVertices(Uint index) const {
            const auto it = m_transformFeedbackObjects.find(index);
            return it == m_transformFeedbackObjects.end() ? 0 : it->second.recordedVertices;
        }

        Bool GLContext::HasTransformFeedbackCompletedSpan(Uint index) const {
            const auto it = m_transformFeedbackObjects.find(index);
            return it != m_transformFeedbackObjects.end() && it->second.hasCompletedSpan;
        }

        void GLContext::CreateTransformFeedbackObject(Uint index) {
            // glCreateTransformFeedbacks has no bind step to infer existence from, so the name it
            // hands out is already the name of an object (GL 4.6 core 13.2.1).
            m_transformFeedbackObjects[index] = {};
            m_transformFeedbackObjects[index].everBound = true;
        }

        Bool GLContext::IsNamedTransformFeedbackActive(Uint index) const {
            if (index == m_boundTransformFeedback) return m_transformFeedbackActive;
            const auto it = m_transformFeedbackObjects.find(index);
            return it != m_transformFeedbackObjects.end() && it->second.active;
        }

        Bool GLContext::IsNamedTransformFeedbackPaused(Uint index) const {
            if (index == m_boundTransformFeedback) return m_transformFeedbackPaused;
            const auto it = m_transformFeedbackObjects.find(index);
            return it != m_transformFeedbackObjects.end() && it->second.paused;
        }

        NamedTransformFeedbackBinding GLContext::GetNamedTransformFeedbackBinding(Uint index, Uint bufferIndex) const {
            NamedTransformFeedbackBinding result;
            if (bufferIndex >= MAX_TRANSFORM_FEEDBACK_BUFFERS) return result;
            // The bound object's capture bindings live in the context's own binding points, not in
            // the saved copy - that one is only written when the object is swapped out.
            if (index == m_boundTransformFeedback) {
                const auto& point = m_bufferState.GetBindingPoint(BufferTarget::TransformFeedback, bufferIndex);
                result.Buffer = point.GetBoundObject();
                result.Range = point.GetRange();
                result.HasExplicitRange = point.HasExplicitRange();
                return result;
            }
            const auto it = m_transformFeedbackObjects.find(index);
            if (it == m_transformFeedbackObjects.end()) return result;
            const auto& saved = it->second.bindings[bufferIndex];
            result.Buffer = saved.buffer;
            result.Range = saved.range;
            result.HasExplicitRange = saved.hasExplicitRange;
            return result;
        }

        void GLContext::SetNamedTransformFeedbackBinding(Uint index, Uint bufferIndex,
                                                         const SharedPtr<BufferObject>& buffer, Range1D range,
                                                         Bool hasExplicitRange) {
            if (bufferIndex >= MAX_TRANSFORM_FEEDBACK_BUFFERS) return;
            if (index == m_boundTransformFeedback) {
                auto& point = m_bufferState.GetBindingPoint(BufferTarget::TransformFeedback, bufferIndex);
                point.Bind(buffer);
                if (buffer && hasExplicitRange) {
                    point.SetRange(range, true);
                } else {
                    point.ClearRange();
                }
                return;
            }
            auto& object = m_transformFeedbackObjects[index];
            object.bindings[bufferIndex] = {buffer, range, hasExplicitRange};
        }
    } // namespace GLState

    // Leak-at-exit storage; see GlobalObjects.cpp.
    UniquePtr<GLState::GLContext>& pGLContext = *new UniquePtr<GLState::GLContext>();
} // namespace MobileGL::MG_State
