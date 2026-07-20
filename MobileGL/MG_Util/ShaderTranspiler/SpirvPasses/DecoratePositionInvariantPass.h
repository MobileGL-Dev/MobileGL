// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DecoratePositionInvariantPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

#include <Includes.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // Adds the Invariant decoration to every Position builtin output. GL apps
            // routinely rely on cross-program position invariance for multi-pass equality
            // depth tests - MC 26.3's OIT re-draws the cloud geometry with GEQUAL against the
            // depth its own first pass wrote - and a driver that optimizes each pipeline
            // separately may otherwise vary the position math between passes, dropping whole
            // primitives from the later ones. Both the plain (OpDecorate on a Position
            // variable) and the block-member (OpMemberDecorate on gl_PerVertex) spellings are
            // handled; targets that already carry Invariant are left alone. DirectVulkan only.
            class DecoratePositionInvariantPass : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "decorate-position-invariant"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreateDecoratePositionInvariantPass();
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
