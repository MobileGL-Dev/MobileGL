// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObjectView.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureObjectView.h"

#include <algorithm>

namespace MobileGL::MG_State::GLState {
    namespace {
        // Where a target keeps its LAYER count. GL puts a 1D array's layers in the state-side
        // height (that is what glTexImage2D(GL_TEXTURE_1D_ARRAY, width, layers) means, and what
        // TextureObject.cpp's completeness walk assumes); every other layered target keeps them
        // in z. GL_TEXTURE_3D is deliberately None: its depth is a spatial axis, not layers, and
        // ARB_texture_view forbids anything but a full-depth 3D->3D view of it.
        enum class LayerAxis { None, Y, Z };

        LayerAxis LayerAxisOf(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1DArray:
                return LayerAxis::Y;
            case TextureTarget::Texture2DArray:
            case TextureTarget::TextureCubeMapArray:
            case TextureTarget::Texture2DMultisampleArray:
                return LayerAxis::Z;
            default:
                return LayerAxis::None;
            }
        }

        Vector<TextureUploadTarget> UploadTargetsForViewTarget(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
                return {TextureUploadTarget::Texture1D};
            case TextureTarget::Texture2D:
                return {TextureUploadTarget::Texture2D};
            case TextureTarget::Texture3D:
                return {TextureUploadTarget::Texture3D};
            case TextureTarget::TextureRectangle:
                return {TextureUploadTarget::TextureRectangle};
            case TextureTarget::Texture1DArray:
                return {TextureUploadTarget::Texture1DArray};
            case TextureTarget::Texture2DArray:
                return {TextureUploadTarget::Texture2DArray};
            case TextureTarget::TextureCubeMapArray:
                return {TextureUploadTarget::CubeMapArray};
            case TextureTarget::Texture2DMultisample:
                return {TextureUploadTarget::Texture2DMultisample};
            case TextureTarget::Texture2DMultisampleArray:
                return {TextureUploadTarget::Texture2DMultisampleArray};
            case TextureTarget::TextureCubeMap:
                return {TextureUploadTarget::CubeMapPositiveX, TextureUploadTarget::CubeMapNegativeX,
                        TextureUploadTarget::CubeMapPositiveY, TextureUploadTarget::CubeMapNegativeY,
                        TextureUploadTarget::CubeMapPositiveZ, TextureUploadTarget::CubeMapNegativeZ};
            default:
                MOBILEGL_ASSERT(false, "TextureObjectView: target %d cannot be a texture view", (int)target);
                return {TextureUploadTarget::Texture2D};
            }
        }
    } // namespace

    TextureObjectView::TextureObjectView(Uint externalIndex, TextureTarget target,
                                         SharedPtr<ITextureObject> storageOwner, Uint minLevel, Uint numLevels,
                                         Uint minLayer, Uint numLayers)
        : TextureObjectMipmap(target, externalIndex), m_storageOwner(Move(storageOwner)),
          m_uploadTargets(UploadTargetsForViewTarget(target)) {
        MOBILEGL_ASSERT(m_storageOwner != nullptr, "TextureObjectView: storage owner is null");
        MOBILEGL_ASSERT(!m_storageOwner->IsTextureView(),
                        "TextureObjectView: storage owner must be a root texture, not another view");
        m_ownerMipmap = AsMipmapTexture(m_storageOwner.get());
        SetViewLevelLayerRange(minLevel, numLevels, minLayer, numLayers);
        // Held rather than forwarded so the base class's level-range clamp works against the
        // view's OWN level count - TEXTURE_BASE_LEVEL / TEXTURE_MAX_LEVEL on a view are relative
        // to the view. GetImmutableLevels() forwards to the owner for the actual GL query, which
        // GL 4.6 core 8.18 defines as the ORIGINAL texture's value.
        SetImmutableLevels(numLevels);
    }

    Uint TextureObjectView::GetImmutableLevels() const {
        return m_storageOwner->GetImmutableLevels();
    }

    Uint64 TextureObjectView::GetContentVersion() const {
        return m_storageOwner->GetContentVersion();
    }

    Int TextureObjectView::GetSamples() const {
        return m_storageOwner->GetSamples();
    }

    Bool TextureObjectView::HasFixedSampleLocations() const {
        return m_storageOwner->HasFixedSampleLocations();
    }

    TextureUploadTarget TextureObjectView::ToOwnerUploadTarget(TextureUploadTarget viewTarget) const {
        const auto& ownerTargets = m_storageOwner->GetUploadTargets();
        MOBILEGL_ASSERT(!ownerTargets.empty(), "TextureObjectView: storage owner has no upload target");
        if (ownerTargets.size() == 1) {
            // The owner keeps every layer in one blob, so there is nothing to choose.
            return ownerTargets[0];
        }
        // The owner is a cube map: six independent blobs, one per face, and the view's layer
        // index selects among them. A cube-map view of a cube map maps face to face; any other
        // view target addresses layers, which for a cube-map owner ARE its faces.
        const Uint faceCount = static_cast<Uint>(ownerTargets.size());
        Uint face = m_viewMinLayer;
        if (GetTarget() == TextureTarget::TextureCubeMap) {
            for (Uint i = 0; i < m_uploadTargets.size(); ++i) {
                if (m_uploadTargets[i] == viewTarget) {
                    face = m_viewMinLayer + i;
                    break;
                }
            }
        }
        return ownerTargets[std::min(face, faceCount - 1)];
    }

    IntVec3 TextureObjectView::ToViewLevelSize(const IntVec3& ownerLevelSize) const {
        IntVec3 size = ownerLevelSize;
        // Collapse whichever axis the OWNER stored its layers in down to a single slice, then
        // impose this view's own layer count on whichever axis THIS target stores layers in.
        // Doing it in that order makes every legal target pair fall out: 2D_ARRAY->2D clears z,
        // 2D->2D_ARRAY sets it, 2D_ARRAY->2D_ARRAY replaces it, and 3D->3D touches neither
        // (LayerAxis::None on both sides), which is what keeps a 3D view's full depth intact.
        switch (LayerAxisOf(m_storageOwner->GetTarget())) {
        case LayerAxis::Y:
            size.y() = 1;
            break;
        case LayerAxis::Z:
            size.z() = 1;
            break;
        case LayerAxis::None:
            break;
        }
        switch (LayerAxisOf(GetTarget())) {
        case LayerAxis::Y:
            size.y() = static_cast<Int>(m_viewNumLayers);
            break;
        case LayerAxis::Z:
            size.z() = static_cast<Int>(m_viewNumLayers);
            break;
        case LayerAxis::None:
            break;
        }
        return size;
    }

    Uint TextureObjectView::GetMipmapLevelCount() const {
        if (m_ownerMipmap == nullptr) return 0;
        const Uint ownerLevels = m_ownerMipmap->GetMipmapLevelCount();
        if (m_viewMinLevel >= ownerLevels) return 0;
        return std::min(m_viewNumLevels, ownerLevels - m_viewMinLevel);
    }

    const IntVec3 TextureObjectView::GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return {0, 0, 0};
        return ToViewLevelSize(
            m_ownerMipmap->GetMipmapTexelSize(ToOwnerUploadTarget(target), ToOwnerLevel(mipmapLevel)));
    }

    const SizeT TextureObjectView::GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return 0;
        const TextureUploadTarget ownerTarget = ToOwnerUploadTarget(target);
        const Uint ownerLevel = ToOwnerLevel(mipmapLevel);
        const IntVec3 ownerSize = m_ownerMipmap->GetMipmapTexelSize(ownerTarget, ownerLevel);
        const SizeT ownerBytes = m_ownerMipmap->GetMipmapByteSize(ownerTarget, ownerLevel);
        const SizeT ownerTexels = static_cast<SizeT>(std::max(ownerSize.x(), 0)) *
                                  static_cast<SizeT>(std::max(ownerSize.y(), 0)) *
                                  static_cast<SizeT>(std::max(ownerSize.z(), 1));
        if (ownerTexels == 0 || ownerBytes == 0) return 0;
        // Scaled rather than recomputed from a format table: the view's internalformat is
        // required to be in the same view class as the owner's (GL 4.6 core table 8.21), i.e. to
        // have the identical texel size, so bytes-per-texel is shared by construction and the
        // only difference is how many texels the view addresses.
        const IntVec3 viewSize = ToViewLevelSize(ownerSize);
        const SizeT viewTexels = static_cast<SizeT>(std::max(viewSize.x(), 0)) *
                                 static_cast<SizeT>(std::max(viewSize.y(), 0)) *
                                 static_cast<SizeT>(std::max(viewSize.z(), 1));
        return (ownerBytes / ownerTexels) * viewTexels;
    }

    void TextureObjectView::AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) {
        // Unreachable through the API: a view is immutable from birth (GL 4.6 core 8.18 sets its
        // TEXTURE_IMMUTABLE_FORMAT), and every entry point that would allocate is gated on
        // ValidateTextureMutable. Forwarded rather than asserted so an internal caller that
        // re-specifies the storage still hits the one real allocation.
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->AllocateStorage(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel), input);
    }

    void TextureObjectView::TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->TruncateMipmapLevels(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(levelCount));
    }

    void TextureObjectView::UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->UpdateMipmapSubData(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel), input);
    }

    void* TextureObjectView::MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) {
        if (m_ownerMipmap == nullptr) return nullptr;
        return m_ownerMipmap->MapMipmapData(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    void TextureObjectView::MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->MarkStorageDirty(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel), dirty);
    }

    Bool TextureObjectView::IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return false;
        return m_ownerMipmap->IsStorageDirty(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    void TextureObjectView::MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel, IntVec3 offset,
                                                   IntVec3 size) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->MarkStorageDirtyRegion(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel), offset,
                                              size);
    }

    MipmapDirtyRegion TextureObjectView::GetStorageDirtyRegion(TextureUploadTarget uploadTarget,
                                                               Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return {};
        return m_ownerMipmap->GetStorageDirtyRegion(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    void TextureObjectView::SetMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                     GLenum internalFormat, const void* data, SizeT size) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->SetMipmapCompressedImage(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel),
                                                internalFormat, data, size);
    }

    GLenum TextureObjectView::GetMipmapCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return GL_NONE;
        return m_ownerMipmap->GetMipmapCompressedFormat(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    SizeT TextureObjectView::GetMipmapCompressedByteSize(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return 0;
        return m_ownerMipmap->GetMipmapCompressedByteSize(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    const void* TextureObjectView::MapMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return nullptr;
        return m_ownerMipmap->MapMipmapCompressedImage(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel));
    }

    void TextureObjectView::SetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                               GLenum internalFormat) {
        if (m_ownerMipmap == nullptr) return;
        m_ownerMipmap->SetMipmapRequestedCompressedFormat(ToOwnerUploadTarget(uploadTarget), ToOwnerLevel(mipmapLevel),
                                                          internalFormat);
    }

    GLenum TextureObjectView::GetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget,
                                                                 Uint mipmapLevel) const {
        if (m_ownerMipmap == nullptr) return GL_NONE;
        return m_ownerMipmap->GetMipmapRequestedCompressedFormat(ToOwnerUploadTarget(uploadTarget),
                                                                 ToOwnerLevel(mipmapLevel));
    }

    IntVec3 TextureObjectView::GetBaseSize() const {
        if (GetMipmapLevelCount() == 0) return {0, 0, 0};
        return GetMipmapTexelSize(m_uploadTargets[0], 0);
    }

    Bool TextureObjectView::IsComplete() const {
        if (!TextureObjectBase::IsComplete()) return false;
        // The view's own level set is what sampling walks, and it can be shorter than the
        // owner's. Everything below it - that the owner has real storage at all - is the owner's
        // answer, because these texels are its texels.
        if (GetMipmapLevelCount() == 0) return false;
        return m_storageOwner->IsComplete();
    }

    Uint TextureObjectView::GetIndexOfTextureUploadTarget(TextureUploadTarget target) const {
        for (Uint i = 0; i < static_cast<Uint>(m_uploadTargets.size()); ++i) {
            if (m_uploadTargets[i] == target) return i;
        }
        return 0;
    }
} // namespace MobileGL::MG_State::GLState
