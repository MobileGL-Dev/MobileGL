// MobileGL - MobileGL/MG_Impl/Init.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>

#include "GLImpl/Texture/ProxyTexture.h"
#include "GLImpl/Framebuffer/GL_Framebuffer.h"
#if defined(__APPLE__)
#include "NSOpenGLImpl/NSOpenGLImpl.h"
#endif

#include <MG_State/GLState/TextureState/TextureObject1D.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_State/GLState/TextureState/TextureObject3D.h>

namespace MobileGL::MG_Impl {
    void Init() {
        MGLOG_D("Initializing MobileGL Implementation...");
        GLImpl::TextureImpl::pProxyTextureManager = MakeUnique<GLImpl::TextureImpl::ProxyTextureManager>();

        // TODO: get real info in EGL
        auto& fbo0 = MG_State::pGLContext->CreateFramebufferObject(0);
        auto colorTex = MakeShared<MG_State::GLState::TextureObject2D>(0);
        colorTex->SetInternalFormat(TextureInternalFormat::RGBA8);
        colorTex->AllocateStorage(TextureUploadTarget::Texture2D, 0, {{512, 512, 1}, 0});
        // colorTex->SetMipmapLevel({{512, 512, 1}, 0, false, 0, {nullptr, 0}});
        auto depthTex = MakeShared<MG_State::GLState::TextureObject2D>(0);
        depthTex->SetInternalFormat(TextureInternalFormat::Depth32FStencil8);
        depthTex->AllocateStorage(TextureUploadTarget::Texture2D, 0, {{512, 512, 1}, 0});
        // depthTex->SetMipmapLevel({{512, 512, 1}, 0, false, 0, {nullptr, 0}});
        auto stencilTex = MakeShared<MG_State::GLState::TextureObject2D>(0);
        stencilTex->SetInternalFormat(TextureInternalFormat::Depth32FStencil8);
        stencilTex->AllocateStorage(TextureUploadTarget::Texture2D, 0, {{512, 512, 1}, 0});
        // stencilTex->SetMipmapLevel({{512, 512, 1}, 0, false, 0, {nullptr, 0}});
        fbo0->AttachTexture(FramebufferAttachmentType::Color0, colorTex);
        fbo0->AttachTexture(FramebufferAttachmentType::FrontLeft, colorTex);
        fbo0->AttachTexture(FramebufferAttachmentType::FrontRight, colorTex);
        fbo0->AttachTexture(FramebufferAttachmentType::BackLeft, colorTex);
        fbo0->AttachTexture(FramebufferAttachmentType::BackRight, colorTex);
        fbo0->AttachTexture(FramebufferAttachmentType::Depth, depthTex);
        fbo0->AttachTexture(FramebufferAttachmentType::Stencil, stencilTex);
        GLImpl::FramebufferImpl::pDefaultFramebufferInfo =
            MakeUnique<GLImpl::FramebufferImpl::DefaultFramebufferInfo>(fbo0, colorTex, depthTex, stencilTex);
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).Bind(fbo0);
        MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).Bind(fbo0);
#if defined(__APPLE__)
        NSOpenGLImpl::InstallHooks();
#endif
    }
} // namespace MobileGL::MG_Impl
