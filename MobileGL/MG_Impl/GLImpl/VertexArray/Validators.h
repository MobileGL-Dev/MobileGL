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
    // Full glVertexAttribPointer / glVertexAttribIPointer format validation, including the packed
    // 2_10_10_10 types and GL_BGRA size. sizeRaw is the untranslated GL size (possibly GL_BGRA);
    // integerPath selects the glVertexAttribIPointer rules.
    Bool ValidateVertexAttribFormat(Uint index, GLint sizeRaw, DataType type, Bool normalized, Int stride,
                                    Bool integerPath);
} // namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl
