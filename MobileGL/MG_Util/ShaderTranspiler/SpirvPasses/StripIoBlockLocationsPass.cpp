// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripIoBlockLocationsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "StripIoBlockLocationsPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // Every struct type carrying the Block decoration, minus the ones with a builtin
                // member (gl_PerVertex and friends): those are spelled by the language, carry no
                // user Location, and are not what this pass is about. Same shape as
                // UniquifyIoBlockNamesPass::CollectUserBlockStructIds, and deliberately kept
                // beside its own pass rather than shared - the two ask the same question of the
                // module but are armed by different gates, and one growing a special case must
                // not silently move the other.
                std::unordered_set<uint32_t> CollectUserBlockStructIds(IRContext* irContext) {
                    std::unordered_set<uint32_t> blockStructIds;
                    std::unordered_set<uint32_t> builtinStructIds;
                    for (Instruction& annotation : irContext->module()->annotations()) {
                        if (annotation.opcode() == spv::Op::OpDecorate) {
                            if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) ==
                                spv::Decoration::Block) {
                                blockStructIds.insert(annotation.GetSingleWordInOperand(0));
                            }
                        } else if (annotation.opcode() == spv::Op::OpMemberDecorate) {
                            if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2)) ==
                                spv::Decoration::BuiltIn) {
                                builtinStructIds.insert(annotation.GetSingleWordInOperand(0));
                            }
                        }
                    }
                    for (const uint32_t builtinStructId : builtinStructIds) {
                        blockStructIds.erase(builtinStructId);
                    }
                    return blockStructIds;
                }

                // True when `variable` is an Input/Output interface block of the direction the
                // caller armed. Tessellation and geometry interfaces are arrays of the block
                // struct, so array levels are unwrapped before the struct is recognised.
                Bool IsArmedInterfaceBlock(IRContext* irContext, Instruction& variable,
                                           const std::unordered_set<uint32_t>& blockStructIds,
                                           Bool stripInputBlocks, Bool stripOutputBlocks) {
                    if (variable.opcode() != spv::Op::OpVariable) return false;
                    const auto storageClass =
                        static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0));
                    if (storageClass == spv::StorageClass::Input) {
                        if (!stripInputBlocks) return false;
                    } else if (storageClass == spv::StorageClass::Output) {
                        if (!stripOutputBlocks) return false;
                    } else {
                        return false;
                    }

                    auto* defUseMgr = irContext->get_def_use_mgr();
                    Instruction* pointerType = defUseMgr->GetDef(variable.type_id());
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                        return false;
                    }
                    uint32_t pointeeId = pointerType->GetSingleWordInOperand(1);
                    Instruction* pointee = defUseMgr->GetDef(pointeeId);
                    while (pointee != nullptr && (pointee->opcode() == spv::Op::OpTypeArray ||
                                                  pointee->opcode() == spv::Op::OpTypeRuntimeArray)) {
                        pointeeId = pointee->GetSingleWordInOperand(0);
                        pointee = defUseMgr->GetDef(pointeeId);
                    }
                    if (pointee == nullptr || pointee->opcode() != spv::Op::OpTypeStruct) return false;
                    return blockStructIds.find(pointeeId) != blockStructIds.end();
                }
            } // namespace

            spvtools::opt::Pass::Status StripIoBlockLocationsPass::Process() {
                if (m_strippedAny != nullptr) *m_strippedAny = false;
                if (!m_stripInputBlocks && !m_stripOutputBlocks) return Status::SuccessWithoutChange;

                auto* irContext = context();
                const std::unordered_set<uint32_t> blockStructIds = CollectUserBlockStructIds(irContext);
                if (blockStructIds.empty()) return Status::SuccessWithoutChange;

                // The variable ids to strip, resolved BEFORE anything is killed: the walk below
                // deletes annotations, and deciding what to delete while deleting reads a list
                // that is being mutated underneath it.
                std::unordered_set<uint32_t> armedVariableIds;
                for (Instruction& variable : irContext->module()->types_values()) {
                    if (IsArmedInterfaceBlock(irContext, variable, blockStructIds, m_stripInputBlocks,
                                              m_stripOutputBlocks)) {
                        armedVariableIds.insert(variable.result_id());
                    }
                }
                if (armedVariableIds.empty()) return Status::SuccessWithoutChange;

                std::vector<Instruction*> toKill;
                for (Instruction& annotation : irContext->module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate) continue;
                    const auto decoration =
                        static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1));
                    // Component travels with Location and is meaningless without it; a block
                    // whose Location is gone and whose Component survives would be a shader
                    // SPIRV-Cross prints `layout(component = N)` for on its own, which ESSL has
                    // no spelling for at all.
                    if (decoration != spv::Decoration::Location &&
                        decoration != spv::Decoration::Component) {
                        continue;
                    }
                    if (armedVariableIds.find(annotation.GetSingleWordInOperand(0)) ==
                        armedVariableIds.end()) {
                        continue;
                    }
                    toKill.push_back(&annotation);
                }

                for (Instruction* inst : toKill) {
                    irContext->KillInst(inst);
                }
                if (m_strippedAny != nullptr) *m_strippedAny = !toKill.empty();
                return toKill.empty() ? Status::SuccessWithoutChange : Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken StripIoBlockLocationsPass::CreateStripIoBlockLocationsPass(
                Bool stripInputBlocks, Bool stripOutputBlocks, Bool* strippedAny) {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<StripIoBlockLocationsPass>(
                    stripInputBlocks, stripOutputBlocks, strippedAny));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
