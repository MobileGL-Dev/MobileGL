// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateSubgroupsPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EmulateSubgroupsPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <array>
#include <map>
#include <set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                // The GL-visible subgroup this pass implements. Keep in sync with
                // SubgroupSupportPolicy.h (kEmulatedSubgroupSize).
                constexpr uint32_t kWidth = 32u;
                constexpr uint32_t kLaneMask = kWidth - 1u;
                constexpr uint32_t kIdShift = 5u;

                // GLSL.std.450 instruction numbers (see 3rdparty/glslang/SPIRV/GLSL.std.450.h).
                constexpr uint32_t kGlslFMin = 37u;
                constexpr uint32_t kGlslUMin = 38u;
                constexpr uint32_t kGlslSMin = 39u;
                constexpr uint32_t kGlslFMax = 40u;
                constexpr uint32_t kGlslUMax = 41u;
                constexpr uint32_t kGlslSMax = 42u;
                constexpr uint32_t kGlslFindILsb = 73u;
                constexpr uint32_t kGlslFindUMsb = 75u;

                Operand IdOp(uint32_t id) { return {SPV_OPERAND_TYPE_ID, {id}}; }

                Instruction* FindBuiltinDefinition(IRContext* context, spv::BuiltIn builtin) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn) {
                            continue;
                        }
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != builtin) {
                            continue;
                        }
                        return defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                    }
                    return nullptr;
                }

                // The scalar shapes a value participating in an emulated exchange may have.
                enum class ScalarKind { Float, SInt, UInt, Bool };

                struct ValueTypeInfo {
                    ScalarKind kind = ScalarKind::UInt;
                    uint32_t componentCount = 1;  // 1 for scalars, 2..4 for vectors
                    uint32_t typeId = 0;          // the value's own type
                    uint32_t scalarTypeId = 0;    // the component type
                };

                bool ClassifyScalar(const Instruction* type, ScalarKind* kind) {
                    switch (type->opcode()) {
                    case spv::Op::OpTypeBool:
                        *kind = ScalarKind::Bool;
                        return true;
                    case spv::Op::OpTypeInt:
                        if (type->GetSingleWordInOperand(0) != 32u) return false;
                        *kind = type->GetSingleWordInOperand(1) == 0u ? ScalarKind::UInt : ScalarKind::SInt;
                        return true;
                    case spv::Op::OpTypeFloat:
                        if (type->GetSingleWordInOperand(0) != 32u) return false;
                        *kind = ScalarKind::Float;
                        return true;
                    default:
                        return false;
                    }
                }

                bool ClassifyValueType(IRContext* context, uint32_t typeId, ValueTypeInfo* info) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    if (type == nullptr) return false;
                    info->typeId = typeId;
                    if (type->opcode() == spv::Op::OpTypeVector) {
                        info->componentCount = type->GetSingleWordInOperand(1);
                        if (info->componentCount < 2u || info->componentCount > 4u) return false;
                        info->scalarTypeId = type->GetSingleWordInOperand(0);
                        const Instruction* component = context->get_def_use_mgr()->GetDef(info->scalarTypeId);
                        return component != nullptr && ClassifyScalar(component, &info->kind);
                    }
                    info->componentCount = 1u;
                    info->scalarTypeId = typeId;
                    return ClassifyScalar(type, &info->kind);
                }

                // Emits instructions immediately before a fixed anchor instruction. Every
                // lowering sequence is straight-line code in the anchor's own block, so
                // dominance is trivially preserved.
                class Emitter {
                public:
                    Emitter(IRContext* context, Instruction* anchor) : m_context(context), m_anchor(anchor) {}

                    uint32_t Emit(spv::Op opcode, uint32_t typeId, Instruction::OperandList operands) {
                        const uint32_t resultId = m_context->TakeNextId();
                        m_anchor->InsertBefore(spvtools::MakeUnique<Instruction>(
                            m_context, opcode, typeId, resultId, std::move(operands)));
                        return resultId;
                    }

                    void EmitNoResult(spv::Op opcode, Instruction::OperandList operands) {
                        m_anchor->InsertBefore(spvtools::MakeUnique<Instruction>(
                            m_context, opcode, 0, 0, std::move(operands)));
                    }

                private:
                    IRContext* m_context;
                    Instruction* m_anchor;
                };

                // One recognized OpGroupNonUniform* site with everything Phase A resolved
                // about it, so Phase B can rewrite without def-use queries.
                struct GroupOpSite {
                    Instruction* inst = nullptr;
                    spv::Op opcode = spv::Op::OpNop;
                    spv::GroupOperation groupOperation = spv::GroupOperation::Reduce;
                    uint32_t clusterSize = 0;    // ClusteredReduce only
                    uint32_t quadDirection = 0;  // QuadSwap only
                    ValueTypeInfo valueType{};   // participating value (where applicable)
                    uint32_t valueId = 0;
                    uint32_t indexId = 0;        // broadcast/shuffle/extract index operand
                };

                struct BarrierSite {
                    Instruction* inst = nullptr;
                    bool executionScopeIsSubgroup = false;
                    bool memoryScopeIsSubgroup = false;
                };

                bool IsGroupNonUniformOpcode(spv::Op opcode) {
                    return opcode >= spv::Op::OpGroupNonUniformElect &&
                           opcode <= spv::Op::OpGroupNonUniformQuadSwap;
                }
            } // namespace

            spvtools::opt::Pass::Status EmulateSubgroupsPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* constMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                // ---------------------------------------------------------------- Phase A
                // Pure analysis; nothing is mutated until every site has been vetted, so a
                // Failure return leaves the module untouched.

                bool allEntryPointsCompute = true;
                for (const Instruction& entryPoint : irContext->module()->entry_points()) {
                    if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) !=
                        spv::ExecutionModel::GLCompute) {
                        allEntryPointsCompute = false;
                    }
                }

                const auto resolveUintConstant = [&](uint32_t id, uint32_t* value) {
                    const Instruction* def = defUseMgr->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant) return false;
                    *value = def->GetSingleWordInOperand(0);
                    return true;
                };

                // Subgroup builtin variables and their loads.
                static constexpr std::array<spv::BuiltIn, 9> kSubgroupBuiltins = {
                    spv::BuiltIn::SubgroupSize,      spv::BuiltIn::SubgroupLocalInvocationId,
                    spv::BuiltIn::SubgroupId,        spv::BuiltIn::NumSubgroups,
                    spv::BuiltIn::SubgroupEqMask,    spv::BuiltIn::SubgroupGeMask,
                    spv::BuiltIn::SubgroupGtMask,    spv::BuiltIn::SubgroupLeMask,
                    spv::BuiltIn::SubgroupLtMask};

                const auto isMaskBuiltin = [](spv::BuiltIn builtin) {
                    return builtin == spv::BuiltIn::SubgroupEqMask ||
                           builtin == spv::BuiltIn::SubgroupGeMask ||
                           builtin == spv::BuiltIn::SubgroupGtMask ||
                           builtin == spv::BuiltIn::SubgroupLeMask ||
                           builtin == spv::BuiltIn::SubgroupLtMask;
                };
                struct BuiltinUse {
                    spv::BuiltIn builtin;
                    Instruction* variable;
                    std::vector<Instruction*> loads;
                    // Mask builtins are uvec4; glslang reaches a single component (the
                    // ubiquitous gl_Subgroup*Mask.x) through an access chain plus a scalar
                    // load. The chain dies with the variable once its loads are rewritten.
                    std::vector<Instruction*> accessChains;
                    std::vector<std::pair<Instruction*, uint32_t>> componentLoads;
                };
                std::vector<BuiltinUse> builtinUses;
                bool sawUnexpectedUser = false;
                for (const spv::BuiltIn builtin : kSubgroupBuiltins) {
                    Instruction* variable = FindBuiltinDefinition(irContext, builtin);
                    if (variable == nullptr || variable->opcode() != spv::Op::OpVariable) continue;
                    BuiltinUse use{builtin, variable, {}, {}, {}};
                    const uint32_t variableId = variable->result_id();
                    defUseMgr->ForEachUser(variable, [&](Instruction* user) {
                        switch (user->opcode()) {
                        case spv::Op::OpLoad:
                            if (user->NumInOperands() >= 1 &&
                                user->GetSingleWordInOperand(0) == variableId) {
                                use.loads.push_back(user);
                            } else {
                                sawUnexpectedUser = true;
                            }
                            return;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain: {
                            uint32_t component = 0;
                            if (!isMaskBuiltin(builtin) || user->NumInOperands() != 2 ||
                                user->GetSingleWordInOperand(0) != variableId) {
                                sawUnexpectedUser = true;
                                return;
                            }
                            const Instruction* index =
                                defUseMgr->GetDef(user->GetSingleWordInOperand(1));
                            if (index == nullptr || index->opcode() != spv::Op::OpConstant ||
                                (component = index->GetSingleWordInOperand(0)) > 3u) {
                                sawUnexpectedUser = true;
                                return;
                            }
                            const uint32_t chainId = user->result_id();
                            defUseMgr->ForEachUser(user, [&](Instruction* chainUser) {
                                if (chainUser->opcode() == spv::Op::OpLoad &&
                                    chainUser->NumInOperands() >= 1 &&
                                    chainUser->GetSingleWordInOperand(0) == chainId) {
                                    use.componentLoads.emplace_back(chainUser, component);
                                } else {
                                    sawUnexpectedUser = true;
                                }
                            });
                            use.accessChains.push_back(user);
                            return;
                        }
                        case spv::Op::OpDecorate:
                        case spv::Op::OpDecorateId:
                        case spv::Op::OpDecorateString:
                        case spv::Op::OpName:
                        case spv::Op::OpEntryPoint:
                            return;
                        default:
                            sawUnexpectedUser = true;
                            return;
                        }
                    });
                    builtinUses.push_back(std::move(use));
                }

                // Group-nonuniform operations and subgroup-scoped barriers.
                std::vector<GroupOpSite> groupOps;
                std::vector<BarrierSite> barriers;
                bool sawUnsupported = false;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            const spv::Op opcode = inst.opcode();
                            // Extended subgroup instructions live outside the core
                            // [Elect..QuadSwap] range IsGroupNonUniformOpcode covers; they
                            // must fail the pass rather than survive into the "subgroup
                            // free" output.
                            if (opcode == spv::Op::OpGroupNonUniformPartitionNV ||
                                opcode == spv::Op::OpGroupNonUniformRotateKHR ||
                                opcode == spv::Op::OpGroupNonUniformQuadAllKHR ||
                                opcode == spv::Op::OpGroupNonUniformQuadAnyKHR) {
                                sawUnsupported = true;
                                continue;
                            }
                            if (opcode == spv::Op::OpControlBarrier) {
                                BarrierSite site{&inst, false, false};
                                uint32_t scope = 0;
                                if (resolveUintConstant(inst.GetSingleWordInOperand(0), &scope) &&
                                    scope == static_cast<uint32_t>(spv::Scope::Subgroup)) {
                                    site.executionScopeIsSubgroup = true;
                                }
                                if (resolveUintConstant(inst.GetSingleWordInOperand(1), &scope) &&
                                    scope == static_cast<uint32_t>(spv::Scope::Subgroup)) {
                                    site.memoryScopeIsSubgroup = true;
                                }
                                if (site.executionScopeIsSubgroup || site.memoryScopeIsSubgroup) {
                                    barriers.push_back(site);
                                }
                                continue;
                            }
                            if (opcode == spv::Op::OpMemoryBarrier) {
                                uint32_t scope = 0;
                                if (resolveUintConstant(inst.GetSingleWordInOperand(0), &scope) &&
                                    scope == static_cast<uint32_t>(spv::Scope::Subgroup)) {
                                    barriers.push_back(BarrierSite{&inst, false, true});
                                }
                                continue;
                            }
                            if (!IsGroupNonUniformOpcode(opcode)) continue;

                            GroupOpSite site;
                            site.inst = &inst;
                            site.opcode = opcode;
                            uint32_t scope = 0;
                            if (!resolveUintConstant(inst.GetSingleWordInOperand(0), &scope) ||
                                scope != static_cast<uint32_t>(spv::Scope::Subgroup)) {
                                sawUnsupported = true;
                                continue;
                            }
                            switch (opcode) {
                            case spv::Op::OpGroupNonUniformElect:
                                break;
                            case spv::Op::OpGroupNonUniformAll:
                            case spv::Op::OpGroupNonUniformAny:
                            case spv::Op::OpGroupNonUniformAllEqual:
                            case spv::Op::OpGroupNonUniformBroadcastFirst:
                            case spv::Op::OpGroupNonUniformBallot:
                            case spv::Op::OpGroupNonUniformInverseBallot:
                            case spv::Op::OpGroupNonUniformBallotFindLSB:
                            case spv::Op::OpGroupNonUniformBallotFindMSB:
                                site.valueId = inst.GetSingleWordInOperand(1);
                                break;
                            case spv::Op::OpGroupNonUniformBroadcast:
                            case spv::Op::OpGroupNonUniformShuffle:
                            case spv::Op::OpGroupNonUniformShuffleXor:
                            case spv::Op::OpGroupNonUniformShuffleUp:
                            case spv::Op::OpGroupNonUniformShuffleDown:
                            case spv::Op::OpGroupNonUniformQuadBroadcast:
                            case spv::Op::OpGroupNonUniformBallotBitExtract:
                                site.valueId = inst.GetSingleWordInOperand(1);
                                site.indexId = inst.GetSingleWordInOperand(2);
                                break;
                            case spv::Op::OpGroupNonUniformQuadSwap:
                                site.valueId = inst.GetSingleWordInOperand(1);
                                if (!resolveUintConstant(inst.GetSingleWordInOperand(2),
                                                         &site.quadDirection) ||
                                    site.quadDirection > 2u) {
                                    sawUnsupported = true;
                                }
                                break;
                            case spv::Op::OpGroupNonUniformBallotBitCount:
                                site.groupOperation = static_cast<spv::GroupOperation>(
                                    inst.GetSingleWordInOperand(1));
                                site.valueId = inst.GetSingleWordInOperand(2);
                                break;
                            case spv::Op::OpGroupNonUniformIAdd:
                            case spv::Op::OpGroupNonUniformFAdd:
                            case spv::Op::OpGroupNonUniformIMul:
                            case spv::Op::OpGroupNonUniformFMul:
                            case spv::Op::OpGroupNonUniformSMin:
                            case spv::Op::OpGroupNonUniformUMin:
                            case spv::Op::OpGroupNonUniformFMin:
                            case spv::Op::OpGroupNonUniformSMax:
                            case spv::Op::OpGroupNonUniformUMax:
                            case spv::Op::OpGroupNonUniformFMax:
                            case spv::Op::OpGroupNonUniformBitwiseAnd:
                            case spv::Op::OpGroupNonUniformBitwiseOr:
                            case spv::Op::OpGroupNonUniformBitwiseXor:
                            case spv::Op::OpGroupNonUniformLogicalAnd:
                            case spv::Op::OpGroupNonUniformLogicalOr:
                            case spv::Op::OpGroupNonUniformLogicalXor:
                                site.groupOperation = static_cast<spv::GroupOperation>(
                                    inst.GetSingleWordInOperand(1));
                                site.valueId = inst.GetSingleWordInOperand(2);
                                if (site.groupOperation == spv::GroupOperation::ClusteredReduce) {
                                    if (inst.NumInOperands() < 4 ||
                                        !resolveUintConstant(inst.GetSingleWordInOperand(3),
                                                             &site.clusterSize) ||
                                        site.clusterSize == 0u ||
                                        (site.clusterSize & (site.clusterSize - 1u)) != 0u) {
                                        sawUnsupported = true;
                                    }
                                } else if (site.groupOperation != spv::GroupOperation::Reduce &&
                                           site.groupOperation != spv::GroupOperation::InclusiveScan &&
                                           site.groupOperation != spv::GroupOperation::ExclusiveScan) {
                                    sawUnsupported = true;
                                }
                                break;
                            default:
                                sawUnsupported = true;
                                continue;
                            }

                            // Classify the participating value where the lowering exchanges
                            // it through shared memory (everything except Elect and the pure
                            // ballot-math ops, whose operand is the uvec4 ballot itself).
                            switch (opcode) {
                            case spv::Op::OpGroupNonUniformElect:
                            case spv::Op::OpGroupNonUniformInverseBallot:
                            case spv::Op::OpGroupNonUniformBallotBitExtract:
                            case spv::Op::OpGroupNonUniformBallotBitCount:
                            case spv::Op::OpGroupNonUniformBallotFindLSB:
                            case spv::Op::OpGroupNonUniformBallotFindMSB:
                                break;
                            case spv::Op::OpGroupNonUniformAll:
                            case spv::Op::OpGroupNonUniformAny:
                            case spv::Op::OpGroupNonUniformBallot: {
                                const Instruction* value = defUseMgr->GetDef(site.valueId);
                                if (value == nullptr ||
                                    !ClassifyValueType(irContext, value->type_id(), &site.valueType) ||
                                    site.valueType.kind != ScalarKind::Bool ||
                                    site.valueType.componentCount != 1u) {
                                    sawUnsupported = true;
                                }
                                break;
                            }
                            default: {
                                const Instruction* value = defUseMgr->GetDef(site.valueId);
                                if (value == nullptr ||
                                    !ClassifyValueType(irContext, value->type_id(), &site.valueType)) {
                                    sawUnsupported = true;
                                }
                                break;
                            }
                            }
                            groupOps.push_back(site);
                        }
                    }
                }

                bool anyBuiltinLoads = false;
                for (const BuiltinUse& use : builtinUses) {
                    if (!use.loads.empty() || !use.componentLoads.empty()) anyBuiltinLoads = true;
                }
                if (!anyBuiltinLoads && groupOps.empty() && barriers.empty() && !sawUnsupported) {
                    return Status::SuccessWithoutChange;
                }
                if (!allEntryPointsCompute || sawUnexpectedUser || sawUnsupported) {
                    return Status::Failure;
                }

                // Workgroup size. The compile chain pins SPIR-V 1.3, where a literal
                // local_size always reaches the module as OpExecutionMode LocalSize and -
                // through glslang - as the WorkgroupSize builtin constant as well. Either
                // is accepted; spec-constant sizes are not (GL has no specialization).
                uint32_t localSize[3] = {0, 0, 0};
                bool haveLocalSize = false;
                if (Instruction* workgroupSize =
                        FindBuiltinDefinition(irContext, spv::BuiltIn::WorkgroupSize)) {
                    if (workgroupSize->opcode() == spv::Op::OpConstantComposite &&
                        workgroupSize->NumInOperands() == 3) {
                        haveLocalSize =
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(0), &localSize[0]) &&
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(1), &localSize[1]) &&
                            resolveUintConstant(workgroupSize->GetSingleWordInOperand(2), &localSize[2]);
                    }
                }
                if (!haveLocalSize) {
                    for (const Instruction& mode : irContext->module()->execution_modes()) {
                        if (mode.opcode() == spv::Op::OpExecutionMode &&
                            static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(1)) ==
                                spv::ExecutionMode::LocalSize) {
                            localSize[0] = mode.GetSingleWordInOperand(2);
                            localSize[1] = mode.GetSingleWordInOperand(3);
                            localSize[2] = mode.GetSingleWordInOperand(4);
                            haveLocalSize = true;
                            break;
                        }
                    }
                }
                if (!haveLocalSize || localSize[0] == 0u || localSize[1] == 0u || localSize[2] == 0u) {
                    return Status::Failure;
                }
                const uint64_t total64 =
                    static_cast<uint64_t>(localSize[0]) * localSize[1] * localSize[2];
                if (total64 == 0u || total64 > (1u << 20)) {
                    return Status::Failure;
                }
                const uint32_t totalInvocations = static_cast<uint32_t>(total64);
                const uint32_t virtualSubgroups = (totalInvocations + kWidth - 1u) / kWidth;
                const uint32_t paddedSlots = virtualSubgroups * kWidth;

                // Phase C creates one Workgroup array of paddedSlots elements per distinct
                // participating element type (bools travel as uint words), 4 bytes per
                // component. Refuse up front any module whose added scratch would exceed
                // the caller's budget - the device's maxComputeSharedMemorySize, or the
                // 16384-byte Vulkan minimum when the caller passed 0 - since spirv-val
                // cannot catch this and the pipeline would fail at creation instead. The
                // estimate over-counts slightly (a bool type and its uint twin are keyed
                // separately) and does not subtract the module's own shared declarations.
                {
                    const uint64_t scratchBudgetBytes =
                        m_maxWorkgroupScratchBytes != 0u ? m_maxWorkgroupScratchBytes : 16384u;
                    std::set<uint32_t> scratchTypeKeys;
                    uint64_t scratchBytes = 0;
                    for (const GroupOpSite& site : groupOps) {
                        if (site.valueType.typeId == 0u) continue;  // no shared-memory exchange
                        const uint32_t key = site.valueType.kind == ScalarKind::Bool
                                                 ? (0x80000000u | site.valueType.componentCount)
                                                 : site.valueType.typeId;
                        if (!scratchTypeKeys.insert(key).second) continue;
                        scratchBytes += static_cast<uint64_t>(paddedSlots) * 4u *
                                        site.valueType.componentCount;
                    }
                    if (scratchBytes > scratchBudgetBytes) {
                        return Status::Failure;
                    }
                }

                // ---------------------------------------------------------------- Phase B
                // Cached types and constants.
                analysis::Bool boolTypeCandidate;
                const uint32_t boolTypeId = typeMgr->GetTypeInstruction(&boolTypeCandidate);
                analysis::Integer uintTypeCandidate(32, false);
                const uint32_t uintTypeId = typeMgr->GetTypeInstruction(&uintTypeCandidate);
                if (boolTypeId == 0u || uintTypeId == 0u) {
                    return Status::Failure;
                }

                const auto uintConst = [&](uint32_t value) -> uint32_t {
                    const analysis::Type* type = typeMgr->GetType(uintTypeId);
                    const analysis::Constant* constant = constMgr->GetConstant(type, {value});
                    const Instruction* inst =
                        constant != nullptr ? constMgr->GetDefiningInstruction(constant, uintTypeId)
                                            : nullptr;
                    return inst != nullptr ? inst->result_id() : 0u;
                };
                const auto boolConst = [&](bool value) -> uint32_t {
                    const analysis::Type* type = typeMgr->GetType(boolTypeId);
                    const analysis::Constant* constant =
                        constMgr->GetConstant(type, {value ? 1u : 0u});
                    const Instruction* inst =
                        constant != nullptr ? constMgr->GetDefiningInstruction(constant, boolTypeId)
                                            : nullptr;
                    return inst != nullptr ? inst->result_id() : 0u;
                };
                // A constant of an arbitrary 32-bit scalar type from its bit pattern, splat
                // to the value type when it is a vector.
                const auto typedConst = [&](const ValueTypeInfo& info, uint32_t scalarBits,
                                            uint32_t scratchScalarTypeId,
                                            uint32_t scratchTypeId) -> uint32_t {
                    const analysis::Type* scalarType = typeMgr->GetType(scratchScalarTypeId);
                    const analysis::Constant* scalar = constMgr->GetConstant(scalarType, {scalarBits});
                    if (scalar == nullptr) return 0u;
                    const Instruction* scalarInst =
                        constMgr->GetDefiningInstruction(scalar, scratchScalarTypeId);
                    if (scalarInst == nullptr) return 0u;
                    if (info.componentCount == 1u) return scalarInst->result_id();
                    const analysis::Type* vectorType = typeMgr->GetType(scratchTypeId);
                    std::vector<uint32_t> componentIds(info.componentCount,
                                                       scalarInst->result_id());
                    const analysis::Constant* vector = constMgr->GetConstant(vectorType, componentIds);
                    if (vector == nullptr) return 0u;
                    const Instruction* vectorInst =
                        constMgr->GetDefiningInstruction(vector, scratchTypeId);
                    return vectorInst != nullptr ? vectorInst->result_id() : 0u;
                };

                const uint32_t c0 = uintConst(0u);
                const uint32_t c1 = uintConst(1u);
                const uint32_t c31 = uintConst(kLaneMask);
                const uint32_t c32 = uintConst(kWidth);
                const uint32_t cNotLane = uintConst(~kLaneMask);
                const uint32_t cTotal = uintConst(totalInvocations);
                const uint32_t cAllOnes = uintConst(0xffffffffu);
                const uint32_t cTrue = boolConst(true);
                if (c0 == 0u || c1 == 0u || c31 == 0u || c32 == 0u || cNotLane == 0u ||
                    cTotal == 0u || cAllOnes == 0u || cTrue == 0u) {
                    return Status::Failure;
                }

                // GLSL.std.450 import (for min/max combines and ballot find ops).
                uint32_t glslStd450Id = 0;
                for (const Instruction& import : irContext->module()->ext_inst_imports()) {
                    const std::string importName =
                        spvtools::utils::MakeString(import.GetInOperand(0).words);
                    if (importName == "GLSL.std.450") {
                        glslStd450Id = import.result_id();
                        break;
                    }
                }
                const auto ensureGlslStd450 = [&]() -> uint32_t {
                    if (glslStd450Id != 0u) return glslStd450Id;
                    glslStd450Id = irContext->TakeNextId();
                    irContext->module()->AddExtInstImport(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpExtInstImport, 0, glslStd450Id,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_LITERAL_STRING,
                             spvtools::utils::MakeVector("GLSL.std.450")}}));
                    return glslStd450Id;
                };

                // gl_LocalInvocationIndex drives the whole virtual topology; synthesize the
                // builtin when the module never declared it.
                Instruction* liiVariable =
                    FindBuiltinDefinition(irContext, spv::BuiltIn::LocalInvocationIndex);
                uint32_t liiVariableId = 0;
                if (liiVariable != nullptr && liiVariable->opcode() == spv::Op::OpVariable) {
                    liiVariableId = liiVariable->result_id();
                } else {
                    const uint32_t pointerTypeId =
                        typeMgr->FindPointerToType(uintTypeId, spv::StorageClass::Input);
                    if (pointerTypeId == 0u) return Status::Failure;
                    liiVariableId = irContext->TakeNextId();
                    irContext->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpVariable, pointerTypeId, liiVariableId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Input)}}}));
                    irContext->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpDecorate, 0, 0,
                        Instruction::OperandList{
                            IdOp(liiVariableId),
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::BuiltIn)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER,
                             {static_cast<uint32_t>(spv::BuiltIn::LocalInvocationIndex)}}}));
                    for (Instruction& entryPoint : irContext->module()->entry_points()) {
                        entryPoint.AddOperand(IdOp(liiVariableId));
                    }
                }

                // Scratch arrays, one per participating element type, each padded to whole
                // virtual subgroups so every guarded read stays in bounds.
                struct Scratch {
                    uint32_t variableId = 0;
                    uint32_t elementTypeId = 0;
                    uint32_t pointerTypeId = 0;
                };
                std::map<uint32_t, Scratch> scratchByType;
                const uint32_t cPadded = uintConst(paddedSlots);
                if (cPadded == 0u) return Status::Failure;
                const auto scratchFor = [&](uint32_t elementTypeId) -> const Scratch* {
                    auto it = scratchByType.find(elementTypeId);
                    if (it != scratchByType.end()) return &it->second;
                    const uint32_t arrayTypeId = irContext->TakeNextId();
                    irContext->AddType(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpTypeArray, 0, arrayTypeId,
                        Instruction::OperandList{IdOp(elementTypeId), IdOp(cPadded)}));
                    const uint32_t arrayPointerTypeId =
                        typeMgr->FindPointerToType(arrayTypeId, spv::StorageClass::Workgroup);
                    const uint32_t elementPointerTypeId =
                        typeMgr->FindPointerToType(elementTypeId, spv::StorageClass::Workgroup);
                    if (arrayPointerTypeId == 0u || elementPointerTypeId == 0u) return nullptr;
                    const uint32_t variableId = irContext->TakeNextId();
                    irContext->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpVariable, arrayPointerTypeId, variableId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Workgroup)}}}));
                    Scratch scratch{variableId, elementTypeId, elementPointerTypeId};
                    return &scratchByType.emplace(elementTypeId, scratch).first->second;
                };

                // The uint-domain twin of a participating type: bools travel as 0/1 words.
                struct ScratchTypeInfo {
                    uint32_t typeId = 0;        // element type stored in scratch
                    uint32_t scalarTypeId = 0;  // its component type
                    bool boolConverted = false;
                };
                const auto scratchTypeFor = [&](const ValueTypeInfo& info) -> ScratchTypeInfo {
                    if (info.kind != ScalarKind::Bool) {
                        return {info.typeId, info.scalarTypeId, false};
                    }
                    if (info.componentCount == 1u) {
                        return {uintTypeId, uintTypeId, true};
                    }
                    const analysis::Type* registeredUint = typeMgr->GetType(uintTypeId);
                    analysis::Vector vectorType(registeredUint, info.componentCount);
                    const uint32_t vectorTypeId = typeMgr->GetTypeInstruction(&vectorType);
                    return {vectorTypeId, uintTypeId, true};
                };

                const auto boolVectorTypeId = [&](uint32_t componentCount) -> uint32_t {
                    if (componentCount == 1u) return boolTypeId;
                    const analysis::Type* registeredBool = typeMgr->GetType(boolTypeId);
                    analysis::Vector vectorType(registeredBool, componentCount);
                    return typeMgr->GetTypeInstruction(&vectorType);
                };

                // Rewrite the builtin loads. Every rewrite keeps the original result id, so
                // downstream uses see the derived value with no further surgery.
                const uint32_t numVirtualConstId = uintConst(virtualSubgroups);
                if (numVirtualConstId == 0u) return Status::Failure;

                // The x word of one of the five ballot-mask builtins for the calling
                // invocation's virtual subgroup, bounded to lanes that exist (words y..w
                // are zero at 32 lanes).
                const auto emitMaskWord = [&](Emitter& em, spv::BuiltIn builtin) -> uint32_t {
                    const uint32_t index =
                        em.Emit(spv::Op::OpLoad, uintTypeId, {IdOp(liiVariableId)});
                    const uint32_t lane =
                        em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(index), IdOp(c31)});
                    const uint32_t base = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                  {IdOp(index), IdOp(cNotLane)});
                    const uint32_t tailBits =
                        em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(cTotal), IdOp(base)});
                    const uint32_t full = em.Emit(spv::Op::OpUGreaterThanEqual, boolTypeId,
                                                  {IdOp(tailBits), IdOp(c32)});
                    const uint32_t tailShift = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                       {IdOp(c1), IdOp(tailBits)});
                    const uint32_t tailMask =
                        em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(tailShift), IdOp(c1)});
                    const uint32_t exists = em.Emit(spv::Op::OpSelect, uintTypeId,
                                                    {IdOp(full), IdOp(cAllOnes), IdOp(tailMask)});
                    const auto lowMaskThroughLane = [&](bool inclusive) -> uint32_t {
                        if (!inclusive) {
                            const uint32_t bit = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                         {IdOp(c1), IdOp(lane)});
                            return em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(bit), IdOp(c1)});
                        }
                        const uint32_t lanePlusOne =
                            em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(lane), IdOp(c1)});
                        const uint32_t atTop = em.Emit(spv::Op::OpUGreaterThanEqual, boolTypeId,
                                                       {IdOp(lanePlusOne), IdOp(c32)});
                        const uint32_t shifted = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                         {IdOp(c1), IdOp(lanePlusOne)});
                        const uint32_t below =
                            em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(shifted), IdOp(c1)});
                        return em.Emit(spv::Op::OpSelect, uintTypeId,
                                       {IdOp(atTop), IdOp(cAllOnes), IdOp(below)});
                    };
                    uint32_t word = 0;
                    switch (builtin) {
                    case spv::BuiltIn::SubgroupEqMask:
                        word = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                       {IdOp(c1), IdOp(lane)});
                        break;
                    case spv::BuiltIn::SubgroupLtMask:
                        word = lowMaskThroughLane(false);
                        break;
                    case spv::BuiltIn::SubgroupLeMask:
                        word = lowMaskThroughLane(true);
                        break;
                    case spv::BuiltIn::SubgroupGeMask:
                        word = em.Emit(spv::Op::OpNot, uintTypeId, {IdOp(lowMaskThroughLane(false))});
                        break;
                    case spv::BuiltIn::SubgroupGtMask:
                        word = em.Emit(spv::Op::OpNot, uintTypeId, {IdOp(lowMaskThroughLane(true))});
                        break;
                    default:
                        return 0u;
                    }
                    return em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(word), IdOp(exists)});
                };

                for (const BuiltinUse& use : builtinUses) {
                    for (Instruction* load : use.loads) {
                        Emitter em(irContext, load);
                        switch (use.builtin) {
                        case spv::BuiltIn::SubgroupSize:
                            load->SetOpcode(spv::Op::OpCopyObject);
                            load->SetInOperands({IdOp(c32)});
                            break;
                        case spv::BuiltIn::NumSubgroups:
                            load->SetOpcode(spv::Op::OpCopyObject);
                            load->SetInOperands({IdOp(numVirtualConstId)});
                            break;
                        case spv::BuiltIn::SubgroupLocalInvocationId: {
                            const uint32_t index =
                                em.Emit(spv::Op::OpLoad, uintTypeId, {IdOp(liiVariableId)});
                            load->SetOpcode(spv::Op::OpBitwiseAnd);
                            load->SetInOperands({IdOp(index), IdOp(c31)});
                            break;
                        }
                        case spv::BuiltIn::SubgroupId: {
                            const uint32_t index =
                                em.Emit(spv::Op::OpLoad, uintTypeId, {IdOp(liiVariableId)});
                            load->SetOpcode(spv::Op::OpShiftRightLogical);
                            load->SetInOperands({IdOp(index), IdOp(uintConst(kIdShift))});
                            break;
                        }
                        default: {
                            const uint32_t word = emitMaskWord(em, use.builtin);
                            if (word == 0u) return Status::Failure;
                            load->SetOpcode(spv::Op::OpCompositeConstruct);
                            load->SetInOperands(
                                {IdOp(word), IdOp(c0), IdOp(c0), IdOp(c0)});
                            break;
                        }
                        }
                    }
                    for (const auto& [load, component] : use.componentLoads) {
                        // gl_Subgroup*Mask.x reached through an access chain: only word 0
                        // carries bits at 32 lanes.
                        Emitter em(irContext, load);
                        uint32_t value = c0;
                        if (component == 0u) {
                            value = emitMaskWord(em, use.builtin);
                            if (value == 0u) return Status::Failure;
                        }
                        load->SetOpcode(spv::Op::OpCopyObject);
                        load->SetInOperands({IdOp(value)});
                    }
                }

                // Rewrite the group operations.
                for (const GroupOpSite& site : groupOps) {
                    Instruction* inst = site.inst;
                    Emitter em(irContext, inst);

                    const uint32_t lii =
                        em.Emit(spv::Op::OpLoad, uintTypeId, {IdOp(liiVariableId)});
                    const uint32_t lane =
                        em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(lii), IdOp(c31)});
                    const uint32_t base =
                        em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(lii), IdOp(cNotLane)});

                    const auto barrier = [&]() {
                        const uint32_t scopeId = uintConst(static_cast<uint32_t>(spv::Scope::Workgroup));
                        const uint32_t semanticsId = uintConst(
                            static_cast<uint32_t>(spv::MemorySemanticsMask::AcquireRelease) |
                            static_cast<uint32_t>(spv::MemorySemanticsMask::WorkgroupMemory));
                        em.EmitNoResult(spv::Op::OpControlBarrier,
                                        {IdOp(scopeId), IdOp(scopeId), IdOp(semanticsId)});
                    };

                    const ScratchTypeInfo scratchType = scratchTypeFor(site.valueType);
                    const auto toScratch = [&](uint32_t valueId) -> uint32_t {
                        if (!scratchType.boolConverted) return valueId;
                        const uint32_t ones =
                            typedConst(site.valueType, 1u, uintTypeId, scratchType.typeId);
                        const uint32_t zeros =
                            typedConst(site.valueType, 0u, uintTypeId, scratchType.typeId);
                        return em.Emit(spv::Op::OpSelect, scratchType.typeId,
                                       {IdOp(valueId), IdOp(ones), IdOp(zeros)});
                    };
                    const auto fromScratch = [&](uint32_t valueId) -> uint32_t {
                        if (!scratchType.boolConverted) return valueId;
                        const uint32_t zeros =
                            typedConst(site.valueType, 0u, uintTypeId, scratchType.typeId);
                        return em.Emit(spv::Op::OpINotEqual, site.valueType.typeId,
                                       {IdOp(valueId), IdOp(zeros)});
                    };

                    const Scratch* scratch = nullptr;
                    const auto storeOwnSlot = [&](uint32_t valueId) -> bool {
                        scratch = scratchFor(scratchType.typeId);
                        if (scratch == nullptr) return false;
                        const uint32_t pointer =
                            em.Emit(spv::Op::OpAccessChain, scratch->pointerTypeId,
                                    {IdOp(scratch->variableId), IdOp(lii)});
                        em.EmitNoResult(spv::Op::OpStore, {IdOp(pointer), IdOp(valueId)});
                        barrier();
                        return true;
                    };
                    const auto readSlot = [&](uint32_t indexId) -> uint32_t {
                        const uint32_t pointer =
                            em.Emit(spv::Op::OpAccessChain, scratch->pointerTypeId,
                                    {IdOp(scratch->variableId), IdOp(indexId)});
                        return em.Emit(spv::Op::OpLoad, scratchType.typeId, {IdOp(pointer)});
                    };
                    // A select whose condition is a scalar bool but whose values may be
                    // vectors; pre-SPIR-V-1.4 needs the condition splat to a bvec.
                    const auto guardedSelect = [&](uint32_t condId, uint32_t thenId,
                                                   uint32_t elseId) -> uint32_t {
                        const uint32_t componentCount = site.valueType.componentCount;
                        uint32_t conditionId = condId;
                        if (componentCount > 1u) {
                            const uint32_t conditionTypeId = boolVectorTypeId(componentCount);
                            Instruction::OperandList splat;
                            for (uint32_t i = 0; i < componentCount; ++i) splat.push_back(IdOp(condId));
                            conditionId =
                                em.Emit(spv::Op::OpCompositeConstruct, conditionTypeId, std::move(splat));
                        }
                        return em.Emit(spv::Op::OpSelect, scratchType.typeId,
                                       {IdOp(conditionId), IdOp(thenId), IdOp(elseId)});
                    };
                    const auto extInst2 = [&](uint32_t typeId, uint32_t instNumber, uint32_t a,
                                              uint32_t b) -> uint32_t {
                        const uint32_t setId = ensureGlslStd450();
                        return em.Emit(spv::Op::OpExtInst, typeId,
                                       {IdOp(setId),
                                        {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER, {instNumber}},
                                        IdOp(a), IdOp(b)});
                    };
                    const auto extInst1 = [&](uint32_t typeId, uint32_t instNumber,
                                              uint32_t a) -> uint32_t {
                        const uint32_t setId = ensureGlslStd450();
                        return em.Emit(spv::Op::OpExtInst, typeId,
                                       {IdOp(setId),
                                        {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER, {instNumber}},
                                        IdOp(a)});
                    };

                    // Combine two scratch-domain values with the site's operation.
                    const auto combine = [&](uint32_t a, uint32_t b) -> uint32_t {
                        const uint32_t typeId = scratchType.typeId;
                        switch (site.opcode) {
                        case spv::Op::OpGroupNonUniformIAdd:
                            return em.Emit(spv::Op::OpIAdd, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformFAdd:
                            return em.Emit(spv::Op::OpFAdd, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformIMul:
                            return em.Emit(spv::Op::OpIMul, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformFMul:
                            return em.Emit(spv::Op::OpFMul, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformSMin:
                            return extInst2(typeId, kGlslSMin, a, b);
                        case spv::Op::OpGroupNonUniformUMin:
                            return extInst2(typeId, kGlslUMin, a, b);
                        case spv::Op::OpGroupNonUniformFMin:
                            return extInst2(typeId, kGlslFMin, a, b);
                        case spv::Op::OpGroupNonUniformSMax:
                            return extInst2(typeId, kGlslSMax, a, b);
                        case spv::Op::OpGroupNonUniformUMax:
                            return extInst2(typeId, kGlslUMax, a, b);
                        case spv::Op::OpGroupNonUniformFMax:
                            return extInst2(typeId, kGlslFMax, a, b);
                        case spv::Op::OpGroupNonUniformBitwiseAnd:
                        case spv::Op::OpGroupNonUniformLogicalAnd:
                            return em.Emit(spv::Op::OpBitwiseAnd, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformBitwiseOr:
                        case spv::Op::OpGroupNonUniformLogicalOr:
                            return em.Emit(spv::Op::OpBitwiseOr, typeId, {IdOp(a), IdOp(b)});
                        case spv::Op::OpGroupNonUniformBitwiseXor:
                        case spv::Op::OpGroupNonUniformLogicalXor:
                            return em.Emit(spv::Op::OpBitwiseXor, typeId, {IdOp(a), IdOp(b)});
                        default:
                            return 0u;
                        }
                    };
                    // The identity element for the site's operation, in the scratch domain.
                    const auto identity = [&]() -> uint32_t {
                        const uint32_t typeId = scratchType.typeId;
                        const uint32_t scalarId = scratchType.scalarTypeId;
                        switch (site.opcode) {
                        case spv::Op::OpGroupNonUniformIAdd:
                        case spv::Op::OpGroupNonUniformBitwiseOr:
                        case spv::Op::OpGroupNonUniformBitwiseXor:
                        case spv::Op::OpGroupNonUniformLogicalOr:
                        case spv::Op::OpGroupNonUniformLogicalXor:
                        case spv::Op::OpGroupNonUniformFAdd:
                        case spv::Op::OpGroupNonUniformUMax:
                            // 0 and +0.0f share the bit pattern; exclusive-scan lane 0 of an
                            // FAdd therefore returns +0.0 even for a -0.0 input, matching
                            // the identity the SPIR-V spec assigns.
                            return typedConst(site.valueType, 0u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformIMul:
                            return typedConst(site.valueType, 1u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformFMul:
                            return typedConst(site.valueType, 0x3f800000u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformSMin:
                            return typedConst(site.valueType, 0x7fffffffu, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformUMin:
                            return typedConst(site.valueType, 0xffffffffu, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformFMin:
                            return typedConst(site.valueType, 0x7f800000u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformSMax:
                            return typedConst(site.valueType, 0x80000000u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformFMax:
                            return typedConst(site.valueType, 0xff800000u, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformBitwiseAnd:
                            return typedConst(site.valueType, 0xffffffffu, scalarId, typeId);
                        case spv::Op::OpGroupNonUniformLogicalAnd:
                            return typedConst(site.valueType, 1u, scalarId, typeId);
                        default:
                            return 0u;
                        }
                    };

                    // The lanes-that-exist word for this virtual subgroup (tail handling).
                    const auto existsWord = [&]() -> uint32_t {
                        const uint32_t tailBits =
                            em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(cTotal), IdOp(base)});
                        const uint32_t full = em.Emit(spv::Op::OpUGreaterThanEqual, boolTypeId,
                                                      {IdOp(tailBits), IdOp(c32)});
                        const uint32_t shifted = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                         {IdOp(c1), IdOp(tailBits)});
                        const uint32_t mask =
                            em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(shifted), IdOp(c1)});
                        return em.Emit(spv::Op::OpSelect, uintTypeId,
                                       {IdOp(full), IdOp(cAllOnes), IdOp(mask)});
                    };

                    const auto finish = [&](uint32_t finalId) {
                        inst->SetOpcode(spv::Op::OpCopyObject);
                        inst->SetInOperands({IdOp(finalId)});
                    };

                    bool ok = true;
                    switch (site.opcode) {
                    case spv::Op::OpGroupNonUniformElect: {
                        finish(em.Emit(spv::Op::OpIEqual, boolTypeId, {IdOp(lane), IdOp(c0)}));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformAll:
                    case spv::Op::OpGroupNonUniformAny: {
                        const bool isAll = site.opcode == spv::Op::OpGroupNonUniformAll;
                        ok = storeOwnSlot(toScratch(site.valueId));
                        if (!ok) break;
                        const uint32_t neutral = uintConst(isAll ? 1u : 0u);
                        uint32_t acc = 0;
                        for (uint32_t k = 0; k < kWidth; ++k) {
                            const uint32_t slot =
                                em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(base), IdOp(uintConst(k))});
                            const uint32_t exists = em.Emit(spv::Op::OpULessThan, boolTypeId,
                                                            {IdOp(slot), IdOp(cTotal)});
                            const uint32_t value = readSlot(slot);
                            const uint32_t guarded = em.Emit(
                                spv::Op::OpSelect, uintTypeId,
                                {IdOp(exists), IdOp(value), IdOp(neutral)});
                            acc = k == 0 ? guarded
                                         : em.Emit(isAll ? spv::Op::OpBitwiseAnd : spv::Op::OpBitwiseOr,
                                                   uintTypeId, {IdOp(acc), IdOp(guarded)});
                        }
                        barrier();
                        finish(em.Emit(spv::Op::OpINotEqual, boolTypeId, {IdOp(acc), IdOp(c0)}));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformAllEqual: {
                        ok = storeOwnSlot(toScratch(site.valueId));
                        if (!ok) break;
                        const uint32_t reference = readSlot(base);
                        uint32_t acc = cTrue;
                        for (uint32_t k = 0; k < kWidth; ++k) {
                            const uint32_t slot =
                                em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(base), IdOp(uintConst(k))});
                            const uint32_t exists = em.Emit(spv::Op::OpULessThan, boolTypeId,
                                                            {IdOp(slot), IdOp(cTotal)});
                            const uint32_t value = readSlot(slot);
                            const spv::Op compareOpcode = site.valueType.kind == ScalarKind::Float
                                                              ? spv::Op::OpFOrdEqual
                                                              : spv::Op::OpIEqual;
                            uint32_t equal = 0;
                            if (site.valueType.componentCount == 1u) {
                                equal = em.Emit(compareOpcode, boolTypeId,
                                                {IdOp(value), IdOp(reference)});
                            } else {
                                const uint32_t comparisonTypeId =
                                    boolVectorTypeId(site.valueType.componentCount);
                                const uint32_t componentsEqual = em.Emit(
                                    compareOpcode, comparisonTypeId, {IdOp(value), IdOp(reference)});
                                equal = em.Emit(spv::Op::OpAll, boolTypeId, {IdOp(componentsEqual)});
                            }
                            const uint32_t guarded = em.Emit(
                                spv::Op::OpSelect, boolTypeId,
                                {IdOp(exists), IdOp(equal), IdOp(cTrue)});
                            acc = em.Emit(spv::Op::OpLogicalAnd, boolTypeId,
                                          {IdOp(acc), IdOp(guarded)});
                        }
                        barrier();
                        finish(acc);
                        break;
                    }
                    case spv::Op::OpGroupNonUniformBroadcast:
                    case spv::Op::OpGroupNonUniformBroadcastFirst:
                    case spv::Op::OpGroupNonUniformShuffle:
                    case spv::Op::OpGroupNonUniformShuffleXor:
                    case spv::Op::OpGroupNonUniformShuffleUp:
                    case spv::Op::OpGroupNonUniformShuffleDown:
                    case spv::Op::OpGroupNonUniformQuadBroadcast:
                    case spv::Op::OpGroupNonUniformQuadSwap: {
                        ok = storeOwnSlot(toScratch(site.valueId));
                        if (!ok) break;
                        uint32_t sourceLane = 0;
                        switch (site.opcode) {
                        case spv::Op::OpGroupNonUniformBroadcastFirst:
                            sourceLane = c0;
                            break;
                        case spv::Op::OpGroupNonUniformBroadcast:
                        case spv::Op::OpGroupNonUniformShuffle:
                            sourceLane = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                 {IdOp(site.indexId), IdOp(c31)});
                            break;
                        case spv::Op::OpGroupNonUniformShuffleXor: {
                            const uint32_t flipped = em.Emit(
                                spv::Op::OpBitwiseXor, uintTypeId, {IdOp(lane), IdOp(site.indexId)});
                            sourceLane = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                 {IdOp(flipped), IdOp(c31)});
                            break;
                        }
                        case spv::Op::OpGroupNonUniformShuffleUp: {
                            const uint32_t shifted =
                                em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(lane), IdOp(site.indexId)});
                            sourceLane = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                 {IdOp(shifted), IdOp(c31)});
                            break;
                        }
                        case spv::Op::OpGroupNonUniformShuffleDown: {
                            const uint32_t shifted =
                                em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(lane), IdOp(site.indexId)});
                            sourceLane = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                 {IdOp(shifted), IdOp(c31)});
                            break;
                        }
                        case spv::Op::OpGroupNonUniformQuadBroadcast: {
                            const uint32_t quadBase =
                                em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                        {IdOp(lane), IdOp(uintConst(~3u & kLaneMask))});
                            const uint32_t withinQuad =
                                em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                        {IdOp(site.indexId), IdOp(uintConst(3u))});
                            sourceLane = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                 {IdOp(quadBase), IdOp(withinQuad)});
                            break;
                        }
                        case spv::Op::OpGroupNonUniformQuadSwap: {
                            const uint32_t partnerMask = uintConst(
                                site.quadDirection == 0u ? 1u : site.quadDirection == 1u ? 2u : 3u);
                            sourceLane = em.Emit(spv::Op::OpBitwiseXor, uintTypeId,
                                                 {IdOp(lane), IdOp(partnerMask)});
                            break;
                        }
                        default:
                            break;
                        }
                        const uint32_t sourceSlot =
                            em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(base), IdOp(sourceLane)});
                        const uint32_t value = readSlot(sourceSlot);
                        barrier();
                        finish(fromScratch(value));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformBallot: {
                        ok = storeOwnSlot(toScratch(site.valueId));
                        if (!ok) break;
                        uint32_t word = c0;
                        for (uint32_t k = 0; k < kWidth; ++k) {
                            const uint32_t slot =
                                em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(base), IdOp(uintConst(k))});
                            const uint32_t exists = em.Emit(spv::Op::OpULessThan, boolTypeId,
                                                            {IdOp(slot), IdOp(cTotal)});
                            const uint32_t value = readSlot(slot);
                            const uint32_t guarded = em.Emit(
                                spv::Op::OpSelect, uintTypeId, {IdOp(exists), IdOp(value), IdOp(c0)});
                            const uint32_t bit = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                         {IdOp(guarded), IdOp(uintConst(k))});
                            word = em.Emit(spv::Op::OpBitwiseOr, uintTypeId, {IdOp(word), IdOp(bit)});
                        }
                        barrier();
                        inst->SetOpcode(spv::Op::OpCompositeConstruct);
                        inst->SetInOperands({IdOp(word), IdOp(c0), IdOp(c0), IdOp(c0)});
                        break;
                    }
                    case spv::Op::OpGroupNonUniformInverseBallot: {
                        const uint32_t word = em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                                      {IdOp(site.valueId),
                                                       {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}});
                        const uint32_t shifted = em.Emit(spv::Op::OpShiftRightLogical, uintTypeId,
                                                         {IdOp(word), IdOp(lane)});
                        const uint32_t bit =
                            em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(shifted), IdOp(c1)});
                        finish(em.Emit(spv::Op::OpINotEqual, boolTypeId, {IdOp(bit), IdOp(c0)}));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformBallotBitExtract: {
                        const uint32_t words[4] = {
                            em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                    {IdOp(site.valueId), {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}}),
                            em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                    {IdOp(site.valueId), {SPV_OPERAND_TYPE_LITERAL_INTEGER, {1u}}}),
                            em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                    {IdOp(site.valueId), {SPV_OPERAND_TYPE_LITERAL_INTEGER, {2u}}}),
                            em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                    {IdOp(site.valueId), {SPV_OPERAND_TYPE_LITERAL_INTEGER, {3u}}})};
                        const uint32_t inFirstPair = em.Emit(
                            spv::Op::OpULessThan, boolTypeId, {IdOp(site.indexId), IdOp(uintConst(64u))});
                        const uint32_t inWord0 = em.Emit(
                            spv::Op::OpULessThan, boolTypeId, {IdOp(site.indexId), IdOp(c32)});
                        const uint32_t inWord2 = em.Emit(
                            spv::Op::OpULessThan, boolTypeId, {IdOp(site.indexId), IdOp(uintConst(96u))});
                        const uint32_t firstPair = em.Emit(
                            spv::Op::OpSelect, uintTypeId,
                            {IdOp(inWord0), IdOp(words[0]), IdOp(words[1])});
                        const uint32_t secondPair = em.Emit(
                            spv::Op::OpSelect, uintTypeId,
                            {IdOp(inWord2), IdOp(words[2]), IdOp(words[3])});
                        const uint32_t word = em.Emit(
                            spv::Op::OpSelect, uintTypeId,
                            {IdOp(inFirstPair), IdOp(firstPair), IdOp(secondPair)});
                        const uint32_t bitIndex = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                          {IdOp(site.indexId), IdOp(c31)});
                        const uint32_t shifted = em.Emit(spv::Op::OpShiftRightLogical, uintTypeId,
                                                         {IdOp(word), IdOp(bitIndex)});
                        const uint32_t bit =
                            em.Emit(spv::Op::OpBitwiseAnd, uintTypeId, {IdOp(shifted), IdOp(c1)});
                        finish(em.Emit(spv::Op::OpINotEqual, boolTypeId, {IdOp(bit), IdOp(c0)}));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformBallotBitCount: {
                        const uint32_t word = em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                                      {IdOp(site.valueId),
                                                       {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}});
                        uint32_t masked = 0;
                        if (site.groupOperation == spv::GroupOperation::Reduce) {
                            masked = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                             {IdOp(word), IdOp(existsWord())});
                        } else {
                            const bool inclusive =
                                site.groupOperation == spv::GroupOperation::InclusiveScan;
                            uint32_t laneMask = 0;
                            if (inclusive) {
                                const uint32_t lanePlusOne =
                                    em.Emit(spv::Op::OpIAdd, uintTypeId, {IdOp(lane), IdOp(c1)});
                                const uint32_t atTop =
                                    em.Emit(spv::Op::OpUGreaterThanEqual, boolTypeId,
                                            {IdOp(lanePlusOne), IdOp(c32)});
                                const uint32_t shifted =
                                    em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                            {IdOp(c1), IdOp(lanePlusOne)});
                                const uint32_t below =
                                    em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(shifted), IdOp(c1)});
                                laneMask = em.Emit(spv::Op::OpSelect, uintTypeId,
                                                   {IdOp(atTop), IdOp(cAllOnes), IdOp(below)});
                            } else {
                                const uint32_t bit = em.Emit(spv::Op::OpShiftLeftLogical, uintTypeId,
                                                             {IdOp(c1), IdOp(lane)});
                                laneMask =
                                    em.Emit(spv::Op::OpISub, uintTypeId, {IdOp(bit), IdOp(c1)});
                            }
                            masked = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                             {IdOp(word), IdOp(laneMask)});
                        }
                        finish(em.Emit(spv::Op::OpBitCount, uintTypeId, {IdOp(masked)}));
                        break;
                    }
                    case spv::Op::OpGroupNonUniformBallotFindLSB:
                    case spv::Op::OpGroupNonUniformBallotFindMSB: {
                        const uint32_t word = em.Emit(spv::Op::OpCompositeExtract, uintTypeId,
                                                      {IdOp(site.valueId),
                                                       {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}});
                        const uint32_t masked = em.Emit(spv::Op::OpBitwiseAnd, uintTypeId,
                                                        {IdOp(word), IdOp(existsWord())});
                        const uint32_t instNumber =
                            site.opcode == spv::Op::OpGroupNonUniformBallotFindLSB ? kGlslFindILsb
                                                                                   : kGlslFindUMsb;
                        finish(extInst1(uintTypeId, instNumber, masked));
                        break;
                    }
                    default: {
                        // The arithmetic family. Serial, ascending-lane combines: the same
                        // left-associated order a host-side reference computes, and exact
                        // for every integer-valued input regardless of native topology.
                        ok = storeOwnSlot(toScratch(site.valueId));
                        if (!ok) break;
                        uint32_t result = 0;
                        switch (site.groupOperation) {
                        case spv::GroupOperation::Reduce: {
                            uint32_t acc = readSlot(base);
                            for (uint32_t k = 1; k < kWidth; ++k) {
                                const uint32_t slot = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                              {IdOp(base), IdOp(uintConst(k))});
                                const uint32_t exists = em.Emit(spv::Op::OpULessThan, boolTypeId,
                                                                {IdOp(slot), IdOp(cTotal)});
                                const uint32_t value = readSlot(slot);
                                const uint32_t combined = combine(acc, value);
                                if (combined == 0u) { ok = false; break; }
                                acc = guardedSelect(exists, combined, acc);
                            }
                            result = acc;
                            break;
                        }
                        case spv::GroupOperation::InclusiveScan: {
                            // Slots at or below the own lane always exist, so the guard is
                            // purely the lane comparison.
                            uint32_t acc = readSlot(base);
                            for (uint32_t k = 1; k < kWidth; ++k) {
                                const uint32_t included =
                                    em.Emit(spv::Op::OpULessThanEqual, boolTypeId,
                                            {IdOp(uintConst(k)), IdOp(lane)});
                                const uint32_t slot = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                              {IdOp(base), IdOp(uintConst(k))});
                                const uint32_t value = readSlot(slot);
                                const uint32_t combined = combine(acc, value);
                                if (combined == 0u) { ok = false; break; }
                                acc = guardedSelect(included, combined, acc);
                            }
                            result = acc;
                            break;
                        }
                        case spv::GroupOperation::ExclusiveScan: {
                            uint32_t acc = identity();
                            if (acc == 0u) { ok = false; break; }
                            for (uint32_t k = 0; k < kWidth; ++k) {
                                const uint32_t included = em.Emit(
                                    spv::Op::OpULessThan, boolTypeId, {IdOp(uintConst(k)), IdOp(lane)});
                                const uint32_t slot = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                              {IdOp(base), IdOp(uintConst(k))});
                                const uint32_t value = readSlot(slot);
                                const uint32_t combined = combine(acc, value);
                                if (combined == 0u) { ok = false; break; }
                                acc = guardedSelect(included, combined, acc);
                            }
                            result = acc;
                            break;
                        }
                        case spv::GroupOperation::ClusteredReduce: {
                            const uint32_t cluster =
                                site.clusterSize >= kWidth ? kWidth : site.clusterSize;
                            const uint32_t clusterBase = em.Emit(
                                spv::Op::OpBitwiseAnd, uintTypeId,
                                {IdOp(lane), IdOp(uintConst(~(cluster - 1u) & kLaneMask))});
                            const uint32_t firstSlot = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                               {IdOp(base), IdOp(clusterBase)});
                            uint32_t acc = readSlot(firstSlot);
                            for (uint32_t k = 1; k < cluster; ++k) {
                                const uint32_t slot = em.Emit(spv::Op::OpIAdd, uintTypeId,
                                                              {IdOp(firstSlot), IdOp(uintConst(k))});
                                const uint32_t exists = em.Emit(spv::Op::OpULessThan, boolTypeId,
                                                                {IdOp(slot), IdOp(cTotal)});
                                const uint32_t value = readSlot(slot);
                                const uint32_t combined = combine(acc, value);
                                if (combined == 0u) { ok = false; break; }
                                acc = guardedSelect(exists, combined, acc);
                            }
                            result = acc;
                            break;
                        }
                        default:
                            ok = false;
                            break;
                        }
                        if (!ok) break;
                        barrier();
                        finish(fromScratch(result));
                        break;
                    }
                    }
                    if (!ok) {
                        // Phase A vetted every site, so a build failure here means an
                        // internal inconsistency; the module is already partially rewritten
                        // and must not be used.
                        return Status::Failure;
                    }
                }

                // Widen subgroup-scoped barriers to the workgroup scope the emulation
                // synchronizes at anyway; strictly stronger, so always safe.
                const uint32_t workgroupScopeId =
                    uintConst(static_cast<uint32_t>(spv::Scope::Workgroup));
                if (workgroupScopeId == 0u) return Status::Failure;
                for (const BarrierSite& site : barriers) {
                    if (site.inst->opcode() == spv::Op::OpControlBarrier) {
                        if (site.executionScopeIsSubgroup) {
                            site.inst->SetInOperand(0, {workgroupScopeId});
                        }
                        if (site.memoryScopeIsSubgroup) {
                            site.inst->SetInOperand(1, {workgroupScopeId});
                        }
                    } else if (site.memoryScopeIsSubgroup) {
                        site.inst->SetInOperand(0, {workgroupScopeId});
                    }
                }

                // ---------------------------------------------------------------- Phase C
                // The replaced builtin variables are dead now; remove them together with
                // their decorations and interface entries, then drop the GroupNonUniform*
                // capabilities nothing references any more.
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                for (const BuiltinUse& use : builtinUses) {
                    const uint32_t variableId = use.variable->result_id();
                    for (Instruction& entryPoint : irContext->module()->entry_points()) {
                        for (uint32_t operandIndex = entryPoint.NumInOperands(); operandIndex > 3;) {
                            --operandIndex;
                            if (entryPoint.GetInOperand(operandIndex).type == SPV_OPERAND_TYPE_ID &&
                                entryPoint.GetSingleWordInOperand(operandIndex) == variableId) {
                                entryPoint.RemoveInOperand(operandIndex);
                            }
                        }
                    }
                    for (Instruction* chain : use.accessChains) {
                        irContext->KillNamesAndDecorates(chain);
                        irContext->KillInst(chain);
                    }
                    irContext->KillNamesAndDecorates(use.variable);
                    irContext->KillInst(use.variable);
                }

                static constexpr std::array<spv::Capability, 9> kSubgroupCapabilities = {
                    spv::Capability::GroupNonUniform,
                    spv::Capability::GroupNonUniformVote,
                    spv::Capability::GroupNonUniformArithmetic,
                    spv::Capability::GroupNonUniformBallot,
                    spv::Capability::GroupNonUniformShuffle,
                    spv::Capability::GroupNonUniformShuffleRelative,
                    spv::Capability::GroupNonUniformClustered,
                    spv::Capability::GroupNonUniformQuad,
                    spv::Capability::GroupNonUniformPartitionedNV};
                std::vector<Instruction*> deadCapabilities;
                for (Instruction& capability : irContext->module()->capabilities()) {
                    const auto value =
                        static_cast<spv::Capability>(capability.GetSingleWordInOperand(0));
                    for (const spv::Capability candidate : kSubgroupCapabilities) {
                        if (value == candidate) {
                            deadCapabilities.push_back(&capability);
                            break;
                        }
                    }
                }
                for (Instruction* capability : deadCapabilities) {
                    irContext->KillInst(capability);
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken EmulateSubgroupsPass::CreateEmulateSubgroupsPass(
                const Uint32 maxWorkgroupScratchBytes) {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<EmulateSubgroupsPass>(maxWorkgroupScratchBytes));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
