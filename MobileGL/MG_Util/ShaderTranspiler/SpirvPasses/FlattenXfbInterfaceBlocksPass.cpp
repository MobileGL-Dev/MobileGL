// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenXfbInterfaceBlocksPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FlattenXfbInterfaceBlocksPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <memory>
#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::IRContext;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                // Decorations that describe how ONE member is interpolated or stored, and
                // therefore have to travel from the block member to the variable that replaces
                // it. Location is handled separately (it is computed per member); everything
                // else about a block - Block itself, member Offsets, builtin decorations - is
                // about the aggregate and stays behind on the shadow.
                Bool IsMemberDecorationToCarry(spv::Decoration decoration) {
                    switch (decoration) {
                    case spv::Decoration::RelaxedPrecision:
                    case spv::Decoration::Flat:
                    case spv::Decoration::NoPerspective:
                    case spv::Decoration::Centroid:
                    case spv::Decoration::Sample:
                    case spv::Decoration::Invariant:
                    case spv::Decoration::Patch:
                        return true;
                    default:
                        return false;
                    }
                }

                // Decorations that say something about a member's placement WITHIN the block's
                // location, which a free-standing variable cannot express the same way. Rather
                // than move a member to a place the consumer will not look for it, decline.
                Bool IsMemberDecorationThatBlocksFlattening(spv::Decoration decoration) {
                    switch (decoration) {
                    case spv::Decoration::Component:
                    case spv::Decoration::XfbBuffer:
                    case spv::Decoration::XfbStride:
                    case spv::Decoration::Stream:
                        return true;
                    default:
                        return false;
                    }
                }

                // Every name the module already spells, so a synthesised "<Block>_<member>" that
                // would collide with one declines instead of emitting two declarations of the
                // same identifier (which the driver rejects, taking the whole program with it).
                std::unordered_set<String> CollectNames(IRContext& irContext) {
                    std::unordered_set<String> names;
                    for (auto& debugInst : irContext.debugs2()) {
                        if (debugInst.opcode() != spv::Op::OpName) continue;
                        names.insert(debugInst.GetInOperand(1).AsString());
                    }
                    return names;
                }

                // Locations one value of `type` occupies (GL 4.6 core 11.1.2.1 / 15.2): a
                // matrix takes one per column, a double-precision vector wider than two takes
                // two, an array takes its element's span once per element. 0 means "this pass
                // cannot place it", which declines the whole block rather than guessing.
                Uint32 LocationSpan(const analysis::Type* type) {
                    if (type == nullptr) return 0;
                    if (type->AsFloat() != nullptr || type->AsInteger() != nullptr ||
                        type->AsBool() != nullptr) {
                        return 1u;
                    }
                    if (const auto* vector = type->AsVector()) {
                        const auto* element = vector->element_type();
                        if (element->AsFloat() == nullptr && element->AsInteger() == nullptr &&
                            element->AsBool() == nullptr) {
                            return 0;
                        }
                        const auto* elementFloat = element->AsFloat();
                        const Bool is64Bit = elementFloat != nullptr && elementFloat->width() == 64;
                        return (is64Bit && vector->element_count() > 2) ? 2u : 1u;
                    }
                    if (const auto* matrix = type->AsMatrix()) {
                        const Uint32 columnSpan = LocationSpan(matrix->element_type());
                        return columnSpan == 0 ? 0 : columnSpan * matrix->element_count();
                    }
                    if (const auto* array = type->AsArray()) {
                        const Uint32 elementSpan = LocationSpan(array->element_type());
                        if (elementSpan == 0) return 0;
                        // A runtime array has no span; a vertex-stage interface never has one.
                        if (!array->length_info().words.empty() &&
                            array->length_info().words[0] !=
                                static_cast<Uint32>(analysis::Array::LengthInfo::kConstant)) {
                            return 0;
                        }
                        // words[0] is the tag, words[1..] the constant value; only a
                        // single-word length can be an array size here.
                        if (array->length_info().words.size() != 2) return 0;
                        const Uint32 count = array->length_info().words[1];
                        return count == 0 ? 0 : elementSpan * count;
                    }
                    // Structs (a nested block member) carry their own layout rules and are not
                    // worth guessing at: declining leaves the module exactly as it was.
                    return 0;
                }

                // The Location a variable carries, or false when it carries none.
                Bool FindLocationDecoration(IRContext& irContext, Uint32 variableId, Uint32& outLocation) {
                    for (auto& annotation : irContext.annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != variableId) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::Location) {
                            continue;
                        }
                        outLocation = annotation.GetSingleWordInOperand(2);
                        return true;
                    }
                    return false;
                }

                // The Location decorating member `member` of struct type `typeId`, if any.
                Bool FindMemberLocationDecoration(IRContext& irContext, Uint32 typeId, Uint32 member,
                                                  Uint32& outLocation) {
                    for (auto& annotation : irContext.annotations()) {
                        if (annotation.opcode() != spv::Op::OpMemberDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != typeId) continue;
                        if (annotation.GetSingleWordInOperand(1) != member) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2)) !=
                            spv::Decoration::Location) {
                            continue;
                        }
                        outLocation = annotation.GetSingleWordInOperand(3);
                        return true;
                    }
                    return false;
                }

                String FindName(IRContext& irContext, Uint32 id) {
                    for (auto& debugInst : irContext.debugs2()) {
                        if (debugInst.opcode() != spv::Op::OpName) continue;
                        if (debugInst.GetSingleWordInOperand(0) != id) continue;
                        return debugInst.GetInOperand(1).AsString();
                    }
                    return String();
                }

                String FindMemberName(IRContext& irContext, Uint32 typeId, Uint32 member) {
                    for (auto& debugInst : irContext.debugs2()) {
                        if (debugInst.opcode() != spv::Op::OpMemberName) continue;
                        if (debugInst.GetSingleWordInOperand(0) != typeId) continue;
                        if (debugInst.GetSingleWordInOperand(1) != member) continue;
                        return debugInst.GetInOperand(2).AsString();
                    }
                    return String();
                }
            } // namespace

            Bool FlattenXfbInterfaceBlocksPass::RewriteCaptureName(const String& captureName,
                                                                   const std::set<String>& flattenedBlockNames,
                                                                   String& outName) {
                const SizeT dot = captureName.find('.');
                if (dot == String::npos || dot == 0) return false;
                const String blockName = captureName.substr(0, dot);
                if (flattenedBlockNames.find(blockName) == flattenedBlockNames.end()) return false;
                outName = blockName + "_" + captureName.substr(dot + 1);
                return true;
            }

            spvtools::opt::Pass::Status FlattenXfbInterfaceBlocksPass::Process() {
                if (m_blockNames.empty()) return Status::SuccessWithoutChange;

                auto* irContext = context();
                auto entryPoints = irContext->module()->entry_points();
                if (entryPoints.begin() == entryPoints.end()) return Status::SuccessWithoutChange;
                Instruction* entryPoint = &*entryPoints.begin();

                // A stage whose outputs are published somewhere other than the end of the entry
                // point cannot take the shadow-and-copy-out shape: a geometry shader's outputs
                // are captured by every OpEmitVertex, and a tessellation control shader's are
                // per-invocation slots of an arrayed interface. Copying at OpReturn would
                // publish once, at the end, which is silently the wrong data rather than a
                // failure - so those stages keep their blocks and the capture keeps its
                // spelling.
                const auto executionModel =
                    static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0));
                if (executionModel == spv::ExecutionModel::Geometry ||
                    executionModel == spv::ExecutionModel::TessellationControl) {
                    MGLOG_D("FlattenXfbInterfaceBlocksPass: execution model %u publishes outputs outside "
                            "the entry point's return; leaving its blocks declared as blocks",
                            static_cast<Uint32>(executionModel));
                    return Status::SuccessWithoutChange;
                }

                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                struct Member {
                    Uint32 typeId = 0;
                    Uint32 interfacePointerTypeId = 0;
                    Uint32 privatePointerTypeId = 0;
                    Uint32 variableId = 0;
                    Uint32 location = 0;
                    Bool hasLocation = false;
                    String name;
                };
                struct Target {
                    Instruction* variable = nullptr;
                    Uint32 structTypeId = 0;
                    Uint32 privatePointerTypeId = 0;
                    spv::StorageClass storageClass = spv::StorageClass::Output;
                    String blockName;
                    std::vector<Member> members;
                };
                std::vector<Target> targets;
                const std::unordered_set<String> existingNames = CollectNames(*irContext);

                for (Instruction& inst : irContext->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    const auto storageClass =
                        static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                    if (storageClass != spv::StorageClass::Input &&
                        storageClass != spv::StorageClass::Output) {
                        continue;
                    }
                    Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                    if (pointerType == nullptr) continue;
                    const Uint32 pointeeTypeId = pointerType->GetSingleWordInOperand(1);
                    const analysis::Type* pointeeType = typeMgr->GetType(pointeeTypeId);
                    const auto* structType = pointeeType != nullptr ? pointeeType->AsStruct() : nullptr;
                    if (structType == nullptr) continue;

                    const String blockName = FindName(*irContext, pointeeTypeId);
                    if (blockName.empty() ||
                        m_blockNames.find(blockName) == m_blockNames.end()) {
                        continue;
                    }

                    Target target;
                    target.variable = &inst;
                    target.structTypeId = pointeeTypeId;
                    target.storageClass = storageClass;
                    target.blockName = blockName;

                    Uint32 blockLocation = 0;
                    const Bool hasBlockLocation =
                        FindLocationDecoration(*irContext, inst.result_id(), blockLocation);

                    Bool usable = true;
                    for (auto& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpMemberDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != pointeeTypeId) continue;
                        if (IsMemberDecorationThatBlocksFlattening(
                                static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2)))) {
                            usable = false;
                            break;
                        }
                    }
                    Uint32 runningLocation = blockLocation;
                    const auto& memberTypes = structType->element_types();
                    for (Uint32 memberIndex = 0; usable && memberIndex < memberTypes.size(); ++memberIndex) {
                        Member member;
                        member.typeId = typeMgr->GetId(memberTypes[memberIndex]);
                        member.name = FindMemberName(*irContext, pointeeTypeId, memberIndex);
                        if (member.typeId == 0 || member.name.empty() ||
                            existingNames.count(blockName + "_" + member.name) != 0) {
                            usable = false;
                            break;
                        }
                        const Uint32 span = LocationSpan(memberTypes[memberIndex]);
                        if (span == 0) {
                            usable = false;
                            break;
                        }
                        Uint32 memberLocation = 0;
                        if (FindMemberLocationDecoration(*irContext, pointeeTypeId, memberIndex,
                                                         memberLocation)) {
                            member.location = memberLocation;
                            member.hasLocation = true;
                        } else if (hasBlockLocation) {
                            member.location = runningLocation;
                            member.hasLocation = true;
                        }
                        runningLocation += span;
                        target.members.push_back(member);
                    }
                    if (!usable || target.members.empty()) {
                        MGLOG_D("FlattenXfbInterfaceBlocksPass: block '%s' has a member this pass cannot "
                                "place; leaving it declared as a block",
                                blockName.c_str());
                        continue;
                    }
                    targets.push_back(std::move(target));
                }

                if (targets.empty()) return Status::SuccessWithoutChange;

                // The entry point's first block, past the leading OpVariable run SPIR-V
                // requires to stay at the top of a function. Resolved BEFORE anything is
                // mutated: a decline after the rewrite has started would leave a half-converted
                // module behind, and the optimizer serialises whatever the module holds
                // regardless of the status this returns.
                const Uint32 entryFunctionId = entryPoint->GetSingleWordInOperand(1);
                spvtools::opt::Function* entryFunction = nullptr;
                for (auto& function : *irContext->module()) {
                    if (function.result_id() == entryFunctionId) {
                        entryFunction = &function;
                        break;
                    }
                }
                if (entryFunction == nullptr || entryFunction->begin() == entryFunction->end()) {
                    return Status::SuccessWithoutChange;
                }
                {
                    auto& probeBlock = *entryFunction->begin();
                    auto probe = probeBlock.begin();
                    while (probe != probeBlock.end() && probe->opcode() == spv::Op::OpVariable) ++probe;
                    if (probe == probeBlock.end()) return Status::SuccessWithoutChange;
                }

                // Demoting the block changes the STORAGE CLASS of every pointer derived from it,
                // and a derived pointer's own result type still says Input/Output - which is an
                // invalid module ("the result pointer storage class and base pointer storage
                // class in OpAccessChain do not match") that spirv-val rejects and a driver may
                // silently miscompile. Collected before anything is mutated so an unsupported
                // use can still decline the whole rewrite rather than leave the module broken.
                std::unordered_set<Uint32> derivedPointers;
                for (const auto& target : targets) {
                    derivedPointers.insert(target.variable->result_id());
                }
                std::vector<Instruction*> pointersToRetype;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            const spv::Op opcode = inst.opcode();
                            const Bool indexes = opcode == spv::Op::OpAccessChain ||
                                                 opcode == spv::Op::OpInBoundsAccessChain ||
                                                 opcode == spv::Op::OpCopyObject;
                            if (indexes) {
                                if (inst.NumInOperands() > 0 &&
                                    derivedPointers.count(inst.GetSingleWordInOperand(0)) != 0) {
                                    derivedPointers.insert(inst.result_id());
                                    pointersToRetype.push_back(&inst);
                                }
                                continue;
                            }
                            // Reads and writes are exactly what a shadowed block is for; the
                            // pointer they use does not change shape.
                            if (opcode == spv::Op::OpLoad || opcode == spv::Op::OpStore) continue;
                            for (Uint32 i = 0; i < inst.NumInOperands(); ++i) {
                                const Operand& operand = inst.GetInOperand(i);
                                if (operand.type != SPV_OPERAND_TYPE_ID || operand.words.size() != 1) {
                                    continue;
                                }
                                if (derivedPointers.count(operand.words[0]) == 0) continue;
                                MGLOG_D("FlattenXfbInterfaceBlocksPass: interface block %%%u reaches a "
                                        "SPIR-V opcode %u that this pass cannot follow; leaving it "
                                        "declared as a block",
                                        operand.words[0], static_cast<Uint32>(opcode));
                                return Status::SuccessWithoutChange;
                            }
                        }
                    }
                }

                // Every type this pass names has to exist before the variables that name it.
                for (auto& target : targets) {
                    target.privatePointerTypeId =
                        typeMgr->FindPointerToType(target.structTypeId, spv::StorageClass::Private);
                    for (auto& member : target.members) {
                        member.interfacePointerTypeId =
                            typeMgr->FindPointerToType(member.typeId, target.storageClass);
                        member.privatePointerTypeId =
                            typeMgr->FindPointerToType(member.typeId, spv::StorageClass::Private);
                    }
                }

                std::vector<Operand> interfaceOperands;
                for (Uint32 i = 0; i < entryPoint->NumInOperands(); ++i) {
                    interfaceOperands.push_back(entryPoint->GetInOperand(i));
                }

                std::unordered_set<Uint32> strippedBlockStructTypes;
                for (auto& target : targets) {
                    Instruction* variable = target.variable;
                    const Uint32 oldVariableId = variable->result_id();

                    std::vector<std::unique_ptr<Instruction>> newDecorations;
                    std::vector<std::unique_ptr<Instruction>> newNames;
                    for (auto& member : target.members) {
                        member.variableId = irContext->TakeNextId();
                        irContext->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpVariable, member.interfacePointerTypeId, member.variableId,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_STORAGE_CLASS,
                                 {static_cast<Uint32>(target.storageClass)}}}));

                        // The name IS the contract: it is what the rewritten capture request
                        // asks the driver for, and what makes the producer and consumer of a
                        // flattened block still match each other by name.
                        const String flatName = target.blockName + "_" + member.name;
                        std::vector<Operand> nameOperands;
                        nameOperands.push_back({SPV_OPERAND_TYPE_ID, {member.variableId}});
                        nameOperands.push_back(
                            {SPV_OPERAND_TYPE_LITERAL_STRING,
                             spvtools::utils::MakeVector(flatName)});
                        newNames.push_back(spvtools::MakeUnique<Instruction>(irContext, spv::Op::OpName, 0, 0,
                                                                             nameOperands));

                        if (member.hasLocation) {
                            newDecorations.push_back(spvtools::MakeUnique<Instruction>(
                                irContext, spv::Op::OpDecorate, 0, 0,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {member.variableId}},
                                    {SPV_OPERAND_TYPE_DECORATION,
                                     {static_cast<Uint32>(spv::Decoration::Location)}},
                                    {SPV_OPERAND_TYPE_LITERAL_INTEGER, {member.location}}}));
                        }
                    }

                    // Member decorations that describe the member (not the aggregate) move to
                    // the variable that now carries it.
                    for (auto& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpMemberDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != target.structTypeId) continue;
                        const Uint32 memberIndex = annotation.GetSingleWordInOperand(1);
                        if (memberIndex >= target.members.size()) continue;
                        const auto decoration =
                            static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2));
                        if (!IsMemberDecorationToCarry(decoration)) continue;
                        std::vector<Operand> operands;
                        operands.push_back({SPV_OPERAND_TYPE_ID, {target.members[memberIndex].variableId}});
                        for (Uint32 i = 2; i < annotation.NumInOperands(); ++i) {
                            operands.push_back(annotation.GetInOperand(i));
                        }
                        newDecorations.push_back(
                            spvtools::MakeUnique<Instruction>(irContext, spv::Op::OpDecorate, 0, 0, operands));
                    }

                    // The block's own Location described where its members start and means
                    // nothing on a Private shadow; leaving it would also make SPIRV-Cross print
                    // a location for a variable that no longer has an interface.
                    std::vector<Instruction*> deadDecorations;
                    // A struct type shared by two flattened variables would otherwise have its
                    // Block decoration killed twice - and the second kill is a use-after-free,
                    // not a no-op.
                    const Bool structTypeAlreadyStripped =
                        strippedBlockStructTypes.count(target.structTypeId) != 0;
                    strippedBlockStructTypes.insert(target.structTypeId);
                    for (auto& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        const Uint32 target0 = annotation.GetSingleWordInOperand(0);
                        if (target0 == oldVariableId) {
                            deadDecorations.push_back(&annotation);
                            continue;
                        }
                        if (structTypeAlreadyStripped) continue;
                        // The struct type stops being an interface block the moment its only
                        // interface variable becomes a Private shadow, and SPIRV-Cross prints a
                        // Block-decorated struct as a BLOCK declaration - `out StageData {...}
                        // vs_out;` for a variable that is no longer an output, which is not
                        // ESSL and which the driver rejects with a bare syntax error on the
                        // instance name. A block name is unique per stage, so nothing else can
                        // still need this decoration.
                        if (target0 == target.structTypeId &&
                            static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) ==
                                spv::Decoration::Block) {
                            deadDecorations.push_back(&annotation);
                        }
                    }
                    for (auto* annotation : deadDecorations) {
                        irContext->KillInst(annotation);
                    }
                    for (auto& decoration : newDecorations) {
                        irContext->AddAnnotationInst(std::move(decoration));
                    }
                    for (auto& debugName : newNames) {
                        irContext->AddDebug2Inst(std::move(debugName));
                    }

                    // Demote the block to a Private shadow. Every OpAccessChain and OpStore the
                    // body already performs on it stays valid and keeps its types; only the
                    // storage class changed, and Private is the one storage class a pointer of
                    // any shape may live in.
                    variable->SetResultType(target.privatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<Uint32>(spv::StorageClass::Private)});
                    variable->RemoveFromList();
                    irContext->AddGlobalValue(std::unique_ptr<Instruction>(variable));

                    // SPIR-V 1.3 lists only Input/Output in the entry-point interface, and the
                    // block is neither any more: it is replaced in place by its members.
                    std::vector<Operand> rebuilt;
                    for (Uint32 i = 0; i < interfaceOperands.size(); ++i) {
                        const Operand& operand = interfaceOperands[i];
                        if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID && operand.words.size() == 1 &&
                            operand.words[0] == oldVariableId) {
                            for (const auto& member : target.members) {
                                rebuilt.push_back({SPV_OPERAND_TYPE_ID, {member.variableId}});
                            }
                            continue;
                        }
                        rebuilt.push_back(operand);
                    }
                    interfaceOperands = std::move(rebuilt);
                }

                entryPoint->SetInOperands(std::move(interfaceOperands));

                // Index constants for the copy access chains. Appended after the variables
                // above, which is legal: nothing declared before them names them, and the only
                // instructions that do are the ones inserted into the function body below.
                auto* constMgr = irContext->get_constant_mgr();
                std::vector<Uint32> memberIndexConstants;
                Uint32 maxMembers = 0;
                for (const auto& target : targets) {
                    maxMembers = std::max(maxMembers, static_cast<Uint32>(target.members.size()));
                }
                memberIndexConstants.resize(maxMembers, 0);
                for (Uint32 index = 0; index < maxMembers; ++index) {
                    memberIndexConstants[index] = constMgr->GetUIntConstId(index);
                }

                // Input: seed the shadow once, before any code that reads it. Output: publish
                // it at every exit, after all the code that writes it.
                auto& entryBlock = *entryFunction->begin();
                auto insertPoint = entryBlock.begin();
                while (insertPoint != entryBlock.end() && insertPoint->opcode() == spv::Op::OpVariable) {
                    ++insertPoint;
                }
                if (insertPoint == entryBlock.end()) return Status::SuccessWithoutChange;

                for (const auto& target : targets) {
                    if (target.storageClass != spv::StorageClass::Input) continue;
                    for (Uint32 memberIndex = 0; memberIndex < target.members.size(); ++memberIndex) {
                        const auto& member = target.members[memberIndex];
                        const Uint32 loadedId = irContext->TakeNextId();
                        const Uint32 memberPointerId = irContext->TakeNextId();
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpLoad, member.typeId, loadedId,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {member.variableId}}}));
                        ++insertPoint;
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpAccessChain, member.privatePointerTypeId, memberPointerId,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_ID, {target.variable->result_id()}},
                                {SPV_OPERAND_TYPE_ID, {memberIndexConstants[memberIndex]}}}));
                        ++insertPoint;
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpStore, 0, 0,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {memberPointerId}},
                                                           {SPV_OPERAND_TYPE_ID, {loadedId}}}));
                        ++insertPoint;
                    }
                }

                // EVERY return, not just the last block: a shader with an early `return` would
                // otherwise publish nothing on that path.
                std::vector<Instruction*> returns;
                for (auto& block : *entryFunction) {
                    Instruction* terminator = block.terminator();
                    if (terminator != nullptr && terminator->opcode() == spv::Op::OpReturn) {
                        returns.push_back(terminator);
                    }
                }
                for (Instruction* returnInst : returns) {
                    for (const auto& target : targets) {
                        if (target.storageClass != spv::StorageClass::Output) continue;
                        for (Uint32 memberIndex = 0; memberIndex < target.members.size(); ++memberIndex) {
                            const auto& member = target.members[memberIndex];
                            const Uint32 memberPointerId = irContext->TakeNextId();
                            const Uint32 loadedId = irContext->TakeNextId();
                            returnInst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                irContext, spv::Op::OpAccessChain, member.privatePointerTypeId,
                                memberPointerId,
                                std::initializer_list<Operand>{
                                    {SPV_OPERAND_TYPE_ID, {target.variable->result_id()}},
                                    {SPV_OPERAND_TYPE_ID, {memberIndexConstants[memberIndex]}}}));
                            returnInst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                irContext, spv::Op::OpLoad, member.typeId, loadedId,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {memberPointerId}}}));
                            returnInst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                irContext, spv::Op::OpStore, 0, 0,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {member.variableId}},
                                                               {SPV_OPERAND_TYPE_ID, {loadedId}}}));
                        }
                    }
                }

                // Retype the derived pointers collected above. Done last, so the pointer types it
                // appends land after the variables.
                for (Instruction* pointer : pointersToRetype) {
                    Instruction* resultType = defUseMgr->GetDef(pointer->type_id());
                    if (resultType == nullptr || resultType->opcode() != spv::Op::OpTypePointer) continue;
                    if (static_cast<spv::StorageClass>(resultType->GetSingleWordInOperand(0)) ==
                        spv::StorageClass::Private) {
                        continue;
                    }
                    pointer->SetResultType(typeMgr->FindPointerToType(resultType->GetSingleWordInOperand(1),
                                                                      spv::StorageClass::Private));
                }

                if (m_flattenedBlockNames != nullptr) {
                    for (const auto& target : targets) {
                        m_flattenedBlockNames->insert(target.blockName);
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken FlattenXfbInterfaceBlocksPass::CreateFlattenXfbInterfaceBlocksPass(
                const std::set<String>& blockNames, std::set<String>* flattenedBlockNames) {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<FlattenXfbInterfaceBlocksPass>(blockNames, flattenedBlockNames));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
