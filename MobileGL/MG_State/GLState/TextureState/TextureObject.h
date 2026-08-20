// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "TextureEnum.h"
#include "MipmapUploadTargetArray.h"
#include "MG_Util/Types.h"
#include "../SamplerState/SamplerObject.h"
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>

namespace MobileGL::MG_State::GLState {
    // Texture objects are always SharedPtr-owned (TextureState creates every instance via
    // MakeShared, including the per-target default objects). enable_shared_from_this lets
    // backends that only receive a reference (e.g. syncing a name-deleted texture kept
    // alive by an FBO attachment) still register a weak liveness reference for GC.
    class ITextureObject : public std::enable_shared_from_this<ITextureObject> {
    public:
        using TargetEnum = TextureTarget;
        virtual ~ITextureObject() = default;

        virtual TextureStorageType GetStorageType() const = 0;

        virtual TextureInternalFormat GetFormat() const = 0;
        virtual TextureTarget GetTarget() const = 0;
        virtual const Vector<TextureUploadTarget>& GetUploadTargets() const = 0;
        virtual IntVec3 GetBaseSize() const = 0;
        virtual const SharedPtr<SamplerObject>& GetSamplerObject() const = 0;
        virtual void SetInternalFormat(TextureInternalFormat format) = 0;
        virtual Bool IsComplete() const = 0;
        virtual Uint GetExternalIndex() const = 0;
        virtual const FloatVec4& GetBorderColor() const = 0;
        virtual void SetBorderColor(const FloatVec4& color) = 0;
        virtual const IntVec4& GetBorderColorI() const = 0;
        virtual void SetBorderColorI(const IntVec4& color) = 0;
        virtual const UintVec4& GetBorderColorUI() const = 0;
        virtual void SetBorderColorUI(const UintVec4& color) = 0;
        virtual TextureSwizzleParam GetSwizzleParam(TextureSwizzleParam param) const = 0;
        virtual void SetSwizzleParam(TextureSwizzleParam param, TextureSwizzleParam value) = 0;
        virtual void SetSwizzleParamRGBA(const Vec4<TextureSwizzleParam>& values) = 0;
        virtual const Vec4<TextureSwizzleParam>& GetAllSwizzleParams() const = 0;
        virtual const UintVec2& GetLevelRange() const = 0;
        virtual void SetBaseLevel(Uint baseLevel) = 0;
        virtual void SetMaxLevel(Uint maxLevel) = 0;
        virtual Bool IsImmutable() const = 0;
        virtual Uint GetImmutableLevels() const = 0;
        virtual void SetImmutableLevels(Uint levels) = 0;
        virtual Uint16 GetTextureParamsVersion() const = 0;
        // Monotonic counter bumped on every CPU-side pixel mutation (see MarkStorageDirty).
        // Backends compare it against a per-resource snapshot to skip re-syncing unchanged
        // textures across draws (e.g. the block atlas bound across a whole terrain batch).
        virtual Uint64 GetContentVersion() const = 0;
        // Monotonic counter bumped on every SHAPE mutation - level sizes, the stored level
        // set, the internal format, the level range (see BumpShapeVersion). Disjoint from the
        // content version on purpose: glTexImage2D(..., nullptr) re-specifies a level's size
        // without dirtying a single texel, so a backend that keys its "nothing changed since
        // the last sync" skip on content alone keeps a resource of the OLD size alive.
        virtual Uint64 GetShapeVersion() const = 0;
        // Answers IsMipmapCompleteForFilter() from a memo. Sampling completeness is a
        // property of the texture's SHAPE - level sizes, level count, level range,
        // internal format - and never of its texel content, but every draw asks about
        // every bound texture, which made recomputing it one of the hottest things both
        // backends did (the walk plus its GetTexelSize calls measured ~8% of the render
        // thread). Shape mutations invalidate the memo; uploads do not.
        virtual Bool IsMipmapCompleteForFilterCached(Bool mipmapped) const = 0;
        virtual Int GetSamples() const = 0;
        virtual void SetSamples(Int samples) = 0;
        virtual Bool HasFixedSampleLocations() const = 0;
        virtual void SetFixedSampleLocations(Bool fixedSampleLocations) = 0;
        virtual Uint64 GetLifetimeId() const = 0;
        // Which aspect of a packed depth/stencil texture a sampler reads (GL 4.6 core 8.10).
        // DEPTH_COMPONENT until set, and meaningless for every other format.
        virtual GLenum GetDepthStencilTextureMode() const = 0;
        virtual void SetDepthStencilTextureMode(GLenum mode) = 0;

    protected:
        virtual Uint GetIndexOfTextureUploadTarget(TextureUploadTarget target) const = 0;
    };

    class TextureObjectBase : public ITextureObject {
    public:
        TextureObjectBase(TextureTarget target, Uint externalIndex);
        virtual ~TextureObjectBase() = default;

        TextureInternalFormat GetFormat() const override;
        TextureTarget GetTarget() const override;
        IntVec3 GetBaseSize() const override;
        const SharedPtr<SamplerObject>& GetSamplerObject() const override;
        void SetInternalFormat(TextureInternalFormat format) override;
        Bool IsComplete() const override;
        Uint GetExternalIndex() const override;
        const FloatVec4& GetBorderColor() const override;
        void SetBorderColor(const FloatVec4& color) override;
        const IntVec4& GetBorderColorI() const override;
        void SetBorderColorI(const IntVec4& color) override;
        const UintVec4& GetBorderColorUI() const override;
        void SetBorderColorUI(const UintVec4& color) override;
        TextureSwizzleParam GetSwizzleParam(TextureSwizzleParam param) const override;
        const Vec4<TextureSwizzleParam>& GetAllSwizzleParams() const override;
        void SetSwizzleParam(TextureSwizzleParam param, TextureSwizzleParam value) override;
        void SetSwizzleParamRGBA(const Vec4<TextureSwizzleParam>& values) override;
        const UintVec2& GetLevelRange() const override;
        void SetBaseLevel(Uint baseLevel) override;
        void SetMaxLevel(Uint maxLevel) override;
        Bool IsImmutable() const override;
        Uint GetImmutableLevels() const override;
        void SetImmutableLevels(Uint levels) override;
        Uint16 GetTextureParamsVersion() const override;
        Uint64 GetContentVersion() const override;
        Uint64 GetShapeVersion() const override;
        Bool IsMipmapCompleteForFilterCached(Bool mipmapped) const override;
        // Bumps the content version without touching per-level storage-dirty flags. Used when the
        // set of defined mip levels grows via GPU-side mip generation (glGenerateMipmap): the level
        // set changed (so a cached sampled view's level range is stale) but no CPU data is dirty.
        void BumpContentVersion();
        Int GetSamples() const override;
        void SetSamples(Int samples) override;
        Bool HasFixedSampleLocations() const override;
        void SetFixedSampleLocations(Bool fixedSampleLocations) override;
        Uint64 GetLifetimeId() const override;
        GLenum GetDepthStencilTextureMode() const override { return m_depthStencilTextureMode; }
        // Bumps the params version like every other backend-visible texture parameter: the mode
        // decides which ASPECT of a packed depth/stencil image a sampler reads, which DirectGLES
        // forwards as a texture parameter and DirectVulkan bakes into the sampled image view. A
        // silent write here would leave both backends showing the aspect they last built.
        void SetDepthStencilTextureMode(GLenum mode) override {
            if (m_depthStencilTextureMode == mode) return;
            m_depthStencilTextureMode = mode;
            ++m_textureParamsVersion;
        }

    protected:
        static Uint64 AllocateLifetimeId();
        // The ONLY way m_shapeVersion may move. Besides invalidating this object's own
        // completeness memo it bumps the context-wide sampling-resolution generation, which is
        // what a backend memo of the resolved per-unit bindings watches: completeness decides
        // whether a bound texture reaches its native target at all, and a shape change is
        // otherwise invisible to such a memo (no bind moved).
        void BumpShapeVersion();

        const Uint m_externalIndex;
        const Uint64 m_lifetimeId;
        const TextureTarget m_target = TextureTarget::Unknown;
        TextureInternalFormat m_internalFormat = TextureInternalFormat::Unknown;
        SharedPtr<SamplerObject> m_sampler = nullptr;
        Vec4<TextureSwizzleParam> m_swizzleParams = {TextureSwizzleParam::Red, TextureSwizzleParam::Green,
                                                     TextureSwizzleParam::Blue, TextureSwizzleParam::Alpha};
        UintVec2 m_levelRange = {0, 1000};
        Uint m_immutableLevels = 0;
        Uint16 m_textureParamsVersion = 0;
        // Bumped by every mutation the completeness answer depends on - internal
        // format, level range, and the stored level set - and by nothing else, so a
        // texel upload leaves the memo below valid.
        Uint64 m_shapeVersion = 1;
        // [0] = the non-mipmapped answer, [1] = the mipmapped one. Mutable because
        // completeness is a query; a zero version means "never computed".
        mutable Uint64 m_completeMemoShapeVersion[2] = {0, 0};
        mutable Bool m_completeMemoValue[2] = {false, false};
        // Starts at 1 so a freshly-created backend resource (snapshot 0) never spuriously
        // matches before its first sync. Bumped only on dirty=true in MarkStorageDirty.
        Uint64 m_contentVersion = 1;
        GLenum m_depthStencilTextureMode = GL_DEPTH_COMPONENT;
        Int m_samples = 0;
        Bool m_fixedSampleLocations = true;
    };

    class TextureObjectMipmap : public TextureObjectBase {
    public:
        TextureObjectMipmap(TextureTarget target, Uint externalIndex) : TextureObjectBase(target, externalIndex) {}

        TextureStorageType GetStorageType() const override { return TextureStorageType::Mipmap; }

        virtual Uint GetMipmapLevelCount() const = 0;
        virtual const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const = 0;
        virtual const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const = 0;
        virtual void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) = 0;
        // AllocateStorage only ever grows the chain. Callers that define the complete level set -
        // glTexStorage*, mip regeneration, or a level-0 respecification at a new size - drop the
        // leftovers explicitly, so a stale tail can never make the texture silently incomplete.
        virtual void TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) = 0;
        virtual void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) = 0;
        virtual void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) = 0;
        virtual void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty = true) = 0;
        virtual Bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const = 0;
        // Sub-image variant of MarkStorageDirty(..., true): backends may then upload
        // only the accumulated region instead of the whole level. The base fallback
        // keeps whole-level semantics for storage classes that do not track regions.
        virtual void MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel, IntVec3 offset,
                                            IntVec3 size) {
            (void)offset;
            (void)size;
            MarkStorageDirty(uploadTarget, mipmapLevel, true);
        }
        // Meaningful only while IsStorageDirty(uploadTarget, mipmapLevel).
        virtual MipmapDirtyRegion GetStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel) const {
            const IntVec3 size = GetMipmapTexelSize(uploadTarget, mipmapLevel);
            return {IntVec3{0, 0, 0}, IntVec3{size.x(), size.y(), std::max(size.z(), 1)}};
        }
        // Scatter detail behind GetStorageDirtyRegion: up to maxRects disjoint rects
        // that together cover every dirty texel, so ~100 sprite writes in a big atlas
        // need not be uploaded as one atlas-sized box. Returns how many rects were
        // written to outRects; 0 means "no list, upload the union box" and is always a
        // safe answer - this base fallback keeps whole-level semantics for storage
        // classes that do not track rects, and backends OPT IN by calling this.
        virtual SizeT GetStorageDirtyRects(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                           MipmapDirtyRegion* outRects, SizeT maxRects) const {
            (void)uploadTarget;
            (void)mipmapLevel;
            (void)outRects;
            (void)maxRects;
            return 0;
        }

        // The compressed image a glCompressedTexImage* call shadowed for this level, kept verbatim
        // next to the texel data rather than instead of it - see MipmapStorage. The texel shadow
        // stays uncompressed and correctly sized, so nothing in the backend upload path has to know
        // these exist; only glGetCompressedTexImage, glGetCompressedTextureImage and the
        // GL_TEXTURE_COMPRESSED* level queries read them.
        virtual void SetMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                              GLenum internalFormat, const void* data, SizeT size) = 0;
        // GL_NONE when this level is not stored compressed.
        virtual GLenum GetMipmapCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel) const = 0;
        virtual SizeT GetMipmapCompressedByteSize(TextureUploadTarget uploadTarget, Uint mipmapLevel) const = 0;
        virtual const void* MapMipmapCompressedImage(TextureUploadTarget uploadTarget, Uint mipmapLevel) const = 0;

        // The compressed internalformat the level was REQUESTED with, recorded even when MobileGL
        // answered it with uncompressed storage (the six generic GL_COMPRESSED_* enums) - see
        // MipmapStorage. Only the entry points GL forbids on a compressed image read it.
        virtual void SetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget, Uint mipmapLevel,
                                                        GLenum internalFormat) = 0;
        // GL_NONE when the level was not requested with a compressed internalformat.
        virtual GLenum GetMipmapRequestedCompressedFormat(TextureUploadTarget uploadTarget,
                                                          Uint mipmapLevel) const = 0;
    };

    // Cheap replacement for dynamic_cast on the hot path: TextureObjectMipmap is the
    // only hierarchy branch whose storage type reports Mipmap, so the tag check makes
    // the static_cast safe.
    inline TextureObjectMipmap* AsMipmapTexture(ITextureObject* texture) {
        return (texture && texture->GetStorageType() == TextureStorageType::Mipmap)
                   ? static_cast<TextureObjectMipmap*>(texture)
                   : nullptr;
    }
    // Whether the texture satisfies the mipmap-completeness rules a minification filter
    // that samples the mip chain imposes (GL 4.6 core 8.17): every level from the base to
    // the effective max must exist at exactly half the previous one's size. `mipmapped` is
    // the effective sampler's answer to "does this filter read more than the base level" -
    // when it is false only base-level completeness matters, which the ordinary
    // IsComplete() already covers. Sampling an incomplete texture returns (0, 0, 0, 1).
    Bool IsMipmapCompleteForFilter(const ITextureObject* texture, Bool mipmapped);

    // The rule above asked as the backends need it: does a lookup on this texture read
    // (0, 0, 0, 1) instead of its contents? `effectiveSampler` is the sampler object bound
    // to the unit when there is one, otherwise the texture's own. A backend answers yes by
    // routing the texture to whatever it already uses for "nothing is bound there".
    Bool SamplesAsIncompleteTexture(const ITextureObject* texture, const SamplerObject* effectiveSampler);

    inline const TextureObjectMipmap* AsMipmapTexture(const ITextureObject* texture) {
        return (texture && texture->GetStorageType() == TextureStorageType::Mipmap)
                   ? static_cast<const TextureObjectMipmap*>(texture)
                   : nullptr;
    }

    // The per-target default texture objects (name 0) sit permanently in every texture unit's
    // binding slots, so "nothing useful bound" is no longer a null slot. While a default texture
    // has never been given an image (its internal format is still Unknown) it can contribute
    // nothing to sampling; backends treat such a binding exactly like the old empty slot and
    // skip per-draw sync/bind work for it. Once an application defines an image on a default
    // texture it loses this shortcut and is synced like any other texture.
    inline Bool IsUndefinedDefaultTexture(const ITextureObject* texture) {
        return texture != nullptr && texture->GetExternalIndex() == 0 &&
               texture->GetFormat() == TextureInternalFormat::Unknown;
    }

    class TextureObjectWithOneMipmap : public TextureObjectMipmap {
    public:
        TextureObjectWithOneMipmap(TextureTarget target, Uint externalIndex)
            : TextureObjectMipmap(target, externalIndex) {}
        virtual ~TextureObjectWithOneMipmap() = default;

        Uint GetMipmapLevelCount() const override;
        const IntVec3 GetMipmapTexelSize(TextureUploadTarget target, Uint mipmapLevel) const override;
        const SizeT GetMipmapByteSize(TextureUploadTarget target, Uint mipmapLevel) const override;
        void AllocateStorage(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapInput input) override;
        void TruncateMipmapLevels(TextureUploadTarget uploadTarget, Uint levelCount) override;
        void UpdateMipmapSubData(TextureUploadTarget uploadTarget, Uint mipmapLevel, DataPtr input) override;
        void* MapMipmapData(TextureUploadTarget uploadTarget, Uint mipmapLevel) override;
        void MarkStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel, Bool dirty) override;
        bool IsStorageDirty(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        void MarkStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel, IntVec3 offset,
                                    IntVec3 size) override;
        MipmapDirtyRegion GetStorageDirtyRegion(TextureUploadTarget uploadTarget, Uint mipmapLevel) const override;
        SizeT GetStorageDirtyRects(TextureUploadTarget uploadTarget, Uint mipmapLevel, MipmapDirtyRegion* outRects,
                                   SizeT maxRects) const override;
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
        MipmapUploadTargetArray<1> m_textureStorage;
    };
} // namespace MobileGL::MG_State::GLState
