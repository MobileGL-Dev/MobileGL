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
} // namespace MobileGL::MG_Impl::GLImpl::BufferImpl
