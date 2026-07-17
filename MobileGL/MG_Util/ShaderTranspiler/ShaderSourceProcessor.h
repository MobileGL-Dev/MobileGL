// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderObject.h>

namespace MobileGL {
    enum class ShaderProfile {
        Core,
        Compatibility,
        ES,
    };

    namespace MG_Util {
        namespace ShaderTranspiler {
            void PreprocessShaderSource(ShaderStage stage, String& source);

            // Rewrites a "#version 330 core" directive that PreprocessShaderSource normalized down
            // from a legacy desktop version back up to "#version 460 core". Returns false (leaving
            // the source untouched) for anything else: ES, compatibility, or an already-modern
            // declaration. Exists so a shader that only parses under the laxer 460 rules - e.g. it
            // uses 420-era syntax without the matching #extension line, which real drivers tend to
            // accept - can be retried instead of failing to compile.
            Bool RetargetLegacyVersionDirectiveTo460(String& source);
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL