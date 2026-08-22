// MobileGL - MobileGL/MG_State/GLState/TextureState/TextureState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Util/Types.h"
#include "TextureUnit.h"

namespace MobileGL::MG_State::GLState {
    struct ImageTextureBinding {
        SharedPtr<ITextureObject> Texture;
        GLint Level = 0;
        GLboolean Layered = GL_FALSE;
        GLint Layer = 0;
        GLenum Access = GL_READ_ONLY;
        GLenum Format = GL_R8;
        Uint16 Version = 0;

        void Bind(SharedPtr<ITextureObject> texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                  GLenum format) {
            Texture = Move(texture);
            Level = level;
            Layered = layered;
            Layer = layer;
            Access = access;
            Format = format;
            ++Version;
        }
    };

    class TextureState {
    public:
        // Capacity of the combined texture-unit state arrays (indexed by glActiveTexture unit).
        static constexpr int MAX_TEXTURE_IMAGE_UNITS = 192;
        // Per-stage sampler limit advertised through GL_MAX_TEXTURE_IMAGE_UNITS /
        // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS. Held at the desktop-driver value (32) so it never exceeds
        // host-side fixed arrays sized off this query -- e.g. Minecraft's 128-entry Blaze3D
        // GlStateManager.TEXTURES[], which Iris iterates over in CompositeRenderer.renderAll.
        static constexpr int MAX_PER_STAGE_TEXTURE_IMAGE_UNITS = 32;

        TextureState();
        void GenerateNames(Uint number, Vector<Uint>& textures);
        const SharedPtr<ITextureObject>& CreateTextureObject(Uint index, TextureTarget target);
        // glTextureView (GL 4.6 core 8.18). `storageOwner` must already be a texture with
        // immutable storage and must NOT itself be a view - the caller composes a view-of-a-view
        // onto the root first, and passes the composed (root-relative) level/layer range here.
        const SharedPtr<ITextureObject>& CreateTextureViewObject(Uint index, TextureTarget target,
                                                                 const SharedPtr<ITextureObject>& storageOwner,
                                                                 Uint minLevel, Uint numLevels, Uint minLayer,
                                                                 Uint numLayers);
        const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
        // The context's default texture object (name 0) for `target`. GL 3.3 core 3.8: texture
        // zero names a real, per-target texture object shared by every texture unit; binding 0
        // binds it, and image/parameter calls on it must work like on any texture. It is not a
        // GenTextures name: it lives outside m_textureObjects (so glIsTexture(0) stays GL_FALSE
        // and by-name lookups keep failing for 0) and can never be deleted.
        const SharedPtr<ITextureObject>& GetDefaultTextureObject(TextureTarget target) const;
        TextureUnit& GetUnitObject(Int unit);
        ImageTextureBinding& GetImageTextureBinding(Int unit);
        const ImageTextureBinding& GetImageTextureBinding(Int unit) const;
        Int GetActiveTextureUnit() const;
        void SetActiveTextureUnit(Int unit);
        void MarkTextureObjectForDeletion(Uint index, Bool keepUnboundReservation);
        Bool ValidateName(Uint index) const;
        Bool ValidateTextureObject(Uint index) const;

        // High-water mark of texture units ever touched by a texture or sampler bind.
        // Units above it have provably-empty binding slots, so per-draw backend scans
        // can stop there instead of walking all MAX_TEXTURE_IMAGE_UNITS units.
        void NoteUnitTouched(Int unit, Bool bindingChanged = true) {
            if (unit > m_maxTouchedUnit && unit < MAX_TEXTURE_IMAGE_UNITS) m_maxTouchedUnit = unit;
            // Every texture/sampler bind entry point (glBindTexture / glBindTextureUnit /
            // glBindTextures / glBindSampler) routes through here, so bumping the generation here
            // - plus in MarkTextureObjectForDeletion for delete-unbind - covers every change to
            // which texture is bound at which unit. A backend that has cached the per-draw
            // sampled-texture set can compare this against a snapshot to skip re-resolving it when
            // no bind changed (the block atlas + lightmap stay bound across a whole terrain batch).
            // Re-binding the object a slot already holds changes nothing that the generation
            // guards; such callers pass bindingChanged=false so only the high-water mark advances
            // and the backend fast path survives the redundant re-binds apps issue every frame.
            if (bindingChanged) ++m_textureBindGeneration;
        }
        Int GetMaxTouchedUnit() const { return m_maxTouchedUnit; }
        Uint64 GetTextureBindGeneration() const { return m_textureBindGeneration; }
        void BumpTextureBindGeneration() { ++m_textureBindGeneration; }

        // Sibling of the bind generation for everything that changes WHICH native texture a
        // backend ends up putting on a unit WITHOUT any binding moving. Two families feed it:
        // a texture's SHAPE (internal format, stored level set, level range - all that
        // mipmap-completeness is computed from) and any sampler object's parameters (MIN_FILTER
        // decides whether the mip chain is read at all, and an incomplete-for-the-filter texture
        // is deliberately left unbound so it samples as (0,0,0,1)). Deliberately coarse - ANY
        // texture, ANY sampler - so that no mutation can slip past a per-unit binding memo; the
        // setters that feed it all early-out when the value is unchanged, so the redundant
        // glTexParameteri calls applications issue every frame do not churn it. Kept separate
        // from the bind generation because the sampled texture SET is unaffected by these, and
        // the Vulkan backend's set memo keys on that one.
        Uint64 GetSamplingResolutionGeneration() const { return m_samplingResolutionGeneration; }
        void BumpSamplingResolutionGeneration() { ++m_samplingResolutionGeneration; }

        // Globally-unique, never-reused id of THIS texture state, i.e. of the context that owns
        // it. Both generations above restart at 0 with a new context, so a backend memo keyed on
        // them alone would accept a destroyed-and-recreated context whose counters happen to line
        // up - and the heap address is no help, since a context freed and remade lands on it
        // again (the unit tests do exactly that between cases).
        Uint64 GetContextId() const { return m_contextId; }

    private:
        static Uint64 AllocateContextId();

        const Uint64 m_contextId;
        Uint64 m_textureBindGeneration = 0;
        Uint64 m_samplingResolutionGeneration = 0;
        Int m_maxTouchedUnit = -1;
        Int m_activeTextureUnit = 0;
        Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS> m_textureUnits;
        Array<ImageTextureBinding, MAX_TEXTURE_IMAGE_UNITS> m_imageTextureBindings;
        IndexGenerator<Uint> m_indexGenerator;
        UnorderedMap<GLuint, SharedPtr<ITextureObject>> m_textureObjects;
        // One default texture object (external name 0) per target, created with the context and
        // immortal for its lifetime; the initial binding of every unit/target slot.
        Array<SharedPtr<ITextureObject>, (int)TextureTarget::TextureTargetCount> m_defaultTextureObjects;
    };
} // namespace MobileGL::MG_State::GLState
