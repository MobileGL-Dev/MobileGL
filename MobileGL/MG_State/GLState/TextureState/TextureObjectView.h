// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObjectView.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureObject.h"

namespace MobileGL::MG_State::GLState {
    // A texture created by glTextureView (ARB_texture_view / GL 4.6 core 8.18): a texture object
    // in every respect - own name, own target, own internal format, own sampler and own
    // per-texture parameters - whose TEXELS are somebody else's. That last part is the whole
    // point of the extension, and the reason this cannot be a plain TextureObject2D with a copy:
    // the application samples the view and the original SIMULTANEOUSLY, reading different aspects
    // or different formats out of one storage, and writes through either name must be visible
    // through the other.
    //
    // So this class owns no MipmapStorage at all. Every storage question is answered by
    // m_storageOwner, shifted by the view's level offset; every parameter question is answered
    // by this object's own TextureObjectBase state. The owner is held by SharedPtr, which is
    // exactly GL's name-deletion rule (5.1.2): glDeleteTextures on the original frees the NAME
    // immediately, but the storage - and every backend resource keyed on the owner object -
    // lives until the last view referencing it is gone too.
    //
    // m_storageOwner is guaranteed never to be a view itself. glTextureView composes a
    // view-of-a-view onto the root at creation time, which is what the spec's additive
    // "<minlevel> plus the value of TEXTURE_VIEW_MIN_LEVEL from the original texture" rule
    // means; one hop therefore always reaches real storage and no recursion is possible.
    //
    // LAYER offsets are deliberately NOT applied here. The TextureObjectMipmap interface
    // addresses storage as (upload target, level) and a layer lives INSIDE a level's blob, so a
    // layer offset is not expressible at this boundary. The entry points that move texels for a
    // view (glTexSubImage*, glGetTexImage) therefore redirect to the owner themselves and add
    // GetViewMinLayer() to the z coordinate there, where it can be said. What this class does
    // apply is the view's layer COUNT, because the level extents it reports are what both
    // backends size their images and views from.
    class TextureObjectView : public TextureObjectMipmap {
    public:
        TextureObjectView(Uint externalIndex, TextureTarget target, SharedPtr<ITextureObject> storageOwner,
                          Uint minLevel, Uint numLevels, Uint minLayer, Uint numLayers);

        const SharedPtr<ITextureObject>& GetViewStorageOwner() const override { return m_storageOwner; }
        const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }

        // GL 4.6 core 8.18: "TEXTURE_IMMUTABLE_LEVELS is set to the value of
        // TEXTURE_IMMUTABLE_LEVELS from the original texture" - NOT to <numlevels>. Kept as a
        // forward rather than in m_immutableLevels so that the base class's level-range clamp
        // keeps using the view's own level count, which is what TEXTURE_BASE_LEVEL /
        // TEXTURE_MAX_LEVEL on a view are relative to.
        Uint GetImmutableLevels() const override;

        // Both follow the storage, not this object: a backend that memoised on the view's own
        // counter would keep serving stale texels after the owner was written through its own
        // name (KHR-GL43.texture_view.coherency is exactly this test).
        Uint64 GetContentVersion() const override;
        Int GetSamples() const override;
        Bool HasFixedSampleLocations() const override;

        Uint GetMipmapLevelCount() const override;
        const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const override;
        const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const override;
        void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) override;
        void TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) override;
        void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) override;
        void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) override;
        void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty) override;
        Bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        void MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel, IntVec3 offset,
                                    IntVec3 size) override;
        MipmapDirtyRegion GetStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        void SetMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel, GLenum internalFormat,
                                      const void* data, SizeT size) override;
        GLenum GetMipmapCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        SizeT GetMipmapCompressedByteSize(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        const void* MapMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        void SetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                GLenum internalFormat) override;
        GLenum GetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;

        IntVec3 GetBaseSize() const override;
        Bool IsComplete() const override;

    protected:
        Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const override;

    private:
        // The owner-side upload target a given view-side one addresses. Only GL_TEXTURE_CUBE_MAP
        // stores its six faces as six separate blobs (MipmapUploadTargetArray<6>); every other
        // target - arrays and cube-map arrays included - keeps all its layers in one blob, so
        // the mapping is "the owner's only target" unless one of the two sides is a cube map.
        TextureUploadTarget ToOwnerUploadTarget(TextureUploadTarget viewTarget) const;
        Uint ToOwnerLevel(Uint viewLevel) const { return m_viewMinLevel + viewLevel; }
        // The owner's level extent rewritten into this view's shape: the owner's layer axis is
        // collapsed to one slice and the view's own layer count is imposed on the view's layer
        // axis. A GL 1D array carries its layer count in the state-side HEIGHT while every other
        // layered target carries it in z, so the axis is target-dependent.
        IntVec3 ToViewLevelSize(const IntVec3& ownerLevelSize) const;

        SharedPtr<ITextureObject> m_storageOwner;
        // Non-owning; m_storageOwner keeps it alive and is never a view, so this is set once in
        // the constructor and is null only for the (rejected at creation) buffer-texture case.
        TextureObjectMipmap* m_ownerMipmap = nullptr;
        Vector<TextureUploadTarget> m_uploadTargets;
    };
} // namespace MobileGL::MG_State::GLState
