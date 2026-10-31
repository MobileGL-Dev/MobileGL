// MobileGL - MobileGL/MG_State/GLState/RenderbufferState/RenderbufferState.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Miscellany/IndexGenerator.h>
#include "RenderbufferObject.h"

namespace MobileGL::MG_State::GLState {
    // The per-context half of the renderbuffer state: which renderbuffer is bound. The
    // objects and their names belong to the share group (ShareGroupState).
    class RenderbufferState {
    public:
        RenderbufferState();

        BindingSlot<RenderbufferObject>& GetBindingSlot(RenderbufferTarget target);
        void UnbindRenderbufferObject(const SharedPtr<RenderbufferObject>& renderbufferObject);

    private:
        Array<BindingSlot<RenderbufferObject>, static_cast<SizeT>(RenderbufferTarget::RenderbufferTargetCount)>
            m_bindingSlots;
    };
} // namespace MobileGL::MG_State::GLState
