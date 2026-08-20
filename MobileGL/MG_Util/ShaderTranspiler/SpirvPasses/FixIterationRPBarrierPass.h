// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FixIterationRPBarrierPass.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "source/opt/pass.h"
#include "spirv-tools/optimizer.hpp"

namespace MobileGL::MG_Util::ShaderTranspiler {
    // Repairs iterationRP Program 203's missing workgroup rendezvous between two
    // reductions that reuse prefixSumCache. The first phase broadcasts its result
    // through prefixSumCache[0], but the second phase may overwrite that element before
    // every invocation has read it. The pass fingerprints that exact two-scan,
    // 512-invocation shape and inserts one Workgroup control barrier immediately before
    // the second scan. Unrelated modules and already-repaired modules are byte-identical.
    class FixIterationRPBarrierPass : public spvtools::opt::Pass {
    public:
        const char* name() const override { return "fix-iterationrp-barrier"; }
        Status Process() override;

        static spvtools::Optimizer::PassToken CreateFixIterationRPBarrierPass();
    };
} // namespace MobileGL::MG_Util::ShaderTranspiler
