// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/ProxyTexture.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Impl::GLImpl::TextureImpl {
    Bool IsProxyTextureTarget(TextureUploadTarget target);

    class ProxyTextureManager {
    public:
        const SharedPtr<MG_State::GLState::ITextureObject>& CreateOrReplaceProxyTextureObject(
            TextureUploadTarget target);
        const SharedPtr<MG_State::GLState::ITextureObject>& GetProxyTextureObject(TextureUploadTarget target);

    private:
        UnorderedMap<TextureUploadTarget, SharedPtr<MG_State::GLState::ITextureObject>> m_proxyTexturesMap;
    };

    extern UniquePtr<ProxyTextureManager>& pProxyTextureManager;
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
