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

                // The interface-block struct an Input/Output variable declares, or 0 when the
                // variable is not one. Tessellation and geometry interfaces are arrays of the
                // block struct, so array levels are unwrapped before the struct is recognised.
                uint32_t GetInterfaceBlockStructId(IRContext* irContext, Instruction& variable,
                                                   const std::unordered_set<uint32_t>& blockStructIds,
                                                   spv::StorageClass& outStorageClass) {
                    if (variable.opcode() != spv::Op::OpVariable) return 0;
                    const auto storageClass =
                        static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0));
                    if (storageClass != spv::StorageClass::Input &&
                        storageClass != spv::StorageClass::Output) {
                        return 0;
                    }

                    auto* defUseMgr = irContext->get_def_use_mgr();
                    Instruction* pointerType = defUseMgr->GetDef(variable.type_id());
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                        return 0;
                    }
                    uint32_t pointeeId = pointerType->GetSingleWordInOperand(1);
                    Instruction* pointee = defUseMgr->GetDef(pointeeId);
                    while (pointee != nullptr && (pointee->opcode() == spv::Op::OpTypeArray ||
                                                  pointee->opcode() == spv::Op::OpTypeRuntimeArray)) {
                        pointeeId = pointee->GetSingleWordInOperand(0);
                        pointee = defUseMgr->GetDef(pointeeId);
                    }
                    if (pointee == nullptr || pointee->opcode() != spv::Op::OpTypeStruct) return 0;
                    if (blockStructIds.find(pointeeId) == blockStructIds.end()) return 0;

                    outStorageClass = storageClass;
                    return pointeeId;
                }

                Bool DirectionIsArmed(spv::StorageClass storageClass, Bool stripInputBlocks,
                                      Bool stripOutputBlocks) {
                    return storageClass == spv::StorageClass::Input ? stripInputBlocks : stripOutputBlocks;
                }
            } // namespace

            spvtools::opt::Pass::Status StripIoBlockLocationsPass::Process() {
                if (m_strippedAny != nullptr) *m_strippedAny = false;
                if (!m_stripInputBlocks && !m_stripOutputBlocks) return Status::SuccessWithoutChange;

                auto* irContext = context();
                const std::unordered_set<uint32_t> blockStructIds = CollectUserBlockStructIds(irContext);
                if (blockStructIds.empty()) return Status::SuccessWithoutChange;

                // What to strip, resolved BEFORE anything is killed: the walk below deletes
                // annotations, and deciding what to delete while deleting reads a list that is
                // being mutated underneath it.
                //
                // BOTH LEVELS, because a block carries its location at exactly one of them and
                // which one is not the caller's choice. When the location came from the
                // cross-stage IO resolver (or from `layout(location=) out Blk {...}`) glslang
                // puts it on the VARIABLE; when the application located the members instead
                // (`out Blk { layout(location = 4) vec4 v; }`) it puts one OpMemberDecorate per
                // member and NOTHING on the variable - and SPIRV-Cross then suppresses the
                // block-level qualifier and prints the member ones instead
                // (spirv_glsl.cpp:1444 and :2037-2045). Stripping only the variable level would
                // leave that second shape emitting exactly the located block this driver drops
                // the payload for, and - because there was no variable decoration to remove -
                // would report nothing stripped, so the caller would decline the module and
                // nothing would say the repair had passed the shader by.
                std::unordered_set<uint32_t> armedVariableIds;
                std::unordered_set<uint32_t> armedStructIds;
                // Block structs reached by an interface variable whose direction is NOT armed.
                // A struct in here is left alone even if some armed variable also reaches it:
                // member decorations belong to the TYPE, so stripping them would take the
                // qualifier off the unarmed side too - the one whose other end is in a
                // different program and is matched by exactly that number.
                std::unordered_set<uint32_t> unarmedStructIds;
                for (Instruction& variable : irContext->module()->types_values()) {
                    spv::StorageClass storageClass = spv::StorageClass::Input;
                    const uint32_t structId =
                        GetInterfaceBlockStructId(irContext, variable, blockStructIds, storageClass);
                    if (structId == 0) continue;
                    if (DirectionIsArmed(storageClass, m_stripInputBlocks, m_stripOutputBlocks)) {
                        armedVariableIds.insert(variable.result_id());
                        armedStructIds.insert(structId);
                    } else {
                        unarmedStructIds.insert(structId);
                    }
                }
                for (const uint32_t unarmedStructId : unarmedStructIds) {
                    armedStructIds.erase(unarmedStructId);
                }
                if (armedVariableIds.empty()) return Status::SuccessWithoutChange;

                // Component travels with Location and is meaningless without it. Leaving one
                // behind is not merely untidy: for an ES target SPIRV-Cross THROWS on a block
                // member's Component (spirv_glsl.cpp:1447-1460) rather than printing it, which
                // costs the whole stage.
                const auto isLocationOrComponent = [](uint32_t decoration) {
                    return static_cast<spv::Decoration>(decoration) == spv::Decoration::Location ||
                           static_cast<spv::Decoration>(decoration) == spv::Decoration::Component;
                };

                std::vector<Instruction*> toKill;
                for (Instruction& annotation : irContext->module()->annotations()) {
                    if (annotation.opcode() == spv::Op::OpDecorate) {
                        if (!isLocationOrComponent(annotation.GetSingleWordInOperand(1))) continue;
                        if (armedVariableIds.find(annotation.GetSingleWordInOperand(0)) ==
                            armedVariableIds.end()) {
                            continue;
                        }
                        toKill.push_back(&annotation);
                    } else if (annotation.opcode() == spv::Op::OpMemberDecorate) {
                        // OpMemberDecorate <struct> <member> <decoration> ...
                        if (!isLocationOrComponent(annotation.GetSingleWordInOperand(2))) continue;
                        if (armedStructIds.find(annotation.GetSingleWordInOperand(0)) ==
                            armedStructIds.end()) {
                            continue;
                        }
                        toKill.push_back(&annotation);
                    }
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
