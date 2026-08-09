// MobileGL - MobileGL/MG_Impl/GLImpl/Buffer/Validators.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/BufferState/BufferObject.h>

namespace MobileGL::MG_Impl::GLImpl::BufferImpl {
    Bool ValidateBufferTarget(BufferTarget target);
    Bool ValidateBufferName(Uint index, Bool allowZero = false);
    Bool ValidateBufferUsage(BufferUsage usage);
    Bool ValidateBufferMappingAccess(Flags<BufferMappingAccessBit> accessBits);
    Bool ValidateBufferBindingPointTarget(BufferTarget target);
    Bool ValidateBufferBindingPointIndex(BufferTarget target, Uint index);
    // ARB_multi_bind: glBindBuffersBase/Range validate the whole [first, first + count) range
    // up front and report INVALID_OPERATION, where a single out-of-range index would be
    // INVALID_VALUE. Naively looping the single-bind entry points reports the wrong class.
    Bool ValidateBufferBindingPointRange(BufferTarget target, Uint first, GLsizei count, const char* funcName);
} // namespace MobileGL::MG_Impl::GLImpl::BufferImpl
