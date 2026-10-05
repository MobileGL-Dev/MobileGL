// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeduplicateImageTypesPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

namespace MobileGL::MG_Util::ShaderTranspiler {
    // Preserve named structs: GL reflection and cross-stage interfaces use their identity.
    class DeduplicateImageTypesPass final : public spvtools::opt::Pass {
    public:
        const char* name() const override { return "mobilegl-deduplicate-image-types"; }
        Status Process() override;
        static spvtools::Optimizer::PassToken Create();
    };
}
