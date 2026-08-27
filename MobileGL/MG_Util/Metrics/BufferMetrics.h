// MobileGL - MobileGL/MG_Util/Metrics/BufferMetrics.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        SizeT GetGLTypeSize(GLenum type);

        // The largest value an index of this type can hold, which is also the value
        // GL_PRIMITIVE_RESTART_FIXED_INDEX (and its GLES/Vulkan equivalents) restart on. Zero for a
        // type that cannot index at all.
        //
        // Shared rather than re-derived per backend on purpose: three places have to agree about
        // what an index of this type can be - whether a rewrite is needed at all, what the rewrite
        // compares against, and whether the driver should be told to restart. GL 4.6 core 10.3.6
        // compares the FETCHED index, zero-extended, against the full 32-bit
        // PRIMITIVE_RESTART_INDEX, so a restart index greater than this value matches no index and
        // the draw restarts nowhere. Truncating it to the type's width instead - which one of these
        // three places used to do - turns a legal vertex index into a restart.
        Uint32 FixedRestartIndexForGLType(GLenum indexType);

    } // namespace MG_Util
} // namespace MobileGL
