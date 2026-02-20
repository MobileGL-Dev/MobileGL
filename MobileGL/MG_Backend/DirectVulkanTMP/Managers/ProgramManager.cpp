// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Managers/ProgramManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramManager.h"

#include "MG_Util/Debug/Log.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/optimizer.hpp"
#include "source/opt/constants.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/pass.h"
#include "source/opt/type_manager.h"

#include <bit>

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    namespace {
        using ProgramObject = MG_State::GLState::ProgramObject;
        using ShaderObject = MG_State::GLState::ShaderObject;

        struct PositionTargetInfo {
            uint32_t variableId = 0;
            uint32_t vectorTypeId = 0;
            uint32_t floatTypeId = 0;
            uint32_t vectorPtrTypeId = 0;
            uint32_t memberIndex = 0;
            bool isMember = false;
        };

        bool IsVec4Float32(spvtools::opt::IRContext* context, uint32_t typeId, uint32_t* outFloatTypeId) {
            auto* vecInst = context->get_def_use_mgr()->GetDef(typeId);
            if (!vecInst || vecInst->opcode() != spv::Op::OpTypeVector) return false;
            if (vecInst->GetSingleWordInOperand(1) != 4) return false;

            const uint32_t floatTypeId = vecInst->GetSingleWordInOperand(0);
            auto* floatInst = context->get_def_use_mgr()->GetDef(floatTypeId);
            if (!floatInst || floatInst->opcode() != spv::Op::OpTypeFloat) return false;
            if (floatInst->GetSingleWordInOperand(0) != 32) return false;

            if (outFloatTypeId) *outFloatTypeId = floatTypeId;
            return true;
        }

        bool ResolveDirectPositionTarget(spvtools::opt::IRContext* context, uint32_t variableId,
                                         PositionTargetInfo* outTarget) {
            auto* varInst = context->get_def_use_mgr()->GetDef(variableId);
            if (!varInst || varInst->opcode() != spv::Op::OpVariable) return false;
            if (varInst->GetSingleWordInOperand(0) != static_cast<uint32_t>(spv::StorageClass::Output)) return false;

            auto* ptrTypeInst = context->get_def_use_mgr()->GetDef(varInst->type_id());
            if (!ptrTypeInst || ptrTypeInst->opcode() != spv::Op::OpTypePointer) return false;
            if (ptrTypeInst->GetSingleWordInOperand(0) != static_cast<uint32_t>(spv::StorageClass::Output))
                return false;

            PositionTargetInfo target{};
            target.variableId = variableId;
            target.vectorTypeId = ptrTypeInst->GetSingleWordInOperand(1);
            if (!IsVec4Float32(context, target.vectorTypeId, &target.floatTypeId)) return false;
            target.vectorPtrTypeId = varInst->type_id();
            target.isMember = false;

            *outTarget = target;
            return true;
        }

        uint32_t FindOutputVectorPointerTypeId(spvtools::opt::IRContext* context, uint32_t vectorTypeId) {
            auto* vectorType = context->get_type_mgr()->GetType(vectorTypeId);
            if (!vectorType) return 0;
            spvtools::opt::analysis::Pointer ptrType(vectorType, spv::StorageClass::Output);
            return context->get_type_mgr()->GetTypeInstruction(&ptrType);
        }

        bool ResolveMemberPositionTarget(spvtools::opt::IRContext* context, uint32_t structTypeId, uint32_t memberIndex,
                                         PositionTargetInfo* outTarget) {
            auto* structInst = context->get_def_use_mgr()->GetDef(structTypeId);
            if (!structInst || structInst->opcode() != spv::Op::OpTypeStruct) return false;
            if (memberIndex >= structInst->NumInOperands()) return false;

            const uint32_t vectorTypeId = structInst->GetSingleWordInOperand(memberIndex);
            uint32_t floatTypeId = 0;
            if (!IsVec4Float32(context, vectorTypeId, &floatTypeId)) return false;

            const uint32_t vectorPtrTypeId = FindOutputVectorPointerTypeId(context, vectorTypeId);
            if (vectorPtrTypeId == 0) return false;

            for (auto& inst : context->module()->types_values()) {
                if (inst.opcode() != spv::Op::OpVariable) continue;
                if (inst.GetSingleWordInOperand(0) != static_cast<uint32_t>(spv::StorageClass::Output)) continue;

                auto* ptrTypeInst = context->get_def_use_mgr()->GetDef(inst.type_id());
                if (!ptrTypeInst || ptrTypeInst->opcode() != spv::Op::OpTypePointer) continue;
                if (ptrTypeInst->GetSingleWordInOperand(0) != static_cast<uint32_t>(spv::StorageClass::Output))
                    continue;
                if (ptrTypeInst->GetSingleWordInOperand(1) != structTypeId) continue;

                PositionTargetInfo target{};
                target.variableId = inst.result_id();
                target.vectorTypeId = vectorTypeId;
                target.floatTypeId = floatTypeId;
                target.vectorPtrTypeId = vectorPtrTypeId;
                target.memberIndex = memberIndex;
                target.isMember = true;
                *outTarget = target;
                return true;
            }

            return false;
        }

        bool FindPositionTarget(spvtools::opt::IRContext* context, PositionTargetInfo* outTarget) {
            Vector<Pair<uint32_t, uint32_t>> memberCandidates;
            constexpr uint32_t kDecorationBuiltIn = static_cast<uint32_t>(spv::Decoration::BuiltIn);
            constexpr uint32_t kBuiltInPosition = static_cast<uint32_t>(spv::BuiltIn::Position);

            for (auto& inst : context->module()->annotations()) {
                if (inst.opcode() == spv::Op::OpDecorate) {
                    if (inst.NumInOperands() < 3) continue;
                    if (inst.GetSingleWordInOperand(1) != kDecorationBuiltIn) continue;
                    if (inst.GetSingleWordInOperand(2) != kBuiltInPosition) continue;
                    if (ResolveDirectPositionTarget(context, inst.GetSingleWordInOperand(0), outTarget)) return true;
                } else if (inst.opcode() == spv::Op::OpMemberDecorate) {
                    if (inst.NumInOperands() < 4) continue;
                    if (inst.GetSingleWordInOperand(2) != kDecorationBuiltIn) continue;
                    if (inst.GetSingleWordInOperand(3) != kBuiltInPosition) continue;
                    memberCandidates.emplace_back(inst.GetSingleWordInOperand(0), inst.GetSingleWordInOperand(1));
                }
            }

            for (const auto& [structTypeId, memberIndex] : memberCandidates) {
                if (ResolveMemberPositionTarget(context, structTypeId, memberIndex, outTarget)) return true;
            }
            return false;
        }

        bool InsertPositionFixup(spvtools::opt::IRContext* context, spvtools::opt::Instruction* insertBefore,
                                 const PositionTargetInfo& target, uint32_t halfConstId, bool doYFlip, bool doZRemap) {
            using namespace spvtools::opt;
            InstructionBuilder builder(context, insertBefore,
                                       IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

            uint32_t positionPtrId = target.variableId;
            if (target.isMember) {
                const uint32_t memberIndexId = builder.GetUintConstantId(target.memberIndex);
                if (memberIndexId == 0) return false;
                auto* access = builder.AddAccessChain(target.vectorPtrTypeId, target.variableId, {memberIndexId});
                if (!access) return false;
                positionPtrId = access->result_id();
            }

            auto* position = builder.AddLoad(target.vectorTypeId, positionPtrId);
            if (!position) return false;
            auto* x = builder.AddCompositeExtract(target.floatTypeId, position->result_id(), {0});
            auto* y = builder.AddCompositeExtract(target.floatTypeId, position->result_id(), {1});
            auto* z = builder.AddCompositeExtract(target.floatTypeId, position->result_id(), {2});
            auto* w = builder.AddCompositeExtract(target.floatTypeId, position->result_id(), {3});
            if (!x || !y || !z || !w) return false;

            if (!doYFlip && !doZRemap) return false;

            uint32_t yValueId = y->result_id();
            if (doYFlip) {
                auto* negY = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, y->result_id());
                if (!negY) return false;
                yValueId = negY->result_id();
            }

            uint32_t zValueId = z->result_id();
            if (doZRemap) {
                auto* zPlusW = builder.AddBinaryOp(target.floatTypeId, spv::Op::OpFAdd, z->result_id(), w->result_id());
                if (!zPlusW) return false;
                auto* mappedZ =
                    builder.AddBinaryOp(target.floatTypeId, spv::Op::OpFMul, zPlusW->result_id(), halfConstId);
                if (!mappedZ) return false;
                zValueId = mappedZ->result_id();
            }

            auto* fixedPosition = builder.AddCompositeConstruct(target.vectorTypeId,
                                                                {x->result_id(), yValueId, zValueId, w->result_id()});
            if (!fixedPosition) return false;

            return builder.AddStore(positionPtrId, fixedPosition->result_id()) != nullptr;
        }

        class GlToVulkanPositionFixPass final : public spvtools::opt::Pass {
        public:
            const char* name() const override { return "gl-to-vulkan-position-fix"; }
            explicit GlToVulkanPositionFixPass(ShaderTransformFlags transformFlags)
                : m_transformFlags(transformFlags) {}

            Status Process() override {
                if (!m_transformFlags) return Status::SuccessWithoutChange;
                PositionTargetInfo target{};
                if (!FindPositionTarget(context(), &target)) return Status::SuccessWithoutChange;

                auto* floatType = context()->get_type_mgr()->GetType(target.floatTypeId);
                if (!floatType) return Status::SuccessWithoutChange;

                const uint32_t halfBits = std::bit_cast<uint32_t>(0.5f);
                const auto* halfConst = context()->get_constant_mgr()->GetConstant(floatType, {halfBits});
                auto* halfInst = context()->get_constant_mgr()->GetDefiningInstruction(halfConst);
                if (!halfInst) return Status::SuccessWithoutChange;
                const uint32_t halfConstId = halfInst->result_id();

                const bool doYFlip = (m_transformFlags & ShaderTransformBit::PositionYFlip);
                const bool doZRemap = (m_transformFlags & ShaderTransformBit::PositionZRemap);

                bool modified = false;
                for (auto& entryPoint : get_module()->entry_points()) {
                    if (entryPoint.opcode() != spv::Op::OpEntryPoint) continue;
                    if (entryPoint.NumInOperands() < 2) continue;

                    const auto model = static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0));
                    if (model != spv::ExecutionModel::Vertex && model != spv::ExecutionModel::TessellationEvaluation &&
                        model != spv::ExecutionModel::Geometry) {
                        continue;
                    }

                    auto* function = context()->GetFunction(entryPoint.GetSingleWordInOperand(1));
                    if (!function) continue;

                    for (auto& bb : *function) {
                        for (auto instIter = bb.begin(); instIter != bb.end(); ++instIter) {
                            auto* inst = &*instIter;
                            const bool needsFixup =
                                (model == spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpEmitVertex) ||
                                (model != spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpReturn);
                            if (!needsFixup) continue;

                            modified |= InsertPositionFixup(context(), inst, target, halfConstId, doYFlip, doZRemap);
                        }
                    }
                }

                if (!modified) return Status::SuccessWithoutChange;
                context()->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisDefUse |
                                                       spvtools::opt::IRContext::kAnalysisInstrToBlockMapping);
                return Status::SuccessWithChange;
            }

        private:
            ShaderTransformFlags m_transformFlags;
        };

        spvtools::Optimizer::PassToken CreateGlToVulkanPositionFixPass(ShaderTransformFlags transformFlags) {
            return spvtools::Optimizer::PassToken(MakeUnique<GlToVulkanPositionFixPass>(transformFlags));
        }

        bool TransformSpirvForVulkanPositionFix(const Vector<Uint32>& input, Vector<Uint32>& output,
                                                ShaderTransformFlags transformFlags) {
            if (input.empty()) {
                output.clear();
                return true;
            }

            if (!transformFlags) {
                output = input;
                return true;
            }

            spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
            spvtools::OptimizerOptions options;
            options.set_run_validator(false);
            optimizer.RegisterPass(CreateGlToVulkanPositionFixPass(transformFlags));

            const bool success = optimizer.Run(input.data(), input.size(), &output, options);
            if (!success) {
                MGLOG_E("Vulkan: failed to run GL->Vulkan position fix pass");
                output = input;
            }
            return success;
        }

        ShaderStage PickClipFixupStage(const Vector<SharedPtr<ShaderObject>>& shaders) {
            bool hasGeometry = false;
            bool hasTessEval = false;
            bool hasVertex = false;

            for (const auto& shader : shaders) {
                if (!shader) continue;
                const auto stage = shader->GetShaderStage();
                hasGeometry |= (stage == ShaderStage::Geometry);
                hasTessEval |= (stage == ShaderStage::TessEval);
                hasVertex |= (stage == ShaderStage::Vertex);
            }

            if (hasGeometry) return ShaderStage::Geometry;
            if (hasTessEval) return ShaderStage::TessEval;
            if (hasVertex) return ShaderStage::Vertex;
            return ShaderStage::Unknown;
        }
    } // namespace

    ProgramManager::~ProgramManager() {
        for (auto& [_, stages] : m_cache) {
            DestroyStages(stages);
        }
        m_cache.clear();
    }

    ProgramManager::HashType ProgramManager::ComputeSourceSpvHash(MG_State::GLState::ProgramObject* program) const {
        if (!program) return 0;
        XXH64_state_t* state = XXH64_createState();
        XXH64_reset(state, 0xC0FFEEu);
        auto& spirvs = program->GetGeneratedSpirv();
        for (const auto& spv : spirvs) {
            if (spv.empty()) continue;
            XXH64_update(state, spv.data(), spv.size() * sizeof(Uint));
        }
        HashType hash = XXH64_digest(state);
        XXH64_freeState(state);
        return hash;
    }

    ProgramManager::HashType ProgramManager::ComputeSpvHash(const Vector<Vector<Uint32>>& spirvs) const {
        XXH64_state_t* state = XXH64_createState();
        XXH64_reset(state, 0xC0FFEEu);
        for (const auto& spv : spirvs) {
            if (spv.empty()) continue;
            XXH64_update(state, spv.data(), spv.size() * sizeof(Uint32));
        }
        HashType hash = XXH64_digest(state);
        XXH64_freeState(state);
        return hash;
    }

    void ProgramManager::BuildPipelineSpirvModules(MG_State::GLState::ProgramObject* program,
                                                   Vector<Vector<Uint32>>& outSpirvs,
                                                   ShaderTransformFlags transformFlags) const {
        outSpirvs.clear();
        if (!program) return;

        auto& spirvs = program->GetGeneratedSpirv();
        auto& shaders = program->GetAttachedShaders();
        const ShaderStage fixupStage = PickClipFixupStage(shaders);

        outSpirvs.reserve(spirvs.size());
        for (SizeT i = 0; i < spirvs.size(); ++i) {
            Vector<Uint32> module = spirvs[i];

            ShaderStage stage = ShaderStage::Unknown;
            if (i < shaders.size() && shaders[i]) stage = shaders[i]->GetShaderStage();

            if (!module.empty() && fixupStage != ShaderStage::Unknown && stage == fixupStage) {
                Vector<Uint32> transformed;
                TransformSpirvForVulkanPositionFix(module, transformed, transformFlags);
                module = Move(transformed);
            }
            outSpirvs.push_back(Move(module));
        }
    }

    ProgramManager::HashType ProgramManager::ComputeSpvHash(MG_State::GLState::ProgramObject* program,
                                                            ShaderTransformFlags transformFlags) const {
        Vector<Vector<Uint32>> spirvs;
        BuildPipelineSpirvModules(program, spirvs, transformFlags);
        return ComputeSpvHash(spirvs);
    }

    ProgramManager::HashType ProgramManager::ComputeProgramHash(MG_State::GLState::ProgramObject* program,
                                                                ShaderTransformFlags transformFlags) const {
        if (!program) return 0;
        const HashType sourceHash = ComputeSourceSpvHash(program);
        auto it = m_cache.find(program);
        if (it != m_cache.end() && it->second.sourceHash == sourceHash) return it->second.hash;
        return ComputeSpvHash(program, transformFlags);
    }

    VkShaderStageFlagBits ProgramManager::ToVkStage(ShaderStage stage) const {
        switch (stage) {
        case ShaderStage::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TessControl:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessEval:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        default:
            return VK_SHADER_STAGE_ALL_GRAPHICS;
        }
    }

    void ProgramManager::DestroyStages(ProgramStages& stages) {
        for (auto module : stages.modules) {
            if (module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_ctx.GetDevice(), module, nullptr);
            }
        }
        stages.modules.clear();
        stages.stages.clear();
        stages.sourceHash = 0;
        stages.hash = 0;
    }

    Vector<VkPipelineShaderStageCreateInfo>& ProgramManager::CreatePipelineShaderStages(
        MG_State::GLState::ProgramObject* program, ShaderTransformFlags transformFlags) {
        auto& entry = m_cache[program];
        HashType sourceHash = ComputeSourceSpvHash(program);
        if (!entry.stages.empty() && entry.sourceHash == sourceHash) return entry.stages;

        DestroyStages(entry);
        entry.sourceHash = sourceHash;

        if (!program) return entry.stages;

        Vector<Vector<Uint32>> spirvs;
        BuildPipelineSpirvModules(program, spirvs, transformFlags);
        entry.hash = ComputeSpvHash(spirvs);
        auto& shaders = program->GetAttachedShaders();

        for (SizeT i = 0; i < spirvs.size(); ++i) {
            auto& spv = spirvs[i];
            if (spv.empty()) continue;

            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = spv.size() * sizeof(Uint);
            smci.pCode = spv.data();

            VkShaderModule module = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateShaderModule(m_ctx.GetDevice(), &smci, nullptr, &module), "vkCreateShaderModule");

            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ShaderStage shaderStage = ShaderStage::Unknown;
            if (i < shaders.size() && shaders[i]) shaderStage = shaders[i]->GetShaderStage();
            stage.stage = ToVkStage(shaderStage);
            stage.module = module;
            stage.pName = "main";

            entry.modules.push_back(module);
            entry.stages.push_back(stage);
        }

        return entry.stages;
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
