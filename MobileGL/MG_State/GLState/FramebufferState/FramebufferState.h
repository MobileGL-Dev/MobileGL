// MobileGL - MobileGL/MG_State/GLState/FramebufferState/FramebufferState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "FramebufferObject.h"

namespace MobileGL::MG_State::GLState {
    class FramebufferState {
    public:
        FramebufferState();

        // FBO 0 should be created by MG_Backend when initializing the context
        const SharedPtr<FramebufferObject>& GetFramebufferObject(Uint index);
        void GenerateNames(Uint number, Vector<Uint>& framebuffers);
        const SharedPtr<FramebufferObject>& CreateFramebufferObject(Uint index);
        BindingSlot<FramebufferObject>& GetBindingSlot(FramebufferTarget target);
        void MarkFramebufferObjectForDeletion(Uint index);
        Bool ValidateName(Uint index) const;
        Bool ValidateFramebufferObject(Uint index) const;

#if MOBILEGL_BUILD_DISAGGREGATED
        // P5c (G6, CONTRACT-P5C §3.3/§5.4): the reverse of HandleFor() for a server that holds
        // only the handle - the named-blit consumer on a backend with no FBO twin registry
        // (Magma) resolves the verb's ReadFbo/DrawFbo to the frontend object by the lifetime
        // id the handle was minted over. Named debt, reached inside
        // MGPipeFrontendKeyedRegistryScope; P3b/P4b retire it by carrying the object identity
        // in the record. nullptr when no live framebuffer owns the id.
        SharedPtr<FramebufferObject> FindFramebufferObjectByLifetimeId(Uint64 lifetimeId) const;
#endif

#if MOBILEGL_PIPE_PUSH
        // P2 brief D4: "did the attachment set or the default geometry of ANY framebuffer
        // move". It does NOT cover a BIND - a bind writes a BindingSlot, not the object - so
        // MGPipeTracker pairs this counter with the bound draw framebuffer identity, which
        // is one extra load and keeps the bump points on the object where they belong.
        void NoteAttachmentChanged() { ++m_anyAttachmentGeneration; }
        Uint64 GetAnyAttachmentGeneration() const { return m_anyAttachmentGeneration; }
#endif

    private:
#if MOBILEGL_PIPE_PUSH
        Uint64 m_anyAttachmentGeneration = 0;
#endif
        UnorderedMap<Uint, SharedPtr<FramebufferObject>> m_framebufferObjects;
        IndexGenerator<Uint> m_indexGenerator;
        Array<BindingSlot<FramebufferObject>, static_cast<SizeT>(FramebufferTarget::FramebufferTargetCount)>
            m_bindingSlots;
    };
} // namespace MobileGL::MG_State::GLState
