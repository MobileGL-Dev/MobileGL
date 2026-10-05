// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeduplicateImageTypesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DeduplicateImageTypesPass.h"
#include "source/opt/decoration_manager.h"
#include "source/opt/ir_context.h"
#include "source/util/make_unique.h"
#include <map>
#include <unordered_set>
#include <vector>

namespace MobileGL::MG_Util::ShaderTranspiler {
    spvtools::opt::Pass::Status DeduplicateImageTypesPass::Process() {
        using spvtools::opt::Instruction;
        std::unordered_set<uint32_t> imageTypes;
        std::map<std::vector<uint32_t>, std::vector<Instruction*>> canonical;
        std::vector<Instruction*> duplicates;
        for (auto& type : context()->types_values()) {
            bool imageRelated = false;
            switch (type.opcode()) {
            case spv::Op::OpTypeImage:
                imageRelated = true;
                break;
            case spv::Op::OpTypeSampledImage:
            case spv::Op::OpTypeArray:
            case spv::Op::OpTypeRuntimeArray:
            case spv::Op::OpTypePointer:
            case spv::Op::OpTypeFunction:
                type.ForEachInId([&](const uint32_t* id) {
                    imageRelated |= imageTypes.count(*id) != 0;
                });
                break;
            default:
                break;
            }
            if (!imageRelated) continue;
            imageTypes.insert(type.result_id());

            // Compare operand IDs, not structural equivalence. Distinct struct IDs
            // must never collapse just because their member layouts happen to match.
            std::vector<uint32_t> key{static_cast<uint32_t>(type.opcode())};
            for (uint32_t i = 0; i < type.NumInOperands(); ++i) {
                const auto& words = type.GetInOperand(i).words;
                key.insert(key.end(), words.begin(), words.end());
            }
            auto& candidates = canonical[key];
            Instruction* match = nullptr;
            for (auto* candidate : candidates) {
                if (context()->get_decoration_mgr()->HaveTheSameDecorations(
                        type.result_id(), candidate->result_id())) {
                    match = candidate;
                    break;
                }
            }
            if (!match) {
                candidates.push_back(&type);
                continue;
            }
            context()->KillNamesAndDecorates(type.result_id());
            context()->ReplaceAllUsesWith(type.result_id(), match->result_id());
            duplicates.push_back(&type);
        }
        for (auto* type : duplicates) context()->KillInst(type);
        return duplicates.empty() ? Status::SuccessWithoutChange : Status::SuccessWithChange;
    }

    spvtools::Optimizer::PassToken DeduplicateImageTypesPass::Create() {
        return spvtools::Optimizer::PassToken(spvtools::MakeUnique<DeduplicateImageTypesPass>());
    }
}
