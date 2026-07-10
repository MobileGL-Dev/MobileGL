// MobileGL - MobileGL/MG_Impl/GLImpl/VertexArray/Validators.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>

namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl {
    // The GL-visible GL_MAX_VERTEX_ATTRIBS: min(active backend limit, VertexArrayObject storage
    // capacity). Falls back to the capacity when no backend is active (unit tests).
    Uint GetMaxVertexAttribs();

    Bool ValidateVertexArrayName(Uint index);
    Bool ValidateVertexArrayObject(Uint index);
    Bool ValidateVertexAttributeIndex(Uint index);
    Bool ValidateVertexAttribPointerParams(Uint index, SizeT size, DataType type, Int stride);
} // namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl
