// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/UniquifyIoBlockNamesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "UniquifyIoBlockNamesPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <unordered_map>
#include <unordered_set>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // Which storage classes a block struct is reachable from. A struct seen in both
                // directions inside ONE module cannot be renamed per direction (there is only
                // one name to change), so it is skipped rather than guessed at.
                constexpr Uint32 kSeenAsInput = 1u;
                constexpr Uint32 kSeenAsOutput = 2u;

                // Every struct type carrying the Block decoration, minus the ones with a builtin
                // member (gl_PerVertex): those are named by the language, not by the shader, and
                // renaming one would invent a block no driver knows.
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
                    for (uint32_t builtinStructId : builtinStructIds) {
                        blockStructIds.erase(builtinStructId);
                    }
                    return blockStructIds;
                }

                // The block struct an Input/Output variable declares, or 0 when the variable is
                // not an interface block of the kind this pass renames. Tessellation and geometry
                // interfaces are arrays of the block struct, so one array level is unwrapped -
                // the same shape StripUboMemberRelaxedPrecisionPass unwraps for instance-arrayed
                // uniform blocks.
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
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) return 0;
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

                String FindName(IRContext* irContext, uint32_t id) {
                    for (Instruction& debugInst : irContext->debugs2()) {
                        if (debugInst.opcode() != spv::Op::OpName) continue;
                        if (debugInst.GetSingleWordInOperand(0) != id) continue;
                        return debugInst.GetInOperand(1).AsString();
                    }
                    return String();
                }

                // Replaces an EXISTING OpName only. A block struct with no name of its own is
                // one SPIRV-Cross would spell from a fallback, which the consuming stage would
                // not agree with anyway - leave it alone rather than invent a name for it.
                Bool ReplaceExistingName(IRContext* irContext, uint32_t id, const String& newName) {
                    for (Instruction& debugInst : irContext->debugs2()) {
                        if (debugInst.opcode() != spv::Op::OpName) continue;
                        if (debugInst.GetSingleWordInOperand(0) != id) continue;
                        debugInst.SetInOperand(
                            1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(newName));
                        return true;
                    }
                    return false;
                }

                // Not a real id: "this name reached two different struct types in the same
                // direction", which is already an illegal shader (glslang refuses to reuse a
                // block name inside one interface) and which no rename could repair - two
                // structs would come out with one new name. Both the probe and the rewrite
                // decline it.
                constexpr uint32_t kAmbiguousStructId = 0xffffffffu;

                // The module's interface blocks indexed the way both halves of this pass need
                // them: by name within each direction, plus which directions each struct type
                // is reached from.
                struct IoBlockIndex {
                    std::map<String, uint32_t> inputStructByName;
                    std::map<String, uint32_t> outputStructByName;
                    std::unordered_map<uint32_t, Uint32> storageMaskByStructId;
                };

                IoBlockIndex IndexIoBlocks(IRContext* irContext,
                                           const std::unordered_set<uint32_t>& blockStructIds) {
                    IoBlockIndex index;
                    for (Instruction& variable : irContext->module()->types_values()) {
                        spv::StorageClass storageClass = spv::StorageClass::Input;
                        const uint32_t structId =
                            GetInterfaceBlockStructId(irContext, variable, blockStructIds, storageClass);
                        if (structId == 0) continue;
                        const Bool isInput = storageClass == spv::StorageClass::Input;
                        index.storageMaskByStructId[structId] |= isInput ? kSeenAsInput : kSeenAsOutput;

                        const String blockName = FindName(irContext, structId);
                        if (blockName.empty()) continue;
                        std::map<String, uint32_t>& byName =
                            isInput ? index.inputStructByName : index.outputStructByName;
                        const auto inserted = byName.emplace(blockName, structId);
                        if (!inserted.second && inserted.first->second != structId) {
                            inserted.first->second = kAmbiguousStructId;
                        }
                    }
                    return index;
                }
            } // namespace

            void UniquifyIoBlockNamesPass::ProbeIoBlockNames(spvtools::opt::IRContext* irContext,
                                                             std::set<String>& outCollidingBlockNames,
                                                             std::set<String>& outDeclaredNames) {
                if (irContext == nullptr) return;

                for (Instruction& debugInst : irContext->debugs2()) {
                    if (debugInst.opcode() != spv::Op::OpName) continue;
                    outDeclaredNames.insert(debugInst.GetInOperand(1).AsString());
                }

                const std::unordered_set<uint32_t> blockStructIds = CollectUserBlockStructIds(irContext);
                if (blockStructIds.empty()) return;

                const IoBlockIndex index = IndexIoBlocks(irContext, blockStructIds);
                for (const auto& input : index.inputStructByName) {
                    const auto output = index.outputStructByName.find(input.first);
                    if (output == index.outputStructByName.end()) continue;
                    if (input.second == kAmbiguousStructId || output->second == kAmbiguousStructId) continue;
                    // Same struct type on both sides: there is one name to rename and two
                    // directions wanting different ones, so the collision cannot be repaired.
                    if (input.second == output->second) continue;
                    outCollidingBlockNames.insert(input.first);
                }
            }

            spvtools::opt::Pass::Status UniquifyIoBlockNamesPass::Process() {
                if (m_inputBlockRenames.empty() && m_outputBlockRenames.empty()) {
                    return Status::SuccessWithoutChange;
                }

                auto* irContext = context();
                const std::unordered_set<uint32_t> blockStructIds = CollectUserBlockStructIds(irContext);
                if (blockStructIds.empty()) return Status::SuccessWithoutChange;

                // Indexed BEFORE anything is renamed, so every decline below is decided against
                // the names the module arrived with rather than against a half-renamed one.
                const IoBlockIndex index = IndexIoBlocks(irContext, blockStructIds);

                Bool modified = false;
                for (int direction = 0; direction < 2; ++direction) {
                    const Bool isInput = direction == 0;
                    const std::map<String, uint32_t>& byName =
                        isInput ? index.inputStructByName : index.outputStructByName;
                    const std::map<String, String>& renames =
                        isInput ? m_inputBlockRenames : m_outputBlockRenames;
                    const Uint32 wantedMask = isInput ? kSeenAsInput : kSeenAsOutput;

                    for (const auto& block : byName) {
                        if (block.second == kAmbiguousStructId) continue;
                        const auto rename = renames.find(block.first);
                        if (rename == renames.end()) continue;
                        if (rename->second.empty() || rename->second == block.first) continue;
                        // A struct type reached from BOTH directions carries one name for two
                        // interfaces, so renaming it for this direction would rename it for the
                        // other one too. Leave the module as it was.
                        const auto mask = index.storageMaskByStructId.find(block.second);
                        if (mask == index.storageMaskByStructId.end() || mask->second != wantedMask) continue;
                        if (!ReplaceExistingName(irContext, block.second, rename->second)) continue;

                        if (m_renamedBlockNames != nullptr) m_renamedBlockNames->insert(block.first);
                        modified = true;
                    }
                }

                return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken UniquifyIoBlockNamesPass::CreateUniquifyIoBlockNamesPass(
                const std::map<String, String>& inputBlockRenames,
                const std::map<String, String>& outputBlockRenames, std::set<String>* renamedBlockNames) {
                return spvtools::Optimizer::PassToken(MakeUnique<UniquifyIoBlockNamesPass>(
                    inputBlockRenames, outputBlockRenames, renamedBlockNames));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
