// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderStage.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

namespace MobileGL {
    // Split out of ShaderObject.h so the compile pipeline's headers form a DAG:
    // ShaderStage.h <- ShaderPreprocessCache.h <- ShaderCompileTask.h <- ShaderObject.h.
    // Every existing includer of ShaderObject.h still sees this type unchanged.
    enum class ShaderStage {
        Vertex,
        TessControl,
        TessEval,
        Geometry,
        Fragment,
        Compute,
        ShaderStageCount,
        Unknown = -1
    };
} // namespace MobileGL
