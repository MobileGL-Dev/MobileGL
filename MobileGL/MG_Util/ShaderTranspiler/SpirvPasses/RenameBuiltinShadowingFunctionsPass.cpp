// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RenameBuiltinShadowingFunctionsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenameBuiltinShadowingFunctionsPass.h"

#include <string>
#include <string_view>

#include "../EsslBuiltinFunctionNames.h"
#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            spvtools::opt::Pass::Status RenameBuiltinShadowingFunctionsPass::Process() {
                Bool modified = false;
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                for (auto& debugInst : irContext->debugs2()) {
                    if (debugInst.opcode() != spv::Op::OpName || debugInst.NumInOperands() < 2) {
                        continue;
                    }
                    const auto* target = defUseMgr->GetDef(debugInst.GetSingleWordInOperand(0));
                    if (target == nullptr || target->opcode() != spv::Op::OpFunction) {
                        continue;
                    }

                    // glslang mangles function OpNames as "name(<paramcodes>"; the base name is
                    // everything before the '(' (entry points like "main" carry no mangling).
                    const std::string mangled = debugInst.GetInOperand(1).AsString();
                    const std::string_view baseName =
                        std::string_view(mangled).substr(0, mangled.find('('));
                    if (!IsEsslBuiltinFunctionName(baseName)) {
                        continue;
                    }

                    debugInst.SetInOperand(1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(
                                                  "mg_" + mangled));
                    modified = true;
                }

                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken
            RenameBuiltinShadowingFunctionsPass::CreateRenameBuiltinShadowingFunctionsPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<RenameBuiltinShadowingFunctionsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
