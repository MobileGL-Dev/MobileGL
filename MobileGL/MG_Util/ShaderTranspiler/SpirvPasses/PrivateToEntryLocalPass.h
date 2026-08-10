// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PrivateToEntryLocalPass.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "spirv-tools/optimizer.hpp"
#include "source/opt/pass.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // AggressiveDCE treats every store to a Private global as an observable side
            // effect while the entry point still contains any OpFunctionCall, so a dead
            // vertex-input -> Private-shim chain (Iris rewrites unused legacy attributes
            // into exactly this shape) survives the whole optimizer chain. Rewriting such
            // a variable to Function storage unlocks ADCE without inlining anything.
            //
            // Upstream's PrivateToLocalPass does that rewrite for a Private variable used
            // in ANY single function - which is unsound here: a Function-storage variable
            // is recreated on every call, so a Private global that carries state across
            // repeated calls of one helper (a memoized init flag, LCG rand state) would
            // silently lose it. This derivative applies the same rewrite restricted to
            // variables whose only using function is an entry point: an entry point runs
            // once per invocation, so the two lifetimes are indistinguishable there.
            //
            // Derived from SPIRV-Tools' PrivateToLocalPass
            // (source/opt/private_to_local_pass.cpp, Copyright (c) 2017 Google Inc.,
            // Apache License 2.0).
            class PrivateToEntryLocalPass final : public spvtools::opt::Pass {
            public:
                const char* name() const override { return "mobilegl-private-to-entry-local"; }
                Status Process() override;

                static spvtools::Optimizer::PassToken CreatePrivateToEntryLocalPass();

            private:
                // The single entry-point function every block-level use of the variable
                // lives in, or nullptr when the uses span functions, include an opcode the
                // rewrite cannot update, or belong to a non-entry function.
                spvtools::opt::Function* FindEntryLocalFunction(const spvtools::opt::Instruction& inst) const;
                bool IsEntryPointFunction(spvtools::opt::Function* function) const;
                bool IsValidUse(const spvtools::opt::Instruction* inst, uint32_t variableId) const;
                bool MoveVariable(spvtools::opt::Instruction* variable, spvtools::opt::Function* function);
                uint32_t GetNewType(uint32_t oldTypeId);
                bool UpdateUse(spvtools::opt::Instruction* inst, spvtools::opt::Instruction* user);
                bool UpdateUses(spvtools::opt::Instruction* inst);
            };
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
