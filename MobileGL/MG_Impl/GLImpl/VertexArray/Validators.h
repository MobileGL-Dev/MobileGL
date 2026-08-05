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

    // GL_MAX_VERTEX_ATTRIB_BINDINGS. The default attribute -> binding mapping is the identity, so a
    // binding point that cannot also be an attribute index would resolve into an attribute the
    // backend has to reject on every draw; real drivers report the two limits equal as well.
    Uint GetMaxVertexAttribBindings();

    // GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET. The relative offset is folded into the resolved
    // attribute offset in the frontend and never reaches a backend limit, so this is the value the
    // spec requires an implementation to support at minimum (GL 4.6 core table 23.63).
    Uint GetMaxVertexAttribRelativeOffset();

    // GL_MAX_VERTEX_ATTRIB_STRIDE. Like the relative offset above, the stride never reaches a
    // backend limit of its own, so this is the spec minimum (GL 4.6 core table 23.63).
    Uint GetMaxVertexAttribStride();

    Bool ValidateVertexArrayName(Uint index);
    Bool ValidateVertexArrayObject(Uint index);
    Bool ValidateVertexAttributeIndex(Uint index);
    Bool ValidateVertexAttribPointerParams(Uint index, SizeT size, DataType type, Int stride);
    // Full glVertexAttribPointer / glVertexAttribIPointer format validation, including the packed
    // 2_10_10_10 types and GL_BGRA size. sizeRaw is the untranslated GL size (possibly GL_BGRA);
    // integerPath selects the glVertexAttribIPointer rules.
    Bool ValidateVertexAttribFormat(Uint index, GLint sizeRaw, GLenum glType, DataType type, Bool normalized,
                                    Int stride, Bool integerPath);
    // glVertexAttribLFormat / glVertexArrayAttribLFormat: the only accepted type is GL_DOUBLE and
    // the size range is 1-4 (GL_BGRA is a float-path size). Separate from the function above
    // because the long path shares none of its type or size rules.
    Bool ValidateVertexAttribLFormat(Uint index, GLint size, GLenum type);
    // Shared by every *Format entry point: INVALID_VALUE once relativeoffset leaves the range the
    // implementation advertises.
    Bool ValidateVertexAttribRelativeOffset(Uint relativeOffset);
} // namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl
