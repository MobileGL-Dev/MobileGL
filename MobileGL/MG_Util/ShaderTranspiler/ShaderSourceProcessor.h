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

            // Some desktop-captured compute shaders build a workgroup-wide linear prefix scan
            // from subgroupInclusiveAdd plus a shared array of subgroup totals. Qualcomm's
            // Vulkan driver miscompiles that exact float InclusiveScan path for native subgroups
            // wider than the capture's 32 lanes. For the narrowly recognized, uniform-control-
            // flow template, replace the subgroup-local scan with a shared-memory, strict
            // left-fold over virtual 32-lane segments. Returns true only when the complete safe
            // template was recognized and rewritten. PreprocessShaderSource reaches this through
            // its device-quirk registry: by default only on detected Qualcomm Vulkan devices,
            // overridable either way with MOBILEGL_QUIRK_SUBGROUP_PREFIX_SCAN=1/0. The explicit
            // entry point exists for deterministic tests.
            Bool RewriteLinearSubgroupPrefixScanForVulkan(ShaderStage stage, Uint32 nativeSubgroupSize, String& source);

            // Rewrites a "#version 330 core" directive that PreprocessShaderSource normalized down
            // from a legacy desktop version back up to "#version 460 core". Returns false (leaving
            // the source untouched) for anything else: ES, compatibility, or an already-modern
            // declaration. Exists so a shader that only parses under the laxer 460 rules - e.g. it
            // uses 420-era syntax without the matching #extension line, which real drivers tend to
            // accept - can be retried instead of failing to compile.
            Bool RetargetLegacyVersionDirectiveTo460(String& source);

            // GLSL reserves a few names glslang happily accepts as identifiers ("packed",
            // "row_major" outside a layout(...) list, the image*Shadow family). Returns the
            // compile-error text for the first violation, or nullopt for a clean source.
            std::optional<String> FindReservedIdentifierViolation(const String& source);
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
