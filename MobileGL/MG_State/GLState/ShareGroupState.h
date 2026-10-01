// MobileGL - MobileGL/MG_State/GLState/ShareGroupState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "BufferState/BufferObject.h"
#include "ProgramState/ProgramState.h"
#include "RenderbufferState/RenderbufferObject.h"
#include "SamplerState/SamplerState.h"
#include "TextureState/TextureObject.h"

namespace MobileGL::MG_State::GLState {
    // The GL object registries and name spaces a set of sharing contexts share, i.e. one EGL
    // share group (GL 4.6 core 5.1). Buffers, textures, renderbuffers, samplers, programs and
    // shaders live here; glGen* names are drawn from this object's generators.
    //
    // What is NOT here is what GL scopes to a single context - bindings, texture units, the
    // current program, errors, and the container objects (VAO/FBO/query/pipeline/transform
    // feedback) - so two contexts sharing a group see the same objects and their own bindings.
    class ShareGroupState {
    public:
        ShareGroupState() = default;
        explicit ShareGroupState(Uint64 token) : m_token(token) {}

        // P14 S1's shareGroupToken: the identity the wire uses, 0 for a group no EGL context
        // was made for.
        Uint64 GetToken() const { return m_token; }

        // Buffer
        const SharedPtr<BufferObject>& GetBufferObject(Uint index);
        void GenerateBufferNames(Uint number, Vector<Uint>& buffers);
        const SharedPtr<BufferObject>& CreateBufferObject(Uint index);
        void MarkBufferObjectForDeletion(Uint index);
        Bool ValidateBufferName(Uint index) const;
        Bool ValidateBufferObject(Uint index) const;

        // Texture
        void GenerateTextureNames(Uint number, Vector<Uint>& textures);
        const SharedPtr<ITextureObject>& GetTextureObject(Uint index);
        const SharedPtr<ITextureObject>& CreateTextureObject(Uint index, TextureTarget target);
        const SharedPtr<ITextureObject>& CreateTextureViewObject(Uint index, TextureTarget target,
                                                                 const SharedPtr<ITextureObject>& storageOwner,
                                                                 Uint minLevel, Uint numLevels, Uint minLayer,
                                                                 Uint numLayers);
        void MarkTextureObjectForDeletion(Uint index, Bool keepUnboundReservation);
        Bool ValidateTextureName(Uint index) const;
        Bool ValidateTextureObject(Uint index) const;

        // Renderbuffer
        const SharedPtr<RenderbufferObject>& GetRenderbufferObject(Uint index);
        void GenerateRenderbufferNames(Uint number, Vector<Uint>& renderbuffers);
        const SharedPtr<RenderbufferObject>& CreateRenderbufferObject(Uint index);
        void MarkRenderbufferObjectForDeletion(Uint index);
        Bool ValidateRenderbufferName(Uint index) const;
        Bool ValidateRenderbufferObject(Uint index) const;

        // Sampler objects carry no per-context state, so the whole table lives here.
        SamplerState& Samplers() { return m_samplerState; }

        // Program and shader objects, one name space for both (GL 3.3 core 2.11).
        ProgramState& Programs() { return m_programState; }
        const ProgramState& Programs() const { return m_programState; }

    private:
        Uint64 m_token = 0;
        UnorderedMap<Uint, SharedPtr<BufferObject>> m_bufferObjects;
        IndexGenerator<Uint> m_bufferNames{1024, 1};
        UnorderedMap<GLuint, SharedPtr<ITextureObject>> m_textureObjects;
        IndexGenerator<Uint> m_textureNames{1024, 1};
        UnorderedMap<Uint, SharedPtr<RenderbufferObject>> m_renderbufferObjects;
        IndexGenerator<Uint> m_renderbufferNames{1024, 1};
        SamplerState m_samplerState;
        ProgramState m_programState;
    };
} // namespace MobileGL::MG_State::GLState
