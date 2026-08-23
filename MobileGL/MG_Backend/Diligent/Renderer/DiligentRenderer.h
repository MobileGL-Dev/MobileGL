// MobileGL - MobileGL/MG_Backend/Diligent/Renderer/DiligentRenderer.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#pragma once

#include <Includes.h>

// X11 (pulled in by Includes.h through Vulkan-Headers) defines True/False as
// macros, which collide with Diligent's Bool constants in BasicTypes.h.
#if defined(True)
#undef True
#endif
#if defined(False)
#undef False
#endif

#include <RefCntAutoPtr.hpp>

namespace Diligent {
    struct IRenderDevice;
    struct IDeviceContext;
    struct ITexture;
    struct ITextureView;
    struct IPipelineState;
    struct IBuffer;
    struct ISampler;
    struct IShaderResourceBinding;
}

namespace MobileGL::MG_State::GLState {
    class ITextureObject;
    class SamplerObject;
    class ProgramObject;
    class RenderbufferObject;
    class FramebufferObject;
}

namespace MobileGL::MG_Backend::DiligentBackend {
    // Minimal real Diligent renderer used to prove the GL 3.2 basic path:
    // clear an offscreen color target, draw a hardcoded triangle, and read
    // pixels back. This is the first concrete rendering layer on top of the
    // Diligent device; it will be expanded into the full MobileGL backend.
    class DiligentRenderer {
    public:
        DiligentRenderer(::Diligent::IRenderDevice* device, ::Diligent::IDeviceContext* context);
        ~DiligentRenderer();

        Bool Initialize(Uint32 width, Uint32 height);
        void Clear(Float r, Float g, Float b, Float a);
        void ClearDepth(Float depth);
        void ClearStencil(Uint32 stencil);
        void DrawTriangle();
        void DrawVertices(const Float* vertices, Uint32 vertexCount);
        // Creates a simple 2D RGBA8 texture from CPU data and makes it available
        // to state PSOs under the shader variable name "g_Texture".
        Bool CreateTestTexture(const void* data, Uint32 width, Uint32 height);
        // Draws using the live MG_State GL context: current program, VAO and
        // bound buffers. This is the front-end emulation entry point.
        void DrawFromState(GLenum mode, GLint first, GLsizei count, GLenum type, const void* indices,
                           GLint baseVertex = 0);
        void ReadPixels(Uint32 x, Uint32 y, Uint32 width, Uint32 height, void* pixels);
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                             GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                             GLbitfield mask, GLenum filter);
        void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFbo,
                                  const SharedPtr<MG_State::GLState::FramebufferObject>& drawFbo,
                                  GLbitfield mask);
        void CopyReadFramebufferToTexture(MG_State::GLState::ITextureObject& dst);
        void CopyTextureSubData(MG_State::GLState::ITextureObject& src, MG_State::GLState::ITextureObject& dst);
        void GenerateMipmap(MG_State::GLState::ITextureObject& texture);
        Bool ReadTextureImage(MG_State::GLState::ITextureObject& texture, Uint32 level, void* pixels);
        void Present();

        ::Diligent::IRenderDevice* GetDevice() const { return m_pDevice; }
        ::Diligent::IDeviceContext* GetContext() const { return m_pContext; }

    private:
        struct TextureResource {
            ::Diligent::RefCntAutoPtr<::Diligent::ITexture> Texture;
            ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> SRV;
            ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> RTV;
            ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> DSV;
            Uint64 ContentVersion = 0;
            Uint16 ParamsVersion = 0;
            Bool IsDepth = false;
        };

        struct SamplerResource {
            ::Diligent::RefCntAutoPtr<::Diligent::ISampler> Sampler;
            Uint16 Version = 0;
        };

        Bool CreateOffscreenTargets();
        Bool CreatePipeline();
        Bool CreateVertexBuffer();
        Bool CreatePipelineFromState(GLenum mode);
        Bool UploadVertexDataFromState(GLenum mode, GLint first, GLsizei count, GLenum type, const void* indices,
                                        GLint baseVertex = 0);
        ::Diligent::ITextureView* SyncTexture(MG_State::GLState::ITextureObject& texture);
        ::Diligent::ITextureView* SyncTextureForAttachment(MG_State::GLState::ITextureObject& texture, Bool depth);
        ::Diligent::ITextureView* SyncRenderbuffer(MG_State::GLState::RenderbufferObject& renderbuffer);
        ::Diligent::ISampler* SyncSampler(const MG_State::GLState::SamplerObject& sampler);
        Bool BindShaderResourcesFromState(const MG_State::GLState::ProgramObject& program);
        Bool UploadUBOFromState(const MG_State::GLState::ProgramObject& program);
        Bool ResolveCurrentRenderTargets(Vector<::Diligent::ITextureView*>& rtvs,
                                         ::Diligent::ITextureView*& dsv);

        ::Diligent::IRenderDevice* m_pDevice = nullptr;
        ::Diligent::IDeviceContext* m_pContext = nullptr;
        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> m_pColorTarget;
        ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> m_pColorRTV;
        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> m_pDepthTarget;
        ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> m_pDepthDSV;
        ::Diligent::RefCntAutoPtr<::Diligent::ITexture> m_pTestTexture;
        ::Diligent::RefCntAutoPtr<::Diligent::ITextureView> m_pTestSRV;
        ::Diligent::RefCntAutoPtr<::Diligent::ISampler> m_pTestSampler;
        ::Diligent::RefCntAutoPtr<::Diligent::IShaderResourceBinding> m_pStateSRB;
        ::Diligent::RefCntAutoPtr<::Diligent::IPipelineState> m_pPSO;
        ::Diligent::RefCntAutoPtr<::Diligent::IBuffer> m_pVertexBuffer;
        ::Diligent::RefCntAutoPtr<::Diligent::IBuffer> m_pUBO;
        Uint32 m_uboSize = 0;
        Uint32 m_uboContentVersion = 0;
        Uint64 m_uboProgramLifetimeId = 0;
        UnorderedMap<Uint64, TextureResource> m_textureCache;
        UnorderedMap<Uint64, SamplerResource> m_samplerCache;
        UnorderedMap<Uint32, TextureResource> m_renderbufferCache;
        UnorderedMap<Uint64, ::Diligent::RefCntAutoPtr<::Diligent::IBuffer>> m_namedUboCache;
        Uint32 m_width = 256;
        Uint32 m_height = 256;
        Uint32 m_lastDrawVertexCount = 0;
        Uint64 m_lastPSOKey = 0;
        Bool m_hasCachedPSO = false;
        Bool m_initialized = false;
    };
} // namespace MobileGL::MG_Backend::DiligentBackend
