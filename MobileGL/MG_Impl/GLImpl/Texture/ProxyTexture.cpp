// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/ProxyTexture.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProxyTexture.h"
#include <MG_State/GLState/TextureState/TextureObject1D.h>
#include <MG_State/GLState/TextureState/TextureObject2D.h>
#include <MG_State/GLState/TextureState/TextureObject2DCube.h>
#include <MG_State/GLState/TextureState/TextureObject3D.h>
#include <MG_State/GLState/TextureState/TextureObjectStubs.h>

namespace MobileGL::MG_Impl::GLImpl::TextureImpl {
    // Leak-at-exit storage; see GlobalObjects.cpp.
    UniquePtr<ProxyTextureManager>& pProxyTextureManager = *new UniquePtr<ProxyTextureManager>();

    Bool IsProxyTextureTarget(TextureUploadTarget target) {
        switch (target) {
        case TextureUploadTarget::ProxyCubeMap:
        case TextureUploadTarget::ProxyTexture1DArray:
        case TextureUploadTarget::ProxyTexture2DArray:
        case TextureUploadTarget::ProxyTexture1D:
        case TextureUploadTarget::ProxyTexture2D:
        case TextureUploadTarget::ProxyTexture3D:
        case TextureUploadTarget::ProxyTexture2DMultisample:
        case TextureUploadTarget::ProxyTexture2DMultisampleArray:
        case TextureUploadTarget::ProxyTextureRectangle:
        case TextureUploadTarget::ProxyCubeMapArray:
            return true;
        default:
            return false;
        }
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& ProxyTextureManager::CreateOrReplaceProxyTextureObject(
        TextureUploadTarget target) {
        auto it = m_proxyTexturesMap.find(target);
        if (it != m_proxyTexturesMap.end()) {
            m_proxyTexturesMap.erase(it);
        }
        auto& obj = m_proxyTexturesMap[target];
        switch (target) {
        case TextureUploadTarget::ProxyTexture1D:
        case TextureUploadTarget::ProxyTexture1DArray:
            obj = MakeShared<MG_State::GLState::TextureObject1D>(0);
            break;
        case TextureUploadTarget::ProxyTexture2D:
            obj = MakeShared<MG_State::GLState::TextureObject2D>(0);
            break;
        case TextureUploadTarget::ProxyTexture3D:
            obj = MakeShared<MG_State::GLState::TextureObject3D>(0);
            break;
        case TextureUploadTarget::ProxyCubeMap:
            obj = MakeShared<MG_State::GLState::TextureObject2DCube>(0);
            break;
        case TextureUploadTarget::ProxyTextureRectangle:
            obj = MakeShared<MG_State::GLState::TextureObjectRectangle>(0);
            break;
        case TextureUploadTarget::ProxyTexture2DArray:
            obj = MakeShared<MG_State::GLState::TextureObject2DArray>(0);
            break;
        case TextureUploadTarget::ProxyCubeMapArray:
            obj = MakeShared<MG_State::GLState::TextureObjectCubeMapArray>(0);
            break;
        case TextureUploadTarget::ProxyTexture2DMultisample:
            obj = MakeShared<MG_State::GLState::TextureObject2DMultisample>(0);
            break;
        case TextureUploadTarget::ProxyTexture2DMultisampleArray:
            obj = MakeShared<MG_State::GLState::TextureObject2DMultisampleArray>(0);
            break;
        default:
            obj = MakeShared<MG_State::GLState::TextureObject2D>(0);
            break;
        }
        return obj;
    }

    const SharedPtr<MG_State::GLState::ITextureObject>& ProxyTextureManager::GetProxyTextureObject(
        TextureUploadTarget target) {
        auto it = m_proxyTexturesMap.find(target);
        if (it != m_proxyTexturesMap.end()) {
            return it->second;
        }
        static SharedPtr<MG_State::GLState::ITextureObject> nullTextureObject = nullptr;
        return nullTextureObject;
    }
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
