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
    // LEVEL offsets are applied by shifting the level index; LAYER offsets cannot be, because the
    // TextureObjectMipmap interface addresses storage as (upload target, level) and a layer lives
    // INSIDE a level's blob. They are applied two other ways instead, and the pair is what keeps
    // a layer-sliced view from corrupting its parent:
    //   * MapMipmapData returns a pointer already advanced to the view's first layer, so a caller
    //     that maps it and then offsets using the extents GetMipmapTexelSize reports - which is
    //     what every glTexSubImage*/glGetTexImage path does - writes the layers it meant to; and
    //   * MarkStorageDirtyRegion moves the region's origin into the OWNER's layer space, which is
    //     the space its upload path walks.
    // Those two are not double-counting: one moves the bytes, the other names which of the
    // owner's layers moved.
    class TextureObjectView : public TextureObjectMipmap {
    public:
        TextureObjectView(Uint externalIndex, TextureTarget target, SharedPtr<ITextureObject> storageOwner,
                          Uint minLevel, Uint numLevels, Uint minLayer, Uint numLayers);

        const SharedPtr<ITextureObject>& GetViewStorageOwner() const override { return m_storageOwner; }
        const Vector<TextureUploadTarget>& GetUploadTargets() const override { return m_uploadTargets; }

        // A view is immutable from birth (GL 4.6 core 8.18 sets its TEXTURE_IMMUTABLE_FORMAT), and
        // unconditionally so: the base class infers immutability from a non-zero level count, and
        // a degenerate view - one the spec's min() composition narrowed to zero levels - would
        // otherwise report GL_FALSE, walk straight past ValidateTextureMutable and let
        // glTexImage2D respecify the PARENT's immutable storage through AllocateStorage.
        Bool IsImmutable() const override { return true; }

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
        // WHICH of the owner's layers a given view-side upload target names, in the owner's layer
        // numbering. For every view target but a cube map that is just this view's layer origin -
        // one target, one layer. A GL_TEXTURE_CUBE_MAP view addresses SIX of the owner's layers at
        // once (GL 4.6 core 8.18), so the face its target token names is an index on top of that
        // origin, and this is the only place that can express it when the owner keeps every layer
        // in one blob: ToOwnerUploadTarget has a single blob to choose from there, so the face
        // would otherwise vanish and all six tokens would read the view's first layer.
        //
        // Every place that turns this view into owner-side bytes goes through here - the blob
        // choice, the byte offset, and the dirty region - so the three cannot disagree about which
        // layer a face is.
        Uint ViewLayerIndex(TextureUploadTarget viewTarget) const;
        Uint ToOwnerLevel(Uint viewLevel) const { return m_viewMinLevel + viewLevel; }
        // The owner's level extent rewritten into this view's shape: the owner's layer axis is
        // collapsed to one slice and the view's own layer count is imposed on the view's layer
        // axis. A GL 1D array carries its layer count in the state-side HEIGHT while every other
        // layered target carries it in z, so the axis is target-dependent.
        IntVec3 ToViewLevelSize(const IntVec3& ownerLevelSize) const;
        // Where this view's first LAYER starts inside the owner's level blob. The layer axis a
        // level's bytes are laid out along is the OWNER's, so this is a slice for a 2D/cube array
        // and a single row for a 1D array; a cube-map owner returns 0 because its faces are
        // separate blobs that ToOwnerUploadTarget already selects between.
        SizeT LayerByteOffset(TextureUploadTarget viewTarget, Uint mipmapLevel) const;
        // A dirty-region origin moved from the view's layer space into the owner's. Takes the view
        // target for the same reason LayerByteOffset does: on a cube-map view the target names the
        // face, and the region has to name the same owner layer the bytes were written to.
        IntVec3 ToOwnerRegionOffset(TextureUploadTarget viewTarget, const IntVec3& viewOffset) const;

        SharedPtr<ITextureObject> m_storageOwner;
        // Non-owning; m_storageOwner keeps it alive and is never a view, so this is set once in
        // the constructor and is null only for the (rejected at creation) buffer-texture case.
        TextureObjectMipmap* m_ownerMipmap = nullptr;
        Vector<TextureUploadTarget> m_uploadTargets;
    };
} // namespace MobileGL::MG_State::GLState
