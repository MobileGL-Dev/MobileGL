// MobileGL - MobileGL/MG_Backend/Diligent/BackendObject_Diligent.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#include "BackendObject_Diligent.h"
#include "DiligentVulkan.h"
#include "Renderer/DiligentRenderer.h"
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>

#include <EngineFactoryVk.h>
#include <RenderDevice.h>
#include <DeviceContext.h>

#include <exception>

namespace MobileGL::MG_Backend::DiligentBackend {
    namespace {
        const RendererInfo BuildInitialRendererInfo() {
            RendererInfo info;
            info.RendererName = "MobileGL (Diligent/Vulkan)";
            info.BackendName = "Diligent Vulkan";
            info.RendererGLInfo.TargetGLVersion = {3, 2, 0};
            info.RendererGLInfo.TargetGLSLVersion = {1, 50, 0};
            info.RendererGLInfo.IsCompatibilityProfile = false;
            return info;
        }

        DiligentRenderer* GetActiveRenderer() {
            auto* backend = dynamic_cast<BackendObject_Diligent*>(pActiveBackendObject.get());
            return backend != nullptr ? backend->GetRenderer() : nullptr;
        }

        struct DrawArraysIndirectCommand {
            Uint32 Count = 0;
            Uint32 InstanceCount = 0;
            Uint32 First = 0;
            Uint32 BaseInstance = 0;
        };

        struct DrawElementsIndirectCommand {
            Uint32 Count = 0;
            Uint32 InstanceCount = 0;
            Uint32 FirstIndex = 0;
            Int32 BaseVertex = 0;
            Uint32 BaseInstance = 0;
        };

        const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
            auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            if (drawBuffer) {
                drawBuffer->SyncPersistentMappedRange();
                const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
                if (drawBuffer->MappedData() == nullptr || commandOffset + requiredBytes > drawBuffer->GetSize()) {
                    MGLOG_E_ONCE("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                    return nullptr;
                }
                return drawBuffer->MappedData() + commandOffset;
            }
            if (indirect == nullptr) {
                MGLOG_E_ONCE("%s skipped: indirect pointer is null", label);
                return nullptr;
            }
            return reinterpret_cast<const Uint8*>(indirect);
        }

        void Clear(GLbitfield mask) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr) {
                return;
            }
            if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
                const auto& color = MG_State::pGLContext->GetClearColor();
                renderer->Clear(color.x(), color.y(), color.z(), color.w());
            }
            if ((mask & GL_DEPTH_BUFFER_BIT) != 0) {
                renderer->ClearDepth(MG_State::pGLContext->GetClearDepth());
            }
            if ((mask & GL_STENCIL_BUFFER_BIT) != 0) {
                renderer->ClearStencil(MG_State::pGLContext->GetClearStencil());
            }
        }

        void DrawArrays(GLenum mode, GLint first, GLsizei count) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, first, count, 0, nullptr);
            }
        }

        void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, 0, count, type, indices);
            }
        }

        void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                               const void* indices) {
            // The CPU-side UploadVertexDataFromState path already honors the selected index
            // range. start/end only restrict which indices may be referenced; they do not
            // change the vertex buffer layout for this backend.
            (void)start;
            (void)end;
            DrawElements(mode, count, type, indices);
        }

        void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                         const void* indices, GLint basevertex) {
            (void)start;
            (void)end;
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, 0, count, type, indices, basevertex);
            }
        }

        void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr) {
                return;
            }
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] > 0) {
                    renderer->DrawFromState(mode, first[i], count[i], 0, nullptr);
                }
            }
        }

        void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                               GLsizei drawcount) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr) {
                return;
            }
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] > 0) {
                    renderer->DrawFromState(mode, 0, count[i], type, indices[i]);
                }
            }
        }

        void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                    GLint basevertex) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->DrawFromState(mode, 0, count, type, indices, basevertex);
            }
        }

        void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type,
                                         const GLvoid* const* indices, GLsizei drawcount,
                                         const GLint* basevertex) {
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] > 0) {
                    DrawElementsBaseVertex(mode, count[i], type, indices[i],
                                           basevertex != nullptr ? basevertex[i] : 0);
                }
            }
        }

        void DrawArraysIndirect(GLenum mode, const void* indirect) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr) {
                return;
            }
            const auto* bytes = ResolveIndirectCommandBytes(indirect, sizeof(DrawArraysIndirectCommand),
                                                            "DrawArraysIndirect");
            if (bytes == nullptr) {
                return;
            }
            DrawArraysIndirectCommand cmd{};
            std::memcpy(&cmd, bytes, sizeof(cmd));
            if (cmd.Count == 0 || cmd.InstanceCount == 0) {
                return;
            }
            for (Uint32 i = 0; i < cmd.InstanceCount; ++i) {
                renderer->DrawFromState(mode, static_cast<GLint>(cmd.First), static_cast<GLsizei>(cmd.Count),
                                        0, nullptr);
            }
        }

        void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr) {
                return;
            }
            const SizeT indexSize = MG_Util::GetGLTypeSize(type);
            if (indexSize == 0) {
                return;
            }
            const auto* bytes = ResolveIndirectCommandBytes(indirect, sizeof(DrawElementsIndirectCommand),
                                                            "DrawElementsIndirect");
            if (bytes == nullptr) {
                return;
            }
            DrawElementsIndirectCommand cmd{};
            std::memcpy(&cmd, bytes, sizeof(cmd));
            if (cmd.Count == 0 || cmd.InstanceCount == 0) {
                return;
            }
            const void* indices = reinterpret_cast<const void*>(static_cast<SizeT>(cmd.FirstIndex) * indexSize);
            for (Uint32 i = 0; i < cmd.InstanceCount; ++i) {
                renderer->DrawFromState(mode, 0, static_cast<GLsizei>(cmd.Count), type, indices,
                                        static_cast<GLint>(cmd.BaseVertex));
            }
        }

        void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || drawcount <= 0) {
                return;
            }
            const GLsizei realStride = stride == 0 ? static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand)) : stride;
            for (GLsizei i = 0; i < drawcount; ++i) {
                const auto* bytes = ResolveIndirectCommandBytes(
                    static_cast<const Uint8*>(indirect) + static_cast<SizeT>(i) * static_cast<SizeT>(realStride),
                    sizeof(DrawArraysIndirectCommand), "MultiDrawArraysIndirect");
                if (bytes == nullptr) {
                    continue;
                }
                DrawArraysIndirectCommand cmd{};
                std::memcpy(&cmd, bytes, sizeof(cmd));
                if (cmd.Count == 0 || cmd.InstanceCount == 0) {
                    continue;
                }
                for (Uint32 instance = 0; instance < cmd.InstanceCount; ++instance) {
                    renderer->DrawFromState(mode, static_cast<GLint>(cmd.First),
                                            static_cast<GLsizei>(cmd.Count), 0, nullptr);
                }
            }
        }

        void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                       GLsizei stride) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || drawcount <= 0) {
                return;
            }
            const SizeT indexSize = MG_Util::GetGLTypeSize(type);
            if (indexSize == 0) {
                return;
            }
            const GLsizei realStride = stride == 0 ? static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand)) : stride;
            for (GLsizei i = 0; i < drawcount; ++i) {
                const auto* bytes = ResolveIndirectCommandBytes(
                    static_cast<const Uint8*>(indirect) + static_cast<SizeT>(i) * static_cast<SizeT>(realStride),
                    sizeof(DrawElementsIndirectCommand), "MultiDrawElementsIndirect");
                if (bytes == nullptr) {
                    continue;
                }
                DrawElementsIndirectCommand cmd{};
                std::memcpy(&cmd, bytes, sizeof(cmd));
                if (cmd.Count == 0 || cmd.InstanceCount == 0) {
                    continue;
                }
                const void* indices = reinterpret_cast<const void*>(static_cast<SizeT>(cmd.FirstIndex) * indexSize);
                for (Uint32 instance = 0; instance < cmd.InstanceCount; ++instance) {
                    renderer->DrawFromState(mode, 0, static_cast<GLsizei>(cmd.Count), type, indices,
                                            static_cast<GLint>(cmd.BaseVertex));
                }
            }
        }

        void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                          GLsizei maxdrawcount, GLsizei stride) {
            if (MG_State::pGLContext == nullptr) {
                return;
            }
            auto paramBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
            if (!paramBuffer) {
                return;
            }
            paramBuffer->SyncPersistentMappedRange();
            const Uint8* paramData = paramBuffer->MappedData();
            if (paramData == nullptr) {
                return;
            }
            Uint32 actualDrawCount = 0;
            std::memcpy(&actualDrawCount, paramData + static_cast<SizeT>(drawcount), sizeof(actualDrawCount));
            actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
            MultiDrawArraysIndirect(mode, indirect, static_cast<GLsizei>(actualDrawCount), stride);
        }

        void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect,
                                            GLintptr drawcount, GLsizei maxdrawcount, GLsizei stride) {
            if (MG_State::pGLContext == nullptr) {
                return;
            }
            auto paramBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
            if (!paramBuffer) {
                return;
            }
            paramBuffer->SyncPersistentMappedRange();
            const Uint8* paramData = paramBuffer->MappedData();
            if (paramData == nullptr) {
                return;
            }
            Uint32 actualDrawCount = 0;
            std::memcpy(&actualDrawCount, paramData + static_cast<SizeT>(drawcount), sizeof(actualDrawCount));
            actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
            MultiDrawElementsIndirect(mode, type, indirect, static_cast<GLsizei>(actualDrawCount), stride);
        }

        void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || instancecount <= 0) {
                return;
            }
            for (GLsizei i = 0; i < instancecount; ++i) {
                renderer->DrawFromState(mode, first, count, 0, nullptr);
            }
        }

        void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                             GLuint baseinstance) {
            (void)baseinstance;
            DrawArraysInstanced(mode, first, count, instancecount);
        }

        void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                   GLsizei instancecount) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || instancecount <= 0) {
                return;
            }
            for (GLsizei i = 0; i < instancecount; ++i) {
                renderer->DrawFromState(mode, 0, count, type, indices);
            }
        }

        void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                             GLsizei instancecount, GLint basevertex) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || instancecount <= 0) {
                return;
            }
            for (GLsizei i = 0; i < instancecount; ++i) {
                renderer->DrawFromState(mode, 0, count, type, indices, basevertex);
            }
        }

        void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                               GLsizei instancecount, GLuint baseinstance) {
            (void)baseinstance;
            DrawElementsInstanced(mode, count, type, indices, instancecount);
        }

        void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type,
                                                         const void* indices, GLsizei instancecount,
                                                         GLint basevertex, GLuint baseinstance) {
            (void)baseinstance;
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || instancecount <= 0) {
                return;
            }
            for (GLsizei i = 0; i < instancecount; ++i) {
                renderer->DrawFromState(mode, 0, count, type, indices, basevertex);
            }
        }

        void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || value == nullptr) {
                return;
            }
            if (buffer == GL_COLOR && drawbuffer == 0) {
                renderer->Clear(value[0], value[1], value[2], value[3]);
            } else if (buffer == GL_DEPTH && drawbuffer == 0) {
                renderer->ClearDepth(value[0]);
            }
        }

        void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
            if (value == nullptr) {
                return;
            }
            if (buffer == GL_STENCIL) {
                auto* renderer = GetActiveRenderer();
                if (renderer != nullptr) {
                    renderer->ClearStencil(static_cast<Uint32>(value[0]));
                }
                return;
            }
            Float color[4] = {
                static_cast<Float>(value[0]) / 255.0f,
                static_cast<Float>(value[1]) / 255.0f,
                static_cast<Float>(value[2]) / 255.0f,
                static_cast<Float>(value[3]) / 255.0f,
            };
            ClearBufferfv(buffer, drawbuffer, color);
        }

        void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
            if (value == nullptr) {
                return;
            }
            if (buffer == GL_STENCIL) {
                auto* renderer = GetActiveRenderer();
                if (renderer != nullptr) {
                    renderer->ClearStencil(value[0]);
                }
                return;
            }
            Float color[4] = {
                static_cast<Float>(value[0]) / 255.0f,
                static_cast<Float>(value[1]) / 255.0f,
                static_cast<Float>(value[2]) / 255.0f,
                static_cast<Float>(value[3]) / 255.0f,
            };
            ClearBufferfv(buffer, drawbuffer, color);
        }

        void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || buffer != GL_DEPTH_STENCIL) {
                return;
            }
            (void)drawbuffer;
            renderer->ClearDepth(depth);
            renderer->ClearStencil(static_cast<Uint32>(stencil));
        }

        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || pixels == nullptr) {
                return;
            }
            // The Diligent backend's offscreen targets are RGBA8; the frontend currently
            // uses this entry for the common GL_RGBA/GL_UNSIGNED_BYTE readback path.
            if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
                return;
            }
            renderer->ReadPixels(static_cast<Uint32>(x), static_cast<Uint32>(y),
                                 static_cast<Uint32>(width), static_cast<Uint32>(height), pixels);
        }

        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                             GLbitfield mask, GLenum filter) {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1,
                                          mask, filter);
            }
        }

        void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                            GLsizei width, GLsizei height, GLint border) {
            (void)level;
            (void)internalformat;
            (void)x;
            (void)y;
            (void)width;
            (void)height;
            (void)border;
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || target != GL_TEXTURE_2D) {
                return;
            }
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto texture = unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
            if (texture) {
                renderer->CopyReadFramebufferToTexture(*texture);
            }
        }

        void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                               GLsizei width, GLsizei height) {
            (void)level;
            (void)xoffset;
            (void)yoffset;
            (void)x;
            (void)y;
            (void)width;
            (void)height;
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || target != GL_TEXTURE_2D) {
                return;
            }
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto texture = unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
            if (texture) {
                renderer->CopyReadFramebufferToTexture(*texture);
            }
        }

        void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || target != GL_TEXTURE_2D ||
                format != GL_RGBA || type != GL_UNSIGNED_BYTE || pixels == nullptr) {
                return;
            }
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto texture = unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
            if (texture) {
                renderer->ReadTextureImage(*texture, static_cast<Uint32>(level), pixels);
            }
        }

        void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                             TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                             GLsizei bufSize, GLvoid* pixels) {
            (void)uploadTarget;
            (void)bufSize;
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || !texture || format != GL_RGBA || type != GL_UNSIGNED_BYTE ||
                pixels == nullptr) {
                return;
            }
            renderer->ReadTextureImage(*texture, static_cast<Uint32>(level), pixels);
        }

        void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                              GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                              const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                              GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                              GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
            (void)srcTarget;
            (void)srcLevel;
            (void)srcX;
            (void)srcY;
            (void)srcZ;
            (void)dstTarget;
            (void)dstLevel;
            (void)dstX;
            (void)dstY;
            (void)dstZ;
            (void)srcWidth;
            (void)srcHeight;
            (void)srcDepth;
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr && srcTexture && dstTexture) {
                renderer->CopyTextureSubData(*srcTexture, *dstTexture);
            }
        }

        void GenerateMipmap(GLenum target) {
            auto* renderer = GetActiveRenderer();
            if (renderer == nullptr || MG_State::pGLContext == nullptr || target != GL_TEXTURE_2D) {
                return;
            }
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            auto texture = unit.GetBindingSlot(TextureTarget::Texture2D).GetBoundObject();
            if (texture) {
                renderer->GenerateMipmap(*texture);
            }
        }

        void Present() {
            auto* renderer = GetActiveRenderer();
            if (renderer != nullptr) {
                renderer->Present();
            }
        }
    } // namespace

    BackendObject_Diligent::BackendObject_Diligent()
        : m_rendererInfo(BuildInitialRendererInfo()) {}

    BackendObject_Diligent::~BackendObject_Diligent() {
        m_pRenderer.reset();
        m_pContext.Release();
        m_pDevice.Release();
        m_pFactoryVk = nullptr;
    }

    Bool BackendObject_Diligent::CreateDiligentDevice() {
        if (m_pDevice && m_pContext) {
            return true;
        }

        try {
            if (m_pFactoryVk == nullptr) {
                m_pFactoryVk = ::Diligent::GetEngineFactoryVk();
                if (m_pFactoryVk == nullptr) {
                    MGLOG_E("Diligent: failed to load Vulkan engine factory");
                    return false;
                }
                m_pFactoryVk->SetBreakOnError(false);
            }

            ::Diligent::Uint32 numAdapters = 0;
            m_pFactoryVk->EnumerateAdapters(::Diligent::Version{}, numAdapters, nullptr);
            if (numAdapters == 0) {
                MGLOG_W("Diligent: no Vulkan adapters available; skipping device creation");
                return false;
            }

            ::Diligent::EngineVkCreateInfo engineCI;
            ::Diligent::ImmediateContextCreateInfo ctxCI;
            ctxCI.Name = "MobileGL Diligent Main Context";
            ctxCI.QueueId = 0;
            ctxCI.Priority = ::Diligent::QUEUE_PRIORITY_MEDIUM;
            engineCI.NumImmediateContexts = 1;
            engineCI.pImmediateContextInfo = &ctxCI;

            ::Diligent::IRenderDevice* pDevice = nullptr;
            ::Diligent::IDeviceContext* pContext = nullptr;
            m_pFactoryVk->CreateDeviceAndContextsVk(engineCI, &pDevice, &pContext);
            if (pDevice == nullptr || pContext == nullptr) {
                MGLOG_E("Diligent: failed to create Vulkan device/context");
                return false;
            }

            m_pDevice.Attach(pDevice);
            m_pContext.Attach(pContext);
            MGLOG_I("Diligent: Vulkan device created");
            return true;
        } catch (const std::exception& e) {
            MGLOG_W("Diligent: Vulkan device creation failed: %s", e.what());
            return false;
        } catch (...) {
            MGLOG_W("Diligent: Vulkan device creation failed");
            return false;
        }
    }

    void BackendObject_Diligent::Initialize() {
        if (m_initialized) {
            return;
        }
        if (!CreateDiligentDevice()) {
            MGLOG_W("Diligent: backend initialization failed");
            return;
        }

        m_pRenderer = std::make_unique<DiligentRenderer>(m_pDevice, m_pContext);
        if (!m_pRenderer->Initialize(256, 256)) {
            MGLOG_W("Diligent: renderer initialization failed");
            m_pRenderer.reset();
            return;
        }

        m_functions.GL.Clear = Clear;
        m_functions.GL.DrawArrays = DrawArrays;
        m_functions.GL.DrawElements = DrawElements;
        m_functions.GL.DrawElementsBaseVertex = DrawElementsBaseVertex;
        m_functions.GL.DrawRangeElements = DrawRangeElements;
        m_functions.GL.DrawRangeElementsBaseVertex = DrawRangeElementsBaseVertex;
        m_functions.GL.MultiDrawArrays = MultiDrawArrays;
        m_functions.GL.MultiDrawElements = MultiDrawElements;
        m_functions.GL.MultiDrawElementsBaseVertex = MultiDrawElementsBaseVertex;
        m_functions.GL.DrawArraysInstanced = DrawArraysInstanced;
        m_functions.GL.DrawArraysInstancedBaseInstance = DrawArraysInstancedBaseInstance;
        m_functions.GL.DrawElementsInstanced = DrawElementsInstanced;
        m_functions.GL.DrawElementsInstancedBaseVertex = DrawElementsInstancedBaseVertex;
        m_functions.GL.DrawElementsInstancedBaseInstance = DrawElementsInstancedBaseInstance;
        m_functions.GL.DrawElementsInstancedBaseVertexBaseInstance = DrawElementsInstancedBaseVertexBaseInstance;
        m_functions.GL.DrawArraysIndirect = DrawArraysIndirect;
        m_functions.GL.DrawElementsIndirect = DrawElementsIndirect;
        m_functions.GL.MultiDrawArraysIndirect = MultiDrawArraysIndirect;
        m_functions.GL.MultiDrawElementsIndirect = MultiDrawElementsIndirect;
        m_functions.GL.MultiDrawArraysIndirectCount = MultiDrawArraysIndirectCount;
        m_functions.GL.MultiDrawElementsIndirectCount = MultiDrawElementsIndirectCount;
        m_functions.GL.ClearBufferfv = ClearBufferfv;
        m_functions.GL.ClearBufferfi = ClearBufferfi;
        m_functions.GL.ClearBufferiv = ClearBufferiv;
        m_functions.GL.ClearBufferuiv = ClearBufferuiv;
        m_functions.GL.BlitFramebuffer = BlitFramebuffer;
        m_functions.GL.CopyTexImage2D = CopyTexImage2D;
        m_functions.GL.CopyTexSubImage2D = CopyTexSubImage2D;
        m_functions.GL.CopyImageSubData = CopyImageSubData;
        m_functions.GL.GenerateMipmap = GenerateMipmap;
        m_functions.GL.GetTexImage = GetTexImage;
        m_functions.GL.GetTextureImage = GetTextureImage;
        m_functions.GL.ReadPixels = ReadPixels;
        m_functions.Present = Present;

        m_initialized = true;
    }

    DiligentRenderer* BackendObject_Diligent::GetRenderer() {
        return m_pRenderer.get();
    }

    Bool BackendObject_Diligent::InitCapabilities() {
        // Skeleton: no format probing yet. The backend advertises GL 3.2 core
        // capability, and the capability tables will be filled as resource
        // creation paths are ported.
        m_backendCapabilitiesInitialized = true;
        return true;
    }

    Bool BackendObject_Diligent::InitWindowSurface() {
        // Skeleton: no native swapchain creation yet.
        return true;
    }

    const RendererInfo& BackendObject_Diligent::GetRendererInfo() const {
        return m_rendererInfo;
    }

    String BackendObject_Diligent::GetBackendAPIVersionString() const {
        return "Diligent Vulkan 0.1 (GL 3.2 skeleton)";
    }

    const GlobalBackendFunctionsTable& BackendObject_Diligent::GetBackendFunctions() const {
        return m_functions;
    }

    const DynamicBackendParameters& BackendObject_Diligent::GetDynamicParameters() const {
        return m_dynamicParameters;
    }

    BackendType BackendObject_Diligent::GetBackendType() const {
        return BackendType::DiligentVulkan;
    }
} // namespace MobileGL::MG_Backend::DiligentBackend
