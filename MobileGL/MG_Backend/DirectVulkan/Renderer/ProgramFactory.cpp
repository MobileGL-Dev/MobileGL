// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramFactory.h"

#include "MG_Backend/DirectVulkan/DirectVulkanResourceState.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"
#include "MG_Util/ShaderTranspiler/SpvcSession.h"
#include "MG_Util/ShaderTranspiler/Types.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <spirv-tools/libspirv.h>
#include <spirv-tools/optimizer.hpp>
#include <source/opt/build_module.h>
#include <source/opt/constants.h>
#include <source/opt/instruction.h>
#include <source/opt/ir_builder.h>
#include <source/opt/ir_context.h>
#include <source/opt/module.h>
#include <source/opt/pass.h>
#include <source/opt/type_manager.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        using ShaderObject = MG_State::GLState::ShaderObject;
        using SpvcSession = MG_Util::ShaderTranspiler::SpvcSession;
        using SessionUsageBit = MG_Util::ShaderTranspiler::SessionUsageBit;

        struct DescriptorKey {
            ProgramFactory::DescriptorBindingKind kind = ProgramFactory::DescriptorBindingKind::None;
            String name;

            Bool operator==(const DescriptorKey& other) const {
                return kind == other.kind && name == other.name;
            }
        };

        struct DescriptorKeyHash {
            SizeT operator()(const DescriptorKey& key) const noexcept {
                return std::hash<String>{}(key.name) ^ (static_cast<SizeT>(key.kind) << 1);
            }
        };

        struct PositionTargetInfo {
            Uint32 variableId = 0;
            Uint32 vectorTypeId = 0;
            Uint32 floatTypeId = 0;
            Uint32 vectorPtrTypeId = 0;
            Uint32 memberIndex = 0;
            Bool isMember = false;
        };

        ShaderStage PickClipFixupStage(const Vector<SharedPtr<ShaderObject>>& shaders);

        Bool IsVec4Float32(spvtools::opt::IRContext* context, Uint32 typeId, Uint32* outFloatTypeId) {
            auto* vecInst = context->get_def_use_mgr()->GetDef(typeId);
            if (!vecInst || vecInst->opcode() != spv::Op::OpTypeVector) return false;
            if (vecInst->GetSingleWordInOperand(1) != 4) return false;

            const Uint32 floatTypeId = vecInst->GetSingleWordInOperand(0);
            auto* floatInst = context->get_def_use_mgr()->GetDef(floatTypeId);
            if (!floatInst || floatInst->opcode() != spv::Op::OpTypeFloat) return false;
            if (floatInst->GetSingleWordInOperand(0) != 32) return false;

            if (outFloatTypeId) *outFloatTypeId = floatTypeId;
            return true;
        }

        spvc_basetype MapReflectInterfaceToSpvcBasetype(const SpvReflectInterfaceVariable& variable) {
            if (variable.type_description == nullptr) {
                return SPVC_BASETYPE_UNKNOWN;
            }

            const auto flags = variable.type_description->type_flags;
            const auto width = variable.numeric.scalar.width;
            const auto signedness = variable.numeric.scalar.signedness;
            if ((flags & SPV_REFLECT_TYPE_FLAG_FLOAT) != 0) {
                switch (width) {
                case 16: return SPVC_BASETYPE_FP16;
                case 32: return SPVC_BASETYPE_FP32;
                case 64: return SPVC_BASETYPE_FP64;
                default: return SPVC_BASETYPE_UNKNOWN;
                }
            }
            if ((flags & SPV_REFLECT_TYPE_FLAG_INT) != 0) {
                if (signedness != 0) {
                    switch (width) {
                    case 8: return SPVC_BASETYPE_INT8;
                    case 16: return SPVC_BASETYPE_INT16;
                    case 32: return SPVC_BASETYPE_INT32;
                    case 64: return SPVC_BASETYPE_INT64;
                    default: return SPVC_BASETYPE_UNKNOWN;
                    }
                }

                switch (width) {
                case 8: return SPVC_BASETYPE_UINT8;
                case 16: return SPVC_BASETYPE_UINT16;
                case 32: return SPVC_BASETYPE_UINT32;
                case 64: return SPVC_BASETYPE_UINT64;
                default: return SPVC_BASETYPE_UNKNOWN;
                }
            }
            if ((flags & SPV_REFLECT_TYPE_FLAG_BOOL) != 0) {
                return SPVC_BASETYPE_BOOLEAN;
            }
            return SPVC_BASETYPE_UNKNOWN;
        }

        Uint32 GetReflectInterfaceLocationSpan(const SpvReflectInterfaceVariable& variable) {
            Uint32 locationSpan = variable.numeric.matrix.column_count;
            if (locationSpan == 0) {
                locationSpan = 1;
            }

            for (Uint32 dimIndex = 0; dimIndex < variable.array.dims_count; ++dimIndex) {
                const Uint32 dim = variable.array.dims[dimIndex];
                if (dim == 0 || dim == SPV_REFLECT_ARRAY_DIM_RUNTIME) {
                    continue;
                }
                locationSpan *= dim;
            }

            return locationSpan;
        }

        GLenum GetReflectInterfaceLocationType(const SpvReflectInterfaceVariable& variable) {
            MG_Util::ShaderTranspiler::SpvcType spvcType{};
            spvcType.basetype = MapReflectInterfaceToSpvcBasetype(variable);
            spvcType.vectorSize = variable.numeric.vector.component_count;
            if (spvcType.vectorSize == 0) {
                spvcType.vectorSize = variable.numeric.matrix.row_count;
            }
            if (spvcType.vectorSize == 0) {
                spvcType.vectorSize = 1;
            }
            spvcType.matCol = 1;

            if (spvcType.vectorSize < 1 || spvcType.vectorSize > 4) {
                return GL_FALSE;
            }

            switch (spvcType.basetype) {
            case SPVC_BASETYPE_BOOLEAN:
                switch (spvcType.vectorSize) {
                case 1: return GL_BOOL;
                case 2: return GL_BOOL_VEC2;
                case 3: return GL_BOOL_VEC3;
                case 4: return GL_BOOL_VEC4;
                default: return GL_FALSE;
                }
            case SPVC_BASETYPE_INT32:
                switch (spvcType.vectorSize) {
                case 1: return GL_INT;
                case 2: return GL_INT_VEC2;
                case 3: return GL_INT_VEC3;
                case 4: return GL_INT_VEC4;
                default: return GL_FALSE;
                }
            case SPVC_BASETYPE_UINT32:
                switch (spvcType.vectorSize) {
                case 1: return GL_UNSIGNED_INT;
                case 2: return GL_UNSIGNED_INT_VEC2;
                case 3: return GL_UNSIGNED_INT_VEC3;
                case 4: return GL_UNSIGNED_INT_VEC4;
                default: return GL_FALSE;
                }
            case SPVC_BASETYPE_FP32:
                switch (spvcType.vectorSize) {
                case 1: return GL_FLOAT;
                case 2: return GL_FLOAT_VEC2;
                case 3: return GL_FLOAT_VEC3;
                case 4: return GL_FLOAT_VEC4;
                default: return GL_FALSE;
                }
            case SPVC_BASETYPE_FP64:
                switch (spvcType.vectorSize) {
                case 1: return GL_DOUBLE;
                case 2: return GL_DOUBLE_VEC2;
                case 3: return GL_DOUBLE_VEC3;
                case 4: return GL_DOUBLE_VEC4;
                default: return GL_FALSE;
                }
            default:
                return GL_FALSE;
            }
        }

        Uint32 GetReflectInterfaceLocationSignature(const SpvReflectInterfaceVariable& variable) {
            Uint32 vectorSize = variable.numeric.vector.component_count;
            if (vectorSize == 0) {
                vectorSize = variable.numeric.matrix.row_count;
            }
            if (vectorSize == 0) {
                vectorSize = 1;
            }
            if (vectorSize < 1 || vectorSize > 4) {
                return 0;
            }

            Uint32 typeClass = 0;
            Uint32 scalarWidth = 0;
            switch (MapReflectInterfaceToSpvcBasetype(variable)) {
            case SPVC_BASETYPE_BOOLEAN:
                typeClass = 1;
                scalarWidth = 1;
                break;
            case SPVC_BASETYPE_INT8:
                typeClass = 2;
                scalarWidth = 8;
                break;
            case SPVC_BASETYPE_INT16:
                typeClass = 2;
                scalarWidth = 16;
                break;
            case SPVC_BASETYPE_INT32:
                typeClass = 2;
                scalarWidth = 32;
                break;
            case SPVC_BASETYPE_INT64:
                typeClass = 2;
                scalarWidth = 64;
                break;
            case SPVC_BASETYPE_UINT8:
                typeClass = 3;
                scalarWidth = 8;
                break;
            case SPVC_BASETYPE_UINT16:
                typeClass = 3;
                scalarWidth = 16;
                break;
            case SPVC_BASETYPE_UINT32:
                typeClass = 3;
                scalarWidth = 32;
                break;
            case SPVC_BASETYPE_UINT64:
                typeClass = 3;
                scalarWidth = 64;
                break;
            case SPVC_BASETYPE_FP16:
                typeClass = 4;
                scalarWidth = 16;
                break;
            case SPVC_BASETYPE_FP32:
                typeClass = 4;
                scalarWidth = 32;
                break;
            case SPVC_BASETYPE_FP64:
                typeClass = 4;
                scalarWidth = 64;
                break;
            default:
                return 0;
            }

            return (typeClass << 24) | (scalarWidth << 8) | vectorSize;
        }

        Uint32 GetReflectInterfaceVectorSize(const SpvReflectInterfaceVariable& variable) {
            Uint32 vectorSize = variable.numeric.vector.component_count;
            if (vectorSize == 0) {
                vectorSize = variable.numeric.matrix.row_count;
            }
            if (vectorSize == 0) {
                vectorSize = 1;
            }
            return vectorSize;
        }

        struct StageInterfaceCursor {
            Uint32 location = 0;
            Uint32 component = 0;
        };

        struct StageInterfaceSummary {
            static constexpr Uint32 kMaxComponentSlots = ProgramFactory::VkProgramObject::kMaxVertexInputLocations * 4;

            Array<Uint32, kMaxComponentSlots> slotSignatures{};
            Array<String, kMaxComponentSlots> slotDebugNames{};
        };

        Uint32 CountOccupiedStageInterfaceSlots(const StageInterfaceSummary& summary) {
            Uint32 occupiedSlotCount = 0;
            for (Uint32 slotIndex = 0; slotIndex < StageInterfaceSummary::kMaxComponentSlots; ++slotIndex) {
                if (summary.slotSignatures[slotIndex] != 0) {
                    ++occupiedSlotCount;
                }
            }
            return occupiedSlotCount;
        }

        spv_target_env GetSpirvTargetEnv(const Vector<Uint>& spirv) {
            spv_target_env targetEnv = SPV_ENV_VULKAN_1_0;
            if (spirv.size() > 1) {
                const Uint32 versionWord = spirv[1];
                const Uint32 major = (versionWord >> 16) & 0xffu;
                const Uint32 minor = (versionWord >> 8) & 0xffu;
                if (major > 1 || (major == 1 && minor >= 6)) {
                    targetEnv = SPV_ENV_VULKAN_1_3;
                } else if (major == 1 && minor >= 5) {
                    targetEnv = SPV_ENV_VULKAN_1_2;
                } else if (major == 1 && minor >= 4) {
                    targetEnv = SPV_ENV_VULKAN_1_1_SPIRV_1_4;
                } else if (major == 1 && minor >= 3) {
                    targetEnv = SPV_ENV_VULKAN_1_1;
                }
            }
            return targetEnv;
        }

        Bool IsInterfaceVariableStaticallyUsed(const Vector<Uint>& spirv, Uint32 spirvId) {
            if (spirv.empty() || spirvId == 0) {
                return false;
            }

            auto context = spvtools::BuildModule(
                GetSpirvTargetEnv(spirv),
                [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                spirv.data(),
                spirv.size());
            if (!context) {
                return true;
            }

            auto* variable = context->get_def_use_mgr()->GetDef(spirvId);
            if (variable == nullptr) {
                return false;
            }

            Bool used = false;
            context->get_def_use_mgr()->ForEachUser(variable, [&used](spvtools::opt::Instruction* user) {
                switch (user->opcode()) {
                case spv::Op::OpName:
                case spv::Op::OpMemberName:
                case spv::Op::OpDecorate:
                case spv::Op::OpMemberDecorate:
                case spv::Op::OpDecorateId:
                case spv::Op::OpEntryPoint:
                    return;
                default:
                    used = true;
                    return;
                }
            });
            return used;
        }

        void ValidateTransformedSpirv(const Vector<Uint>& spirv, ShaderStage shaderStage, Uint programExternalIndex) {
            if (spirv.empty()) {
                return;
            }

            spv_const_binary_t binary = {spirv.data(), spirv.size()};
            const spv_target_env targetEnv = GetSpirvTargetEnv(spirv);

            spv_context context = spvContextCreate(targetEnv);
            MOBILEGL_ASSERT(context != nullptr,
                            "ProgramFactory::ValidateTransformedSpirv: failed to create validator context for stage=%d program=%u",
                            static_cast<Int>(shaderStage),
                            programExternalIndex);

            spv_validator_options options = spvValidatorOptionsCreate();
            MOBILEGL_ASSERT(options != nullptr,
                            "ProgramFactory::ValidateTransformedSpirv: failed to create validator options for stage=%d program=%u",
                            static_cast<Int>(shaderStage),
                            programExternalIndex);
            spvValidatorOptionsSetFriendlyNames(options, true);

            spv_diagnostic diagnostic = nullptr;
            const spv_result_t result = spvValidateWithOptions(context, options, &binary, &diagnostic);
            if (result != SPV_SUCCESS) {
                // MGLOG_I, not E: at the INFO compile level of the CI/test lanes that arm
                // the validation switch, MGLOG_E is compiled out (Log.h orders
                // DEBUG < WARN < ERROR < INFO) and the VUID would never reach a log. The
                // latch is what a test harness asserts on.
                MG_Util::ShaderTranspiler::ShaderCompiler::NoteSpirvValidationFailure();
                MGLOG_I(
                    "ProgramFactory::ValidateTransformedSpirv: validation failed for stage=%d program=%u result=%d index=%zu msg=%s",
                    static_cast<Int>(shaderStage),
                    programExternalIndex,
                    static_cast<Int>(result),
                    diagnostic != nullptr ? diagnostic->position.index : 0,
                    diagnostic != nullptr && diagnostic->error != nullptr ? diagnostic->error : "<null>");
            }
            MOBILEGL_ASSERT(
                result == SPV_SUCCESS,
                "ProgramFactory::ValidateTransformedSpirv: validation failed for stage=%d program=%u result=%d line=%zu column=%zu index=%zu msg=%s",
                static_cast<Int>(shaderStage),
                programExternalIndex,
                static_cast<Int>(result),
                diagnostic != nullptr ? diagnostic->position.line : 0,
                diagnostic != nullptr ? diagnostic->position.column : 0,
                diagnostic != nullptr ? diagnostic->position.index : 0,
                diagnostic != nullptr && diagnostic->error != nullptr ? diagnostic->error : "<null>");

            spvDiagnosticDestroy(diagnostic);
            spvValidatorOptionsDestroy(options);
            spvContextDestroy(context);
        }

        void ReflectStageInterfaceVariable(const SpvReflectInterfaceVariable& variable,
                                           Bool reflectInputs,
                                           StageInterfaceSummary& outSummary,
                                           Uint programExternalIndex,
                                           const char* stageLabel,
                                           StageInterfaceCursor& cursor,
                                           Uint32 locationBase = 0,
                                           Bool allowImplicitPacking = false,
                                           const char* inheritedName = nullptr) {
            if ((variable.decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                return;
            }

            const char* debugName = variable.name;
            if (debugName == nullptr || debugName[0] == '\0') {
                debugName = inheritedName;
            }
            if (debugName == nullptr || debugName[0] == '\0') {
                debugName = "<null>";
            }

            const Bool hasConcreteLocation =
                allowImplicitPacking ? (variable.location != 0 || variable.component != 0)
                                     : variable.location != std::numeric_limits<Uint32>::max();
            const Bool hasConcreteComponent =
                allowImplicitPacking ? (variable.component != 0)
                                     : variable.component != std::numeric_limits<Uint32>::max();
            const Uint32 explicitLocationBase = locationBase + (hasConcreteLocation ? variable.location : 0u);

            if (variable.member_count > 0 && variable.members != nullptr) {
                StageInterfaceCursor memberCursor = cursor;
                if (hasConcreteLocation) {
                    memberCursor.location = explicitLocationBase;
                    memberCursor.component = 0;
                }
                for (Uint32 memberIndex = 0; memberIndex < variable.member_count; ++memberIndex) {
                    ReflectStageInterfaceVariable(variable.members[memberIndex], reflectInputs, outSummary,
                                                  programExternalIndex, stageLabel, memberCursor,
                                                  explicitLocationBase, true, debugName);
                }
                if (memberCursor.location > cursor.location ||
                    (memberCursor.location == cursor.location && memberCursor.component > cursor.component)) {
                    cursor = memberCursor;
                }
                return;
            }

            MOBILEGL_ASSERT(
                hasConcreteLocation || allowImplicitPacking || locationBase != 0,
                "ProgramFactory::ReflectStageInterface: missing concrete %s %s location for name='%s' program=%u",
                stageLabel,
                reflectInputs ? "input" : "output",
                debugName,
                programExternalIndex);

            const Uint32 component = variable.component;
            MOBILEGL_ASSERT(
                component < 4 || component == std::numeric_limits<Uint32>::max(),
                "ProgramFactory::ReflectStageInterface: unsupported %s %s component=%u at location=%u name='%s' program=%u",
                stageLabel,
                reflectInputs ? "input" : "output",
                component,
                explicitLocationBase,
                debugName,
                programExternalIndex);

            const Uint32 locationSignature = GetReflectInterfaceLocationSignature(variable);
            MOBILEGL_ASSERT(
                locationSignature != 0,
                "ProgramFactory::ReflectStageInterface: unsupported %s %s type at location=%u name='%s' flags=0x%x width=%u signed=%u vec=%u rows=%u cols=%u program=%u",
                stageLabel,
                reflectInputs ? "input" : "output",
                explicitLocationBase,
                debugName,
                static_cast<Uint32>(variable.type_description != nullptr ? variable.type_description->type_flags : 0),
                variable.numeric.scalar.width,
                variable.numeric.scalar.signedness,
                variable.numeric.vector.component_count,
                variable.numeric.matrix.row_count,
                variable.numeric.matrix.column_count,
                programExternalIndex);

            const Uint32 vectorSize = GetReflectInterfaceVectorSize(variable);
            const Uint32 locationSpan = GetReflectInterfaceLocationSpan(variable);
            Uint32 startLocation = explicitLocationBase;
            Uint32 startComponent = hasConcreteComponent ? component : 0u;
            const Bool useImplicitPacking = allowImplicitPacking && !hasConcreteLocation && !hasConcreteComponent;
            if (useImplicitPacking) {
                startLocation = cursor.location;
                startComponent = cursor.component;
                if (locationSpan > 1 || startComponent + vectorSize > 4) {
                    if (startComponent != 0) {
                        ++startLocation;
                        startComponent = 0;
                    }
                    if (locationSpan == 1 && startComponent + vectorSize > 4) {
                        ++startLocation;
                        startComponent = 0;
                    }
                }
            }

            MOBILEGL_ASSERT(
                startComponent < 4,
                "ProgramFactory::ReflectStageInterface: %s %s component overflow at location=%u component=%u name='%s' program=%u",
                stageLabel,
                reflectInputs ? "input" : "output",
                startLocation,
                startComponent,
                debugName,
                programExternalIndex);
            MOBILEGL_ASSERT(
                locationSpan == 1 || startComponent == 0,
                "ProgramFactory::ReflectStageInterface: %s %s multi-location variable starts at non-zero component location=%u component=%u name='%s' program=%u",
                stageLabel,
                reflectInputs ? "input" : "output",
                startLocation,
                startComponent,
                debugName,
                programExternalIndex);

            for (Uint32 locationOffset = 0; locationOffset < locationSpan; ++locationOffset) {
                const Uint32 expandedLocation = startLocation + locationOffset;
                const Uint32 componentBase = (locationOffset == 0) ? startComponent : 0u;
                MOBILEGL_ASSERT(
                    expandedLocation < ProgramFactory::VkProgramObject::kMaxVertexInputLocations,
                    "ProgramFactory::ReflectStageInterface: %s %s location=%u span=%u exceeds tracked limit for name='%s' program=%u",
                    stageLabel,
                    reflectInputs ? "input" : "output",
                    startLocation,
                    locationSpan,
                    debugName,
                    programExternalIndex);
                MOBILEGL_ASSERT(
                    componentBase + vectorSize <= 4,
                    "ProgramFactory::ReflectStageInterface: %s %s component span overflow at location=%u component=%u vec=%u name='%s' program=%u",
                    stageLabel,
                    reflectInputs ? "input" : "output",
                    expandedLocation,
                    componentBase,
                    vectorSize,
                    debugName,
                    programExternalIndex);
                for (Uint32 componentOffset = 0; componentOffset < vectorSize; ++componentOffset) {
                    const Uint32 expandedComponent = componentBase + componentOffset;
                    const Uint32 slotIndex = expandedLocation * 4 + expandedComponent;
                    MOBILEGL_ASSERT(
                        outSummary.slotSignatures[slotIndex] == 0 || outSummary.slotSignatures[slotIndex] == locationSignature,
                        "ProgramFactory::ReflectStageInterface: conflicting %s %s type at location=%u component=%u existingSignature=0x%x existingName='%s' newSignature=0x%x newName='%s' program=%u",
                        stageLabel,
                        reflectInputs ? "input" : "output",
                        expandedLocation,
                        expandedComponent,
                        outSummary.slotSignatures[slotIndex],
                        outSummary.slotDebugNames[slotIndex].empty() ? "<null>" : outSummary.slotDebugNames[slotIndex].c_str(),
                        locationSignature,
                        debugName,
                        programExternalIndex);
                    outSummary.slotSignatures[slotIndex] = locationSignature;
                    outSummary.slotDebugNames[slotIndex] = debugName;
                }
            }

            StageInterfaceCursor endCursor{};
            if (locationSpan > 1) {
                endCursor.location = startLocation + locationSpan;
                endCursor.component = 0;
            } else {
                endCursor.location = startLocation;
                endCursor.component = startComponent + vectorSize;
                if (endCursor.component >= 4) {
                    endCursor.location += endCursor.component / 4;
                    endCursor.component %= 4;
                }
            }
            if (endCursor.location > cursor.location ||
                (endCursor.location == cursor.location && endCursor.component > cursor.component)) {
                cursor = endCursor;
            }
        }

        void ReflectStageInterface(ShaderStage targetStage,
                                   Bool reflectInputs,
                                   const Vector<SharedPtr<ShaderObject>>& shaders,
                                   const Vector<Vector<Uint>>& spirv,
                                   StageInterfaceSummary& outSummary,
                                   Uint programExternalIndex,
                                   const char* stageLabel) {
            outSummary.slotSignatures.fill(0);

            for (SizeT moduleIndex = 0; moduleIndex < shaders.size() && moduleIndex < spirv.size(); ++moduleIndex) {
                if (!shaders[moduleIndex] || shaders[moduleIndex]->GetShaderStage() != targetStage) {
                    continue;
                }

                const auto& module = spirv[moduleIndex];
                if (module.empty()) {
                    continue;
                }

                SpvReflectShaderModule reflectModule{};
                const SpvReflectResult createResult =
                    spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
                MOBILEGL_ASSERT(
                    createResult == SPV_REFLECT_RESULT_SUCCESS,
                    "ProgramFactory::ReflectStageInterface: failed to create reflection module for %s %s (result=%d program=%u)",
                    stageLabel,
                    reflectInputs ? "input" : "output",
                    static_cast<Int>(createResult),
                    programExternalIndex);
                if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
                    continue;
                }

                uint32_t variableCount = 0;
                SpvReflectResult reflectResult = reflectInputs
                    ? spvReflectEnumerateInputVariables(&reflectModule, &variableCount, nullptr)
                    : spvReflectEnumerateOutputVariables(&reflectModule, &variableCount, nullptr);
                MOBILEGL_ASSERT(
                    reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                    "ProgramFactory::ReflectStageInterface: failed to enumerate %s %s variables (result=%d program=%u)",
                    stageLabel,
                    reflectInputs ? "input" : "output",
                    static_cast<Int>(reflectResult),
                    programExternalIndex);

                Vector<SpvReflectInterfaceVariable*> variables(variableCount);
                if (reflectResult == SPV_REFLECT_RESULT_SUCCESS && variableCount > 0) {
                    reflectResult = reflectInputs
                        ? spvReflectEnumerateInputVariables(&reflectModule, &variableCount, variables.data())
                        : spvReflectEnumerateOutputVariables(&reflectModule, &variableCount, variables.data());
                    MOBILEGL_ASSERT(
                        reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                        "ProgramFactory::ReflectStageInterface: failed to fetch %s %s variables (result=%d program=%u)",
                        stageLabel,
                        reflectInputs ? "input" : "output",
                        static_cast<Int>(reflectResult),
                        programExternalIndex);
                }

                if (reflectResult == SPV_REFLECT_RESULT_SUCCESS) {
                    StageInterfaceCursor stageCursor{};
                    for (auto* variable : variables) {
                        if (variable == nullptr) {
                            continue;
                        }
                        if (reflectInputs && !IsInterfaceVariableStaticallyUsed(module, variable->spirv_id)) {
                            continue;
                        }
                        ReflectStageInterfaceVariable(*variable, reflectInputs, outSummary, programExternalIndex,
                                                      stageLabel, stageCursor);
                    }
                }

                spvReflectDestroyShaderModule(&reflectModule);
                break;
            }
        }

        void ValidateRasterizationStageInterface(const Vector<SharedPtr<ShaderObject>>& shaders,
                                                 const Vector<Vector<Uint>>& spirv,
                                                 ProgramFactory::VkProgramObject& entry,
                                                 Uint programExternalIndex) {
            const ShaderStage producerStage = PickClipFixupStage(shaders);
            entry.rasterizationProducerStage = producerStage;
            entry.producerOutputComponentCount = 0;
            entry.fragmentInputComponentCount = 0;
            if (producerStage == ShaderStage::Unknown) {
                return;
            }

            Bool hasFragmentStage = false;
            for (const auto& shader : shaders) {
                if (shader && shader->GetShaderStage() == ShaderStage::Fragment) {
                    hasFragmentStage = true;
                    break;
                }
            }
            if (!hasFragmentStage) {
                return;
            }

            StageInterfaceSummary producerOutputs{};
            StageInterfaceSummary fragmentInputs{};
            ReflectStageInterface(producerStage, false, shaders, spirv, producerOutputs, programExternalIndex,
                                  "producer");
            ReflectStageInterface(ShaderStage::Fragment, true, shaders, spirv, fragmentInputs, programExternalIndex,
                                  "fragment");
            entry.producerOutputComponentCount = CountOccupiedStageInterfaceSlots(producerOutputs);
            entry.fragmentInputComponentCount = CountOccupiedStageInterfaceSlots(fragmentInputs);

            for (Uint32 slotIndex = 0; slotIndex < StageInterfaceSummary::kMaxComponentSlots; ++slotIndex) {
                if (fragmentInputs.slotSignatures[slotIndex] == 0) {
                    continue;
                }

                MOBILEGL_ASSERT(
                    producerOutputs.slotSignatures[slotIndex] == fragmentInputs.slotSignatures[slotIndex],
                    "ProgramFactory::ValidateRasterizationStageInterface: location=%u component=%u producerSignature=0x%x producerName='%s' fragmentSignature=0x%x fragmentName='%s' program=%u",
                    slotIndex / 4,
                    slotIndex % 4,
                    producerOutputs.slotSignatures[slotIndex],
                    producerOutputs.slotDebugNames[slotIndex].empty() ? "<null>" : producerOutputs.slotDebugNames[slotIndex].c_str(),
                    fragmentInputs.slotSignatures[slotIndex],
                    fragmentInputs.slotDebugNames[slotIndex].empty() ? "<null>" : fragmentInputs.slotDebugNames[slotIndex].c_str(),
                    programExternalIndex);
            }
        }

        Bool ResolveDirectPositionTarget(spvtools::opt::IRContext* context, Uint32 variableId,
                                         PositionTargetInfo* outTarget) {
            auto* varInst = context->get_def_use_mgr()->GetDef(variableId);
            if (!varInst || varInst->opcode() != spv::Op::OpVariable) return false;
            if (varInst->GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) return false;

            auto* ptrTypeInst = context->get_def_use_mgr()->GetDef(varInst->type_id());
            if (!ptrTypeInst || ptrTypeInst->opcode() != spv::Op::OpTypePointer) return false;
            if (ptrTypeInst->GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) return false;

            PositionTargetInfo target{};
            target.variableId = variableId;
            target.vectorTypeId = ptrTypeInst->GetSingleWordInOperand(1);
            if (!IsVec4Float32(context, target.vectorTypeId, &target.floatTypeId)) return false;
            target.vectorPtrTypeId = varInst->type_id();
            target.isMember = false;

            *outTarget = target;
            return true;
        }

        Uint32 FindOutputVectorPointerTypeId(spvtools::opt::IRContext* context, Uint32 vectorTypeId) {
            auto* vectorType = context->get_type_mgr()->GetType(vectorTypeId);
            if (!vectorType) return 0;
            spvtools::opt::analysis::Pointer ptrType(vectorType, spv::StorageClass::Output);
            return context->get_type_mgr()->GetTypeInstruction(&ptrType);
        }

        Bool ResolveMemberPositionTarget(spvtools::opt::IRContext* context, Uint32 structTypeId, Uint32 memberIndex,
                                         PositionTargetInfo* outTarget) {
            auto* structInst = context->get_def_use_mgr()->GetDef(structTypeId);
            if (!structInst || structInst->opcode() != spv::Op::OpTypeStruct) return false;
            if (memberIndex >= structInst->NumInOperands()) return false;

            const Uint32 vectorTypeId = structInst->GetSingleWordInOperand(memberIndex);
            Uint32 floatTypeId = 0;
            if (!IsVec4Float32(context, vectorTypeId, &floatTypeId)) return false;

            const Uint32 vectorPtrTypeId = FindOutputVectorPointerTypeId(context, vectorTypeId);
            if (vectorPtrTypeId == 0) return false;

            for (auto& inst : context->module()->types_values()) {
                if (inst.opcode() != spv::Op::OpVariable) continue;
                if (inst.GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) continue;

                auto* ptrTypeInst = context->get_def_use_mgr()->GetDef(inst.type_id());
                if (!ptrTypeInst || ptrTypeInst->opcode() != spv::Op::OpTypePointer) continue;
                if (ptrTypeInst->GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) continue;
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

        Bool FindPositionTarget(spvtools::opt::IRContext* context, PositionTargetInfo* outTarget) {
            Vector<Pair<Uint32, Uint32>> memberCandidates;
            constexpr auto kDecorationBuiltIn = static_cast<Uint32>(spv::Decoration::BuiltIn);
            constexpr auto kBuiltInPosition = static_cast<Uint32>(spv::BuiltIn::Position);

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

        Bool InsertPositionFixup(spvtools::opt::IRContext* context, spvtools::opt::Instruction* insertBefore,
                                 const PositionTargetInfo& target, Uint32 halfConstId, Bool doYFlip, Bool doZRemap,
                                 Bool doSurfaceRotate90, Bool doSurfaceRotate180, Bool doSurfaceRotate270) {
            using namespace spvtools::opt;
            InstructionBuilder builder(context, insertBefore,
                                       IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

            Uint32 positionPtrId = target.variableId;
            if (target.isMember) {
                const Uint32 memberIndexId = builder.GetUintConstantId(target.memberIndex);
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

            if (!doYFlip && !doZRemap && !doSurfaceRotate90 && !doSurfaceRotate180 && !doSurfaceRotate270) {
                return false;
            }

            Uint32 xValueId = x->result_id();
            Uint32 yValueId = y->result_id();
            if (doYFlip) {
                auto* negY = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, y->result_id());
                if (!negY) return false;
                yValueId = negY->result_id();
            }

            if (doSurfaceRotate90) {
                auto* negY = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, yValueId);
                if (!negY) return false;
                xValueId = negY->result_id();
                yValueId = x->result_id();
            } else if (doSurfaceRotate180) {
                auto* negX = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, xValueId);
                auto* negY = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, yValueId);
                if (!negX || !negY) return false;
                xValueId = negX->result_id();
                yValueId = negY->result_id();
            } else if (doSurfaceRotate270) {
                auto* negX = builder.AddUnaryOp(target.floatTypeId, spv::Op::OpFNegate, xValueId);
                if (!negX) return false;
                xValueId = yValueId;
                yValueId = negX->result_id();
            }

            Uint32 zValueId = z->result_id();
            if (doZRemap) {
                auto* zPlusW = builder.AddBinaryOp(target.floatTypeId, spv::Op::OpFAdd, z->result_id(), w->result_id());
                if (!zPlusW) return false;
                auto* mappedZ =
                    builder.AddBinaryOp(target.floatTypeId, spv::Op::OpFMul, zPlusW->result_id(), halfConstId);
                if (!mappedZ) return false;
                zValueId = mappedZ->result_id();
            }

            auto* fixedPosition = builder.AddCompositeConstruct(target.vectorTypeId,
                                                                {xValueId, yValueId, zValueId, w->result_id()});
            if (!fixedPosition) return false;

            return builder.AddStore(positionPtrId, fixedPosition->result_id()) != nullptr;
        }

        class GlToVulkanPositionFixPass final : public spvtools::opt::Pass {
        public:
            const char* name() const override { return "gl-to-vulkan-position-fix"; }
            explicit GlToVulkanPositionFixPass(ProgramFactory::CompileOptionFlags transformFlags)
                : m_transformFlags(transformFlags) {}

            Status Process() override {
                if (!m_transformFlags) return Status::SuccessWithoutChange;
                PositionTargetInfo target{};
                if (!FindPositionTarget(context(), &target)) return Status::SuccessWithoutChange;

                auto* floatType = context()->get_type_mgr()->GetType(target.floatTypeId);
                if (!floatType) return Status::SuccessWithoutChange;

                const auto halfBits = std::bit_cast<Uint32>(0.5f);
                const auto* halfConst = context()->get_constant_mgr()->GetConstant(floatType, {halfBits});
                auto* halfInst = context()->get_constant_mgr()->GetDefiningInstruction(halfConst);
                if (!halfInst) return Status::SuccessWithoutChange;
                const Uint32 halfConstId = halfInst->result_id();

                const Bool doYFlip = (m_transformFlags & ProgramFactory::CompileOptionBit::PositionYFlip);
                const Bool doZRemap = (m_transformFlags & ProgramFactory::CompileOptionBit::PositionZRemap);
                const Bool doSurfaceRotate90 = (m_transformFlags & ProgramFactory::CompileOptionBit::SurfaceRotate90);
                const Bool doSurfaceRotate180 =
                    (m_transformFlags & ProgramFactory::CompileOptionBit::SurfaceRotate180);
                const Bool doSurfaceRotate270 =
                    (m_transformFlags & ProgramFactory::CompileOptionBit::SurfaceRotate270);

                Bool modified = false;
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
                            const Bool needsFixup =
                                (model == spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpEmitVertex) ||
                                (model != spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpReturn);
                            if (!needsFixup) continue;

                            modified |= InsertPositionFixup(context(), inst, target, halfConstId, doYFlip, doZRemap,
                                                             doSurfaceRotate90, doSurfaceRotate180, doSurfaceRotate270);
                        }
                    }
                }

                if (!modified) return Status::SuccessWithoutChange;
                context()->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisDefUse |
                                                       spvtools::opt::IRContext::kAnalysisInstrToBlockMapping);
                return Status::SuccessWithChange;
            }

        private:
            ProgramFactory::CompileOptionFlags m_transformFlags;
        };

        // gl_FragCoord back into GL's window space, for default-framebuffer draws only.
        //
        // Vulkan's gl_FragCoord.y is the framebuffer ROW being written - not a value the
        // viewport rect can move independently of placement. The default framebuffer's image is
        // stored display-side-up and the vertex stage compensates by negating gl_Position.y, so
        // for every default-FBO draw the framebuffer row of a fragment is exactly
        // `height - y_GL` (the viewport terms cancel: yf_VK = H - yf_GL for any viewport rect).
        // A shader that reads gl_FragCoord therefore sees a flipped Y, and once the viewport
        // rect started being converted to the stored orientation it also sees a Y that is
        // OUTSIDE the range GL promises - a 32-pixel-tall viewport at GL y=0 reports 224..255 on
        // a 256-tall surface. GL CTS shader_image_load_store writes imageStore(image,
        // ivec2(gl_FragCoord.xy)) into an image exactly the size of that viewport, so every
        // store fell outside the image and the test read back zeroes.
        //
        // The rewrite redirects every read of the builtin to a Private copy initialised once at
        // entry, which is exact for all access forms (whole-vector loads, `.y` access chains,
        // OpCopyMemory) and leaves the builtin itself - and its decorations - untouched.
        class GlFragCoordYFlipPass final : public spvtools::opt::Pass {
        public:
            const char* name() const override { return "mobilegl-fragcoord-y-flip"; }
            explicit GlFragCoordYFlipPass(Uint32 framebufferHeight) : m_framebufferHeight(framebufferHeight) {}

            Status Process() override {
                using namespace spvtools::opt;
                if (m_framebufferHeight == 0) return Status::SuccessWithoutChange;

                Instruction* entryPoint = nullptr;
                for (auto& candidate : get_module()->entry_points()) {
                    if (candidate.NumInOperands() >= 2 &&
                        static_cast<spv::ExecutionModel>(candidate.GetSingleWordInOperand(0)) ==
                            spv::ExecutionModel::Fragment) {
                        entryPoint = &candidate;
                        break;
                    }
                }
                if (!entryPoint) return Status::SuccessWithoutChange;

                const Uint32 builtinVarId = FindFragCoordVariable();
                if (builtinVarId == 0) return Status::SuccessWithoutChange;

                Instruction* builtinVar = context()->get_def_use_mgr()->GetDef(builtinVarId);
                if (!builtinVar || builtinVar->opcode() != spv::Op::OpVariable) return Status::SuccessWithoutChange;

                // The builtin is `Input vec4`; take the vector and component types from its own
                // pointer type rather than assuming float32x4, so a module that spells it
                // differently declines instead of miscompiling.
                Instruction* inputPtrType = context()->get_def_use_mgr()->GetDef(builtinVar->type_id());
                if (!inputPtrType || inputPtrType->opcode() != spv::Op::OpTypePointer) {
                    return Status::SuccessWithoutChange;
                }
                const Uint32 vectorTypeId = inputPtrType->GetSingleWordInOperand(1);
                Instruction* vectorType = context()->get_def_use_mgr()->GetDef(vectorTypeId);
                if (!vectorType || vectorType->opcode() != spv::Op::OpTypeVector ||
                    vectorType->GetSingleWordInOperand(1) != 4) {
                    return Status::SuccessWithoutChange;
                }
                const Uint32 floatTypeId = vectorType->GetSingleWordInOperand(0);
                auto* floatType = context()->get_type_mgr()->GetType(floatTypeId);
                if (!floatType || !floatType->AsFloat() || floatType->AsFloat()->width() != 32) {
                    return Status::SuccessWithoutChange;
                }

                const auto heightBits = std::bit_cast<Uint32>(static_cast<float>(m_framebufferHeight));
                const auto* heightConst = context()->get_constant_mgr()->GetConstant(floatType, {heightBits});
                auto* heightInst = context()->get_constant_mgr()->GetDefiningInstruction(heightConst);
                if (!heightInst) return Status::SuccessWithoutChange;

                auto* function = context()->GetFunction(entryPoint->GetSingleWordInOperand(1));
                if (!function || function->begin() == function->end()) return Status::SuccessWithoutChange;

                const Uint32 privatePtrTypeId =
                    context()->get_type_mgr()->FindPointerToType(vectorTypeId, spv::StorageClass::Private);
                if (privatePtrTypeId == 0) return Status::SuccessWithoutChange;

                const Uint32 copyVarId = context()->TakeNextId();
                if (copyVarId == 0) return Status::SuccessWithoutChange;
                auto copyVar = std::make_unique<Instruction>(
                    context(), spv::Op::OpVariable, privatePtrTypeId, copyVarId,
                    std::initializer_list<Operand>{
                        {SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<Uint32>(spv::StorageClass::Private)}}});
                context()->AddGlobalValue(std::move(copyVar));

                // Redirect the reads BEFORE emitting the initialiser, so the initialiser's own
                // load of the builtin is not rewritten into a load of the (still empty) copy.
                if (!RedirectReads(builtinVarId, copyVarId)) return Status::SuccessWithoutChange;

                auto& entryBlock = *function->begin();
                auto insertPoint = entryBlock.begin();
                while (insertPoint != entryBlock.end() && insertPoint->opcode() == spv::Op::OpVariable) {
                    ++insertPoint;
                }
                if (insertPoint == entryBlock.end()) return Status::SuccessWithoutChange;

                InstructionBuilder builder(context(), &*insertPoint,
                                           IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                auto* raw = builder.AddLoad(vectorTypeId, builtinVarId);
                if (!raw) return Status::SuccessWithoutChange;
                auto* x = builder.AddCompositeExtract(floatTypeId, raw->result_id(), {0});
                auto* y = builder.AddCompositeExtract(floatTypeId, raw->result_id(), {1});
                auto* z = builder.AddCompositeExtract(floatTypeId, raw->result_id(), {2});
                auto* w = builder.AddCompositeExtract(floatTypeId, raw->result_id(), {3});
                if (!x || !y || !z || !w) return Status::SuccessWithoutChange;
                auto* flippedY =
                    builder.AddBinaryOp(floatTypeId, spv::Op::OpFSub, heightInst->result_id(), y->result_id());
                if (!flippedY) return Status::SuccessWithoutChange;
                auto* corrected = builder.AddCompositeConstruct(
                    vectorTypeId, {x->result_id(), flippedY->result_id(), z->result_id(), w->result_id()});
                if (!corrected) return Status::SuccessWithoutChange;
                if (!builder.AddStore(copyVarId, corrected->result_id())) return Status::SuccessWithoutChange;

                // SPIR-V 1.4 widened the entry-point interface to every global the entry point
                // statically uses, Private included; earlier versions accept Input/Output only,
                // so listing it there would be invalid.
                if (get_module()->version() >= 0x00010400u) {
                    entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {copyVarId}});
                    context()->AnalyzeUses(entryPoint);
                }

                context()->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisDefUse |
                                                       spvtools::opt::IRContext::kAnalysisInstrToBlockMapping);
                return Status::SuccessWithChange;
            }

        private:
            Uint32 FindFragCoordVariable() const {
                for (const auto& annotation : get_module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate) continue;
                    if (annotation.NumInOperands() < 3) continue;
                    if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                        spv::Decoration::BuiltIn) {
                        continue;
                    }
                    if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != spv::BuiltIn::FragCoord) {
                        continue;
                    }
                    return annotation.GetSingleWordInOperand(0);
                }
                return 0;
            }

            // Every instruction that reads through the builtin's POINTER gets the copy instead.
            // Decorations, names and the entry-point interface keep naming the builtin.
            Bool RedirectReads(Uint32 builtinVarId, Uint32 copyVarId) {
                using namespace spvtools::opt;
                Bool ok = true;
                Vector<Instruction*> users;
                context()->get_def_use_mgr()->ForEachUser(builtinVarId, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpLoad:
                    case spv::Op::OpAccessChain:
                    case spv::Op::OpInBoundsAccessChain:
                    case spv::Op::OpPtrAccessChain:
                    case spv::Op::OpInBoundsPtrAccessChain:
                    case spv::Op::OpCopyMemory:
                    case spv::Op::OpCopyMemorySized:
                        users.push_back(user);
                        break;
                    case spv::Op::OpStore:
                        // gl_FragCoord is read-only; a store through it means this is not the
                        // module we think it is.
                        ok = false;
                        break;
                    default:
                        break;
                    }
                });
                if (!ok) return false;
                for (Instruction* user : users) {
                    for (Uint32 i = 0; i < user->NumInOperands(); ++i) {
                        auto& operand = user->GetInOperand(i);
                        if (operand.type == SPV_OPERAND_TYPE_ID && !operand.words.empty() &&
                            operand.words[0] == builtinVarId) {
                            operand.words[0] = copyVarId;
                        }
                    }
                    context()->AnalyzeUses(user);
                }
                return true;
            }

            Uint32 m_framebufferHeight = 0;
        };

        // Decorates the module's captured varyings for VK_EXT_transform_feedback:
        // user outputs get XfbBuffer/XfbStride/Offset directly; a captured
        // gl_Position (a gl_PerVertex member) is mirrored into a dedicated output
        // variable copied before every OpReturn, BEFORE the position fixup runs,
        // so the captured value is the shader's own (pre-remap) gl_Position.
        class XfbCaptureDecoratePass final : public spvtools::opt::Pass {
        public:
            struct CapturedVarying {
                std::string name;
                Uint32 bufferIndex = 0;
                Uint32 offsetBytes = 0;
                // Set when the capture names a member of an output interface block
                // ("Block.member"): the decoration target is then the block's struct TYPE,
                // decorated per member, not the variable. `name` keeps the GL spelling and
                // is useless for the id lookup, so the instance name is carried separately.
                std::string blockInstanceName;
                std::string blockName;
                Int blockMemberIndex = -1;
                Int blockMemberElement = -1; // array element of that member, -1 = the whole member
                Uint32 byteSize = 0;
            };
            const char* name() const override { return "mobilegl-xfb-capture-decorate"; }
            XfbCaptureDecoratePass(Vector<CapturedVarying> varyings, Vector<Uint32> strides)
                : m_varyings(Move(varyings)), m_strides(Move(strides)) {}

            Status Process() override {
                using namespace spvtools::opt;
                if (m_varyings.empty()) return Status::SuccessWithoutChange;

                auto entryPointIter = get_module()->entry_points().begin();
                if (entryPointIter == get_module()->entry_points().end()) return Status::SuccessWithoutChange;
                spvtools::opt::Instruction* entryPoint = &*entryPointIter;
                const Uint32 entryFunctionId = entryPoint->GetSingleWordInOperand(1);

                // Name -> result id map from the debug section.
                std::unordered_map<std::string, Uint32> idsByName;
                for (auto& debugInst : get_module()->debugs2()) {
                    if (debugInst.opcode() != spv::Op::OpName) continue;
                    idsByName[debugInst.GetInOperand(1).AsString()] = debugInst.GetSingleWordInOperand(0);
                }

                auto* decorationManager = context()->get_decoration_mgr();
                const auto decorateForXfb = [&](Uint32 targetId, Uint32 bufferIndex, Uint32 offsetBytes) {
                    const Uint32 stride = bufferIndex < m_strides.size() ? m_strides[bufferIndex] : 0;
                    decorationManager->AddDecorationVal(targetId, static_cast<Uint32>(spv::Decoration::XfbBuffer),
                                                        bufferIndex);
                    decorationManager->AddDecorationVal(targetId, static_cast<Uint32>(spv::Decoration::XfbStride),
                                                        stride);
                    decorationManager->AddDecorationVal(targetId, static_cast<Uint32>(spv::Decoration::Offset),
                                                        offsetBytes);
                };
                // SPIR-V puts XfbBuffer/XfbStride/Offset on the struct MEMBER when the
                // captured varying lives in an interface block (SPIR-V 1.6 §3.20 lists all
                // three as member-decoratable); Offset in particular is illegal on the block
                // variable once the type is decorated Block.
                const auto decorateMemberForXfb = [&](Uint32 structTypeId, Uint32 memberIndex, Uint32 bufferIndex,
                                                      Uint32 offsetBytes) {
                    const Uint32 stride = bufferIndex < m_strides.size() ? m_strides[bufferIndex] : 0;
                    decorationManager->AddMemberDecoration(structTypeId, memberIndex,
                                                           static_cast<Uint32>(spv::Decoration::XfbBuffer),
                                                           bufferIndex);
                    decorationManager->AddMemberDecoration(structTypeId, memberIndex,
                                                           static_cast<Uint32>(spv::Decoration::XfbStride), stride);
                    decorationManager->AddMemberDecoration(structTypeId, memberIndex,
                                                           static_cast<Uint32>(spv::Decoration::Offset), offsetBytes);
                };

                // A member array captured element by element ("Block.attrib[0]" .. "[15]")
                // is one SPIR-V member, so its captures collapse into a single decoration
                // placed at the first element's offset - the rest follow from the member's
                // own layout. Collected first so the group is complete before it decorates.
                struct MemberGroup {
                    Uint32 bufferIndex = 0;
                    Uint32 minOffset = 0;
                    Uint32 elementBytes = 0;
                    Vector<Uint32> offsets;
                };
                std::map<std::pair<Uint32, Uint32>, MemberGroup> memberGroups;

                Bool modified = false;
                Bool needsPositionMirror = false;
                Uint32 positionBufferIndex = 0;
                Uint32 positionOffset = 0;
                for (const auto& varying : m_varyings) {
                    if (varying.name == "gl_Position") {
                        needsPositionMirror = true;
                        positionBufferIndex = varying.bufferIndex;
                        positionOffset = varying.offsetBytes;
                        continue;
                    }
                    if (varying.blockMemberIndex >= 0) {
                        // glslang names the block's instance variable and its struct type
                        // separately; an anonymous instance leaves only the type named, so
                        // both spellings are tried before giving up.
                        Uint32 structTypeId = 0;
                        if (const auto it = idsByName.find(varying.blockInstanceName); it != idsByName.end()) {
                            structTypeId = BlockStructTypeOf(it->second);
                        }
                        if (structTypeId == 0) {
                            if (const auto it = idsByName.find(varying.blockName); it != idsByName.end()) {
                                const spvtools::opt::Instruction* def = context()->get_def_use_mgr()->GetDef(it->second);
                                if (def != nullptr && def->opcode() == spv::Op::OpTypeStruct) {
                                    structTypeId = it->second;
                                } else if (def != nullptr && def->opcode() == spv::Op::OpVariable) {
                                    structTypeId = BlockStructTypeOf(it->second);
                                }
                            }
                        }
                        if (structTypeId == 0) {
                            MGLOG_E("XfbCaptureDecoratePass: no SPIR-V interface block '%s' (instance '%s') for "
                                    "capture '%s'",
                                    varying.blockName.c_str(), varying.blockInstanceName.c_str(),
                                    varying.name.c_str());
                            continue;
                        }
                        auto& group =
                            memberGroups[{structTypeId, static_cast<Uint32>(varying.blockMemberIndex)}];
                        if (group.offsets.empty() || varying.offsetBytes < group.minOffset) {
                            group.minOffset = varying.offsetBytes;
                        }
                        group.bufferIndex = varying.bufferIndex;
                        group.elementBytes = varying.byteSize;
                        group.offsets.push_back(varying.offsetBytes);
                        continue;
                    }
                    const auto idIt = idsByName.find(varying.name);
                    if (idIt == idsByName.end()) {
                        MGLOG_E("XfbCaptureDecoratePass: no SPIR-V variable named '%s'", varying.name.c_str());
                        continue;
                    }
                    decorateForXfb(idIt->second, varying.bufferIndex, varying.offsetBytes);
                    modified = true;
                }

                for (auto& [key, group] : memberGroups) {
                    // The single Offset can only stand for the whole group when the group's
                    // captures are a gap-free ascending run - that is what SPIR-V lays the
                    // member's elements out as. Anything else still gets a best-effort
                    // decoration, but say so, because the capture layout will not match GL.
                    std::sort(group.offsets.begin(), group.offsets.end());
                    for (SizeT i = 1; i < group.offsets.size(); ++i) {
                        if (group.elementBytes == 0 ||
                            group.offsets[i] != group.offsets[i - 1] + group.elementBytes) {
                            MGLOG_I("XfbCaptureDecoratePass: block member %u of type %%%u is captured with a "
                                    "non-contiguous element set; the capture layout will differ from GL's",
                                    key.second, key.first);
                            break;
                        }
                    }
                    decorateMemberForXfb(key.first, key.second, group.bufferIndex, group.minOffset);
                    modified = true;
                }

                if (needsPositionMirror) {
                    modified |= MirrorPositionForCapture(entryFunctionId, *entryPoint, positionBufferIndex,
                                                         positionOffset, decorateForXfb);
                }

                if (!modified) return Status::SuccessWithoutChange;

                context()->AddCapability(spv::Capability::TransformFeedback);
                {
                    auto executionMode = MakeUnique<spvtools::opt::Instruction>(
                        context(), spv::Op::OpExecutionMode, 0, 0,
                        std::initializer_list<spvtools::opt::Operand>{
                            {SPV_OPERAND_TYPE_ID, {entryPoint->GetSingleWordInOperand(1)}},
                            {SPV_OPERAND_TYPE_EXECUTION_MODE, {static_cast<Uint32>(spv::ExecutionMode::Xfb)}}});
                    get_module()->AddExecutionMode(Move(executionMode));
                }
                context()->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

        private:
            // The struct type an interface-block variable points at, peeling an array of
            // block instances on the way. 0 when the id is not a block variable at all.
            Uint32 BlockStructTypeOf(Uint32 variableId) {
                auto* defUse = context()->get_def_use_mgr();
                const spvtools::opt::Instruction* variable = defUse->GetDef(variableId);
                if (variable == nullptr || variable->opcode() != spv::Op::OpVariable) return 0;
                const spvtools::opt::Instruction* pointer = defUse->GetDef(variable->type_id());
                if (pointer == nullptr || pointer->opcode() != spv::Op::OpTypePointer) return 0;
                Uint32 pointeeId = pointer->GetSingleWordInOperand(1);
                for (const spvtools::opt::Instruction* pointee = defUse->GetDef(pointeeId); pointee != nullptr;
                     pointee = defUse->GetDef(pointeeId)) {
                    if (pointee->opcode() == spv::Op::OpTypeStruct) return pointeeId;
                    if (pointee->opcode() != spv::Op::OpTypeArray &&
                        pointee->opcode() != spv::Op::OpTypeRuntimeArray) {
                        return 0;
                    }
                    pointeeId = pointee->GetSingleWordInOperand(0);
                }
                return 0;
            }

            template <typename DecorateFn>
            Bool MirrorPositionForCapture(Uint32 entryFunctionId, spvtools::opt::Instruction& entryPoint,
                                          Uint32 bufferIndex, Uint32 offsetBytes, const DecorateFn& decorateForXfb) {
                const Uint32 entryPointModel = entryPoint.GetSingleWordInOperand(0);
                using namespace spvtools::opt;
                PositionTargetInfo target{};
                if (!FindPositionTarget(context(), &target)) {
                    MGLOG_E("XfbCaptureDecoratePass: gl_Position capture requested but no position output found");
                    return false;
                }
                if (!target.isMember) {
                    // Standalone gl_Position variable: decorate it directly.
                    decorateForXfb(target.variableId, bufferIndex, offsetBytes);
                    return true;
                }

                auto* typeManager = context()->get_type_mgr();
                const Uint32 mirrorPointerTypeId =
                    typeManager->FindPointerToType(target.vectorTypeId, spv::StorageClass::Output);
                if (mirrorPointerTypeId == 0) return false;

                const Uint32 mirrorVariableId = context()->TakeNextId();
                auto mirrorVariable = MakeUnique<Instruction>(
                    context(), spv::Op::OpVariable, mirrorPointerTypeId, mirrorVariableId,
                    std::initializer_list<Operand>{
                        {SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<Uint32>(spv::StorageClass::Output)}}});
                get_module()->AddGlobalValue(Move(mirrorVariable));

                // A free output location: past every explicitly decorated output.
                Uint32 mirrorLocation = 0;
                for (auto& annotation : get_module()->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate ||
                        annotation.GetSingleWordInOperand(1) != static_cast<Uint32>(spv::Decoration::Location)) {
                        continue;
                    }
                    mirrorLocation = std::max(mirrorLocation, annotation.GetSingleWordInOperand(2) + 1);
                }
                auto* decorationManager = context()->get_decoration_mgr();
                decorationManager->AddDecorationVal(mirrorVariableId,
                                                    static_cast<Uint32>(spv::Decoration::Location), mirrorLocation);
                decorateForXfb(mirrorVariableId, bufferIndex, offsetBytes);
                entryPoint.AddOperand({SPV_OPERAND_TYPE_ID, {mirrorVariableId}});

                auto* function = context()->GetFunction(entryFunctionId);
                if (function == nullptr) return false;
                const auto model = static_cast<spv::ExecutionModel>(entryPointModel);
                Bool injected = false;
                for (auto& block : *function) {
                    for (auto instIter = block.begin(); instIter != block.end(); ++instIter) {
                        // Geometry stages capture per emitted vertex; other stages at return.
                        const Bool isInjectionSite =
                            model == spv::ExecutionModel::Geometry
                                ? instIter->opcode() == spv::Op::OpEmitVertex
                                : instIter->opcode() == spv::Op::OpReturn;
                        if (!isInjectionSite) continue;
                        InstructionBuilder builder(context(), &*instIter, IRContext::kAnalysisNone);
                        const Uint32 memberIndexId = builder.GetUintConstantId(target.memberIndex);
                        auto* access =
                            builder.AddAccessChain(target.vectorPtrTypeId, target.variableId, {memberIndexId});
                        if (access == nullptr) return injected;
                        auto* value = builder.AddLoad(target.vectorTypeId, access->result_id());
                        if (value == nullptr) return injected;
                        builder.AddStore(mirrorVariableId, value->result_id());
                        injected = true;
                    }
                }
                return injected;
            }

            Vector<CapturedVarying> m_varyings;
            Vector<Uint32> m_strides;
        };

        // Adreno 650 (driver 512.502) faults the GPU on an implicit-LOD sample of a full-screen
        // colour render target: the texture unit's derivative path reads outside the image's
        // allocation even though the sampler clamps LOD to 0 and the mapping is 1:1. MobileGL's
        // own default-framebuffer blit shader works around it with textureLod, but an
        // application's shader (Minecraft's blit.fsh is `texture(InSampler, texCoord)`) cannot be
        // edited - so rewrite the sample at the SPIR-V level instead.
        //
        // The rewrite is only requested for draws whose every sampler binding is clamped to one
        // mip level, where explicit LOD 0 is exactly what the implicit form must already produce:
        // lambda' = clamp(lambda + bias, minLod, maxLod) with minLod = maxLod = 0. Bias and MinLod
        // operands are therefore dropped rather than translated.
        class ForceExplicitLod0SamplePass final : public spvtools::opt::Pass {
        public:
            const char* name() const override { return "force-explicit-lod0-sample"; }

            Status Process() override {
                Bool isFragment = false;
                for (auto& entryPoint : get_module()->entry_points()) {
                    if (entryPoint.opcode() != spv::Op::OpEntryPoint) continue;
                    if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) ==
                        spv::ExecutionModel::Fragment) {
                        isFragment = true;
                        break;
                    }
                }
                if (!isFragment) return Status::SuccessWithoutChange;

                // Plan first, mutate second. Materializing the LOD constant is itself a module
                // change, so it must not happen unless at least one rewrite is going to follow -
                // otherwise the pass would grow the binary while reporting SuccessWithoutChange.
                Vector<RewritePlan> plans;
                for (auto& function : *get_module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            RewritePlan plan{};
                            if (PlanRewrite(&inst, plan)) plans.push_back(Move(plan));
                        }
                    }
                }
                if (plans.empty()) return Status::SuccessWithoutChange;

                const Uint32 zeroId = GetFloatZeroId();
                if (zeroId == 0) return Status::SuccessWithoutChange;

                for (auto& plan : plans) {
                    plan.operands.push_back({SPV_OPERAND_TYPE_ID, {zeroId}});
                    for (auto& operand : plan.trailingOperands) {
                        plan.operands.push_back(operand);
                    }
                    plan.instruction->SetOpcode(plan.opcode);
                    plan.instruction->SetInOperands(Move(plan.operands));
                }
                // Opcodes and operand lists changed underneath every cached analysis.
                context()->InvalidateAnalysesExceptFor(spvtools::opt::IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

        private:
            struct RewritePlan {
                spvtools::opt::Instruction* instruction = nullptr;
                spv::Op opcode = spv::Op::OpNop;
                // Everything up to and including the Image Operands mask; the Lod id and the
                // trailing operand values are appended once the constant exists.
                Vector<spvtools::opt::Operand> operands;
                Vector<spvtools::opt::Operand> trailingOperands;
            };

            // Image Operands bits that may accompany an implicit-LOD sample, in the canonical
            // ascending order SPIR-V requires the operand values to appear in.
            static constexpr Uint32 kBias = 0x1;
            static constexpr Uint32 kLod = 0x2;
            static constexpr Uint32 kGrad = 0x4;
            static constexpr Uint32 kConstOffset = 0x8;
            static constexpr Uint32 kOffset = 0x10;
            static constexpr Uint32 kConstOffsets = 0x20;
            static constexpr Uint32 kSample = 0x40;
            static constexpr Uint32 kMinLod = 0x80;
            static constexpr Uint32 kKnownMask = 0xFF;

            Uint32 GetFloatZeroId() {
                // Reuse a 32-bit float type already in the module; a shader that samples always has
                // one, and looking it up avoids depending on type-creation API details.
                Uint32 floatTypeId = 0;
                for (auto& inst : get_module()->types_values()) {
                    if (inst.opcode() == spv::Op::OpTypeFloat && inst.NumInOperands() >= 1 &&
                        inst.GetSingleWordInOperand(0) == 32) {
                        floatTypeId = inst.result_id();
                        break;
                    }
                }
                if (floatTypeId == 0) return 0;

                const auto* floatType = context()->get_type_mgr()->GetType(floatTypeId);
                if (floatType == nullptr) return 0;
                const auto zeroBits = std::bit_cast<Uint32>(0.0f);
                const auto* zeroConst = context()->get_constant_mgr()->GetConstant(floatType, {zeroBits});
                if (zeroConst == nullptr) return 0;
                auto* zeroInst = context()->get_constant_mgr()->GetDefiningInstruction(zeroConst);
                return zeroInst != nullptr ? zeroInst->result_id() : 0;
            }

            static Bool MapOpcode(spv::Op op, spv::Op& outOpcode, Uint32& outFixedOperandCount) {
                switch (op) {
                case spv::Op::OpImageSampleImplicitLod:
                    outOpcode = spv::Op::OpImageSampleExplicitLod;
                    outFixedOperandCount = 2; // sampled image, coordinate
                    return true;
                case spv::Op::OpImageSampleProjImplicitLod:
                    outOpcode = spv::Op::OpImageSampleProjExplicitLod;
                    outFixedOperandCount = 2;
                    return true;
                case spv::Op::OpImageSampleDrefImplicitLod:
                    outOpcode = spv::Op::OpImageSampleDrefExplicitLod;
                    outFixedOperandCount = 3; // sampled image, coordinate, Dref
                    return true;
                case spv::Op::OpImageSampleProjDrefImplicitLod:
                    outOpcode = spv::Op::OpImageSampleProjDrefExplicitLod;
                    outFixedOperandCount = 3;
                    return true;
                default:
                    return false;
                }
            }

            static Bool PlanRewrite(spvtools::opt::Instruction* inst, RewritePlan& outPlan) {
                spv::Op newOpcode = spv::Op::OpNop;
                Uint32 fixedCount = 0;
                if (!MapOpcode(inst->opcode(), newOpcode, fixedCount)) return false;
                if (inst->NumInOperands() < fixedCount) return false;

                Uint32 mask = 0;
                Uint32 next = fixedCount;
                if (inst->NumInOperands() > fixedCount) {
                    mask = inst->GetSingleWordInOperand(fixedCount);
                    next = fixedCount + 1;
                }
                // An operand this pass does not model would be silently reordered or dropped, and
                // Grad cannot legally accompany an implicit-LOD sample: leave such an instruction be.
                if ((mask & ~kKnownMask) != 0 || (mask & kGrad) != 0) return false;

                Vector<spvtools::opt::Operand> fixedOperands;
                fixedOperands.reserve(fixedCount + 1);
                for (Uint32 i = 0; i < fixedCount; ++i) {
                    fixedOperands.push_back(inst->GetInOperand(i));
                }

                // Collect the surviving operand values in the same ascending-bit order they were
                // encoded in, so the rebuilt list stays canonical.
                Uint32 keptMask = kLod;
                Vector<spvtools::opt::Operand> keptOperands;
                static constexpr Uint32 kOrderedBits[] = {kBias,   kLod,          kGrad,   kConstOffset,
                                                          kOffset, kConstOffsets, kSample, kMinLod};
                for (const Uint32 bit : kOrderedBits) {
                    if ((mask & bit) == 0) continue;
                    if (next >= inst->NumInOperands()) return false;
                    const spvtools::opt::Operand value = inst->GetInOperand(next++);
                    // Bias and MinLod only shift a lambda that is already clamped to 0, and any
                    // original Lod is replaced by the constant the caller appends.
                    if (bit == kBias || bit == kMinLod || bit == kLod) continue;
                    keptMask |= bit;
                    keptOperands.push_back(value);
                }

                fixedOperands.push_back({SPV_OPERAND_TYPE_IMAGE, {keptMask}});
                outPlan.instruction = inst;
                outPlan.opcode = newOpcode;
                outPlan.operands = Move(fixedOperands);
                outPlan.trailingOperands = Move(keptOperands);
                return true;
            }
        };

        spvtools::Optimizer::PassToken CreateForceExplicitLod0SamplePass() {
            return spvtools::Optimizer::PassToken(MakeUnique<ForceExplicitLod0SamplePass>());
        }

        Bool TransformSpirvForExplicitLod0Sampling(const Vector<Uint>& input, Vector<Uint>& output) {
            if (input.empty()) {
                output.clear();
                return true;
            }
            spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
            spvtools::OptimizerOptions options;
            // Always off: the optimizer's input validator conflates "input invalid" with
            // "transform failed", and this call site fails open. Validating lanes check the
            // FINAL module via ValidateTransformedSpirv, which latches instead of rerouting
            // control flow.
            options.set_run_validator(false);
            optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&,
                                            const char* message) {
                MGLOG_E("Vulkan: explicit-LOD0 pass: %s", message != nullptr ? message : "");
            });
            optimizer.RegisterPass(CreateForceExplicitLod0SamplePass());

            const Bool success = optimizer.Run(input.data(), input.size(), &output, options);
            if (!success) {
                MGLOG_E("Vulkan: explicit-LOD0 sampling pass failed; keeping the original module");
                output = input;
            }
            return success;
        }

        spvtools::Optimizer::PassToken CreateGlToVulkanPositionFixPass(
            ProgramFactory::CompileOptionFlags transformFlags) {
            return spvtools::Optimizer::PassToken(MakeUnique<GlToVulkanPositionFixPass>(transformFlags));
        }

        Bool TransformSpirvForFragCoordYFlip(const Vector<Uint>& input, Vector<Uint>& output,
                                             Uint32 framebufferHeight) {
            if (input.empty()) {
                output.clear();
                return true;
            }
            if (framebufferHeight == 0) {
                output = input;
                return true;
            }

            spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
            spvtools::OptimizerOptions options;
            options.set_run_validator(false); // see TransformSpirvForExplicitLod0Sampling
            optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&,
                                            const char* message) {
                MGLOG_E("Vulkan: fragcoord y-flip pass: %s", message != nullptr ? message : "");
            });
            optimizer.RegisterPass(
                spvtools::Optimizer::PassToken(MakeUnique<GlFragCoordYFlipPass>(framebufferHeight)));

            const Bool success = optimizer.Run(input.data(), input.size(), &output, options);
            if (!success) {
                MGLOG_E("Vulkan: failed to run the gl_FragCoord y-flip pass; keeping the original module");
                output = input;
            }
            return success;
        }

        Bool TransformSpirvForXfbCapture(const Vector<Uint>& input, Vector<Uint>& output,
                                         const MG_State::GLState::ProgramObject& program) {
            if (input.empty()) {
                output.clear();
                return true;
            }
            Vector<XfbCaptureDecoratePass::CapturedVarying> varyings;
            varyings.reserve(program.GetTransformFeedbackVaryingCount());
            for (const auto& varying : program.GetTransformFeedbackVaryings()) {
                varyings.push_back({varying.name, varying.bufferIndex, varying.offsetBytes,
                                    varying.blockInstanceName, varying.blockName, varying.blockMemberIndex,
                                    varying.blockMemberElement, varying.byteSize});
            }
            Vector<Uint32> strides;
            strides.reserve(program.GetTransformFeedbackBufferCount());
            for (SizeT i = 0; i < program.GetTransformFeedbackBufferCount(); ++i) {
                strides.push_back(program.GetTransformFeedbackStride(static_cast<Uint32>(i)));
            }

            spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
            spvtools::OptimizerOptions options;
            options.set_run_validator(false); // see TransformSpirvForExplicitLod0Sampling
            optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&,
                                            const char* message) {
                MGLOG_E("Vulkan: xfb capture pass: %s", message != nullptr ? message : "");
            });
            optimizer.RegisterPass(spvtools::Optimizer::PassToken(
                MakeUnique<XfbCaptureDecoratePass>(Move(varyings), Move(strides))));

            const Bool success = optimizer.Run(input.data(), input.size(), &output, options);
            if (!success) {
                MGLOG_E("Vulkan: xfb capture decoration pass failed; keeping the original module");
                output = input;
            }
            return success;
        }

        Bool TransformSpirvForVulkanPositionFix(const Vector<Uint>& input, Vector<Uint>& output,
                                                ProgramFactory::CompileOptionFlags transformFlags) {
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
            options.set_run_validator(false); // see TransformSpirvForExplicitLod0Sampling
            optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&,
                                            const char* message) {
                MGLOG_E("Vulkan: position fix pass: %s", message != nullptr ? message : "");
            });
            optimizer.RegisterPass(CreateGlToVulkanPositionFixPass(transformFlags));

            const Bool success = optimizer.Run(input.data(), input.size(), &output, options);
            if (!success) {
                MGLOG_E("Vulkan: failed to run GL->Vulkan position fix pass");
                output = input;
            }
            return success;
        }

        ShaderStage PickClipFixupStage(const Vector<SharedPtr<ShaderObject>>& shaders) {
            Bool hasGeometry = false;
            Bool hasTessEval = false;
            Bool hasVertex = false;

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

        ProgramFactory::DescriptorBindingKind ReflectDescriptorTypeToBindingKind(SpvReflectDescriptorType descriptorType) {
            switch (descriptorType) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                return ProgramFactory::DescriptorBindingKind::UniformBufferDynamic;
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                return ProgramFactory::DescriptorBindingKind::CombinedImageSampler;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                return ProgramFactory::DescriptorBindingKind::UniformTexelBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                return ProgramFactory::DescriptorBindingKind::StorageBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return ProgramFactory::DescriptorBindingKind::StorageImage;
            default:
                MOBILEGL_ASSERT(false, "ProgramFactory: unsupported reflected descriptor type %d",
                                static_cast<Int>(descriptorType));
                return ProgramFactory::DescriptorBindingKind::None;
            }
        }

        String NormalizeDescriptorName(const SpvReflectDescriptorBinding& binding,
                                       ProgramFactory::DescriptorBindingKind kind) {
            const char* rawName = binding.name;
            if ((kind == ProgramFactory::DescriptorBindingKind::UniformBufferDynamic ||
                 kind == ProgramFactory::DescriptorBindingKind::StorageBuffer) &&
                binding.type_description != nullptr && binding.type_description->type_name != nullptr) {
                rawName = binding.type_description->type_name;
            }

            String name = (rawName != nullptr) ? rawName : "";
            if (name.empty()) {
                name = std::format("__mg_unnamed_descriptor_set{}_binding{}_id{}", binding.set, binding.binding,
                                   binding.spirv_id);
                MGLOG_W("ProgramFactory: descriptor has empty name; using generated name '%s' (type=%d)",
                        name.c_str(), static_cast<Int>(binding.descriptor_type));
            }
            if (kind == ProgramFactory::DescriptorBindingKind::CombinedImageSampler ||
                kind == ProgramFactory::DescriptorBindingKind::UniformTexelBuffer ||
                kind == ProgramFactory::DescriptorBindingKind::StorageImage) {
                const auto arraySuffix = name.find("[0]");
                if (arraySuffix != String::npos) {
                    name = name.substr(0, arraySuffix);
                }
            }
            return name;
        }

        Bool RemapDescriptorBindingsForVulkan(const Vector<Vector<Uint>>& inputModules, Uint32 maxBindings,
                                              Vector<Vector<Uint>>& outputModules) {
            outputModules = inputModules;

            Vector<SpvReflectShaderModule> reflectModules(outputModules.size());
            Vector<Bool> reflectModuleValid(outputModules.size(), false);
            UnorderedMap<DescriptorKey, Uint32, DescriptorKeyHash> assignedBindings;
            Uint32 nextBinding = 0;

            const auto destroyReflectModules = [&]() {
                for (SizeT moduleIndex = 0; moduleIndex < reflectModules.size(); ++moduleIndex) {
                    if (!reflectModuleValid[moduleIndex]) {
                        continue;
                    }
                    spvReflectDestroyShaderModule(&reflectModules[moduleIndex]);
                    reflectModuleValid[moduleIndex] = false;
                }
            };

            for (SizeT moduleIndex = 0; moduleIndex < outputModules.size(); ++moduleIndex) {
                auto& moduleSpv = outputModules[moduleIndex];
                if (moduleSpv.empty()) {
                    continue;
                }

                const SpvReflectResult createResult =
                    spvReflectCreateShaderModule(moduleSpv.size() * sizeof(Uint), moduleSpv.data(),
                                                 &reflectModules[moduleIndex]);
                MOBILEGL_ASSERT(createResult == SPV_REFLECT_RESULT_SUCCESS,
                                "ProgramFactory: failed to create reflection module for stage %zu (result=%d)",
                                moduleIndex, static_cast<Int>(createResult));
                if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
                    destroyReflectModules();
                    return false;
                }
                reflectModuleValid[moduleIndex] = true;

                uint32_t bindingCount = 0;
                SpvReflectResult reflectResult =
                    spvReflectEnumerateDescriptorBindings(&reflectModules[moduleIndex], &bindingCount, nullptr);
                MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                                "ProgramFactory: failed to enumerate descriptor bindings for stage %zu (result=%d)",
                                moduleIndex, static_cast<Int>(reflectResult));
                if (reflectResult != SPV_REFLECT_RESULT_SUCCESS) {
                    destroyReflectModules();
                    return false;
                }

                Vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
                if (bindingCount > 0) {
                    reflectResult = spvReflectEnumerateDescriptorBindings(&reflectModules[moduleIndex], &bindingCount,
                                                                          bindings.data());
                    MOBILEGL_ASSERT(
                        reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                        "ProgramFactory: failed to fetch descriptor bindings for stage %zu (result=%d)", moduleIndex,
                        static_cast<Int>(reflectResult));
                    if (reflectResult != SPV_REFLECT_RESULT_SUCCESS) {
                        destroyReflectModules();
                        return false;
                    }
                }

                std::sort(bindings.begin(), bindings.end(), [](const auto* lhs, const auto* rhs) {
                    if (lhs->set != rhs->set) {
                        return lhs->set < rhs->set;
                    }
                    if (lhs->binding != rhs->binding) {
                        return lhs->binding < rhs->binding;
                    }
                    return lhs->spirv_id < rhs->spirv_id;
                });

                for (auto* binding : bindings) {
                    MOBILEGL_ASSERT(binding != nullptr, "ProgramFactory: null descriptor binding reflection record");
                    const auto kind = ReflectDescriptorTypeToBindingKind(binding->descriptor_type);
                    // UBO instance arrays (uniform Block {...} b[N];) occupy one binding with
                    // descriptorCount = N; other descriptor arrays stay unsupported and must
                    // fail program creation cleanly rather than continue with corrupt state.
                    if (binding->count != 1 && kind != ProgramFactory::DescriptorBindingKind::UniformBufferDynamic) {
                        MGLOG_E("ProgramFactory: descriptor arrays are unsupported for this descriptor "
                                "kind (name='%s' count=%u type=%d)",
                                binding->name ? binding->name : "<null>", binding->count,
                                static_cast<Int>(binding->descriptor_type));
                        destroyReflectModules();
                        return false;
                    }

                    DescriptorKey key{};
                    key.kind = kind;
                    key.name = NormalizeDescriptorName(*binding, kind);

                    Uint32 assignedBinding = 0;
                    const auto it = assignedBindings.find(key);
                    if (it == assignedBindings.end()) {
                        MOBILEGL_ASSERT(nextBinding < maxBindings,
                                        "ProgramFactory: reflected descriptor count exceeded maxBindings (%u >= %u)",
                                        nextBinding, maxBindings);
                        assignedBinding = nextBinding;
                        assignedBindings.emplace(key, assignedBinding);
                        ++nextBinding;
                    } else {
                        assignedBinding = it->second;
                    }

                    if (binding->binding != assignedBinding || binding->set != 0) {
                        reflectResult = spvReflectChangeDescriptorBindingNumbers(&reflectModules[moduleIndex], binding,
                                                                                assignedBinding, 0);
                        MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                                        "ProgramFactory: failed to remap descriptor '%s' in stage %zu (result=%d)",
                                        key.name.c_str(), moduleIndex, static_cast<Int>(reflectResult));
                        if (reflectResult != SPV_REFLECT_RESULT_SUCCESS) {
                            destroyReflectModules();
                            return false;
                        }
                    }
                }
            }

            for (SizeT moduleIndex = 0; moduleIndex < outputModules.size(); ++moduleIndex) {
                if (!reflectModuleValid[moduleIndex]) {
                    continue;
                }

                const Uint32 codeSizeBytes = spvReflectGetCodeSize(&reflectModules[moduleIndex]);
                MOBILEGL_ASSERT((codeSizeBytes % sizeof(Uint)) == 0,
                                "ProgramFactory: reflected SPIR-V size is not word aligned for stage %zu",
                                moduleIndex);
                const Uint32* code = spvReflectGetCode(&reflectModules[moduleIndex]);
                MOBILEGL_ASSERT(code != nullptr, "ProgramFactory: reflected SPIR-V code pointer is null for stage %zu",
                                moduleIndex);
                outputModules[moduleIndex].assign(code, code + (codeSizeBytes / sizeof(Uint)));
            }

            destroyReflectModules();
            return true;
        }

        TextureTarget ReflectImageTraitsToTextureTarget(const SpvReflectImageTraits& imageTraits) {
            switch (imageTraits.dim) {
            case SpvDim1D:
                return imageTraits.arrayed != 0 ? TextureTarget::Texture1DArray : TextureTarget::Texture1D;
            case SpvDim2D:
                if (imageTraits.ms != 0) {
                    return imageTraits.arrayed != 0 ? TextureTarget::Texture2DMultisampleArray
                                                    : TextureTarget::Texture2DMultisample;
                }
                return imageTraits.arrayed != 0 ? TextureTarget::Texture2DArray : TextureTarget::Texture2D;
            case SpvDim3D:
                return TextureTarget::Texture3D;
            case SpvDimCube:
                return imageTraits.arrayed != 0 ? TextureTarget::TextureCubeMapArray : TextureTarget::TextureCubeMap;
            case SpvDimBuffer:
                return TextureTarget::TextureBuffer;
            default:
                MOBILEGL_ASSERT(false, "ProgramFactory: unsupported sampler image dim %d", imageTraits.dim);
                return TextureTarget::Unknown;
            }
        }

        Bool IsFloatStorageImageUniformType(GLenum uniformType) {
            switch (uniformType) {
            case GL_IMAGE_1D:
            case GL_IMAGE_2D:
            case GL_IMAGE_3D:
            case GL_IMAGE_2D_RECT:
            case GL_IMAGE_CUBE:
            case GL_IMAGE_BUFFER:
            case GL_IMAGE_1D_ARRAY:
            case GL_IMAGE_2D_ARRAY:
            case GL_IMAGE_CUBE_MAP_ARRAY:
            case GL_IMAGE_2D_MULTISAMPLE:
            case GL_IMAGE_2D_MULTISAMPLE_ARRAY:
                return true;
            default:
                return false;
            }
        }
    } // namespace

    // A shader that assigns gl_FragDepth (SPIR-V DepthReplacing) supplies depth itself
    // instead of taking the pipeline's interpolated Z, so a driver that varies the vertex
    // position math between pipelines cannot desynchronize it; the blended depth-write
    // quirk therefore leaves it alone (see PipelineFactory::ShouldSuppressDepthWrite).
    Bool ProgramFactory::ReflectedFragmentReplacesDepth(const SpvReflectShaderModule& reflectModule) {
        for (Uint32 entryIndex = 0; entryIndex < reflectModule.entry_point_count; ++entryIndex) {
            const SpvReflectEntryPoint& entryPoint = reflectModule.entry_points[entryIndex];
            for (Uint32 modeIndex = 0; modeIndex < entryPoint.execution_mode_count; ++modeIndex) {
                if (entryPoint.execution_modes[modeIndex] == SpvExecutionModeDepthReplacing) {
                    return true;
                }
            }
        }
        return false;
    }

    // glslang's relaxed-Vulkan mode maps GL's gl_InstanceID onto the InstanceIndex builtin.
    // Without shaderDrawParameters there is no gl_BaseInstance to subtract, so such a shader
    // cannot be corrected and instanced draws with a non-zero baseInstance misrender; this
    // detects the case so the user gets one warning instead of silent corruption.
    Bool ProgramFactory::ReflectedReadsInstanceIndexBuiltin(const SpvReflectShaderModule& reflectModule) {
        for (Uint32 entryIndex = 0; entryIndex < reflectModule.entry_point_count; ++entryIndex) {
            const SpvReflectEntryPoint& entryPoint = reflectModule.entry_points[entryIndex];
            for (Uint32 variableIndex = 0; variableIndex < entryPoint.input_variable_count; ++variableIndex) {
                const SpvReflectInterfaceVariable* variable = entryPoint.input_variables[variableIndex];
                if (variable != nullptr &&
                    (variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0 &&
                    variable->built_in == SpvBuiltInInstanceIndex) {
                    return true;
                }
            }
        }
        return false;
    }

    VkShaderStageFlagBits ProgramFactory::ToVkStage(ShaderStage stage) {
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

    VkFormat ProgramFactory::ConvertSpirvImageFormatToVkFormat(SpvImageFormat format) {
        switch (format) {
        case SpvImageFormatUnknown: return VK_FORMAT_UNDEFINED;
        case SpvImageFormatRgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case SpvImageFormatRgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case SpvImageFormatR32f: return VK_FORMAT_R32_SFLOAT;
        case SpvImageFormatRgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        case SpvImageFormatRgba8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
        case SpvImageFormatRg32f: return VK_FORMAT_R32G32_SFLOAT;
        case SpvImageFormatRg16f: return VK_FORMAT_R16G16_SFLOAT;
        case SpvImageFormatR11fG11fB10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case SpvImageFormatR16f: return VK_FORMAT_R16_SFLOAT;
        case SpvImageFormatRgba16: return VK_FORMAT_R16G16B16A16_UNORM;
        case SpvImageFormatRgb10A2: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case SpvImageFormatRg16: return VK_FORMAT_R16G16_UNORM;
        case SpvImageFormatRg8: return VK_FORMAT_R8G8_UNORM;
        case SpvImageFormatR16: return VK_FORMAT_R16_UNORM;
        case SpvImageFormatR8: return VK_FORMAT_R8_UNORM;
        case SpvImageFormatRgba16Snorm: return VK_FORMAT_R16G16B16A16_SNORM;
        case SpvImageFormatRg16Snorm: return VK_FORMAT_R16G16_SNORM;
        case SpvImageFormatRg8Snorm: return VK_FORMAT_R8G8_SNORM;
        case SpvImageFormatR16Snorm: return VK_FORMAT_R16_SNORM;
        case SpvImageFormatR8Snorm: return VK_FORMAT_R8_SNORM;
        case SpvImageFormatRgba32i: return VK_FORMAT_R32G32B32A32_SINT;
        case SpvImageFormatRgba16i: return VK_FORMAT_R16G16B16A16_SINT;
        case SpvImageFormatRgba8i: return VK_FORMAT_R8G8B8A8_SINT;
        case SpvImageFormatR32i: return VK_FORMAT_R32_SINT;
        case SpvImageFormatRg32i: return VK_FORMAT_R32G32_SINT;
        case SpvImageFormatRg16i: return VK_FORMAT_R16G16_SINT;
        case SpvImageFormatRg8i: return VK_FORMAT_R8G8_SINT;
        case SpvImageFormatR16i: return VK_FORMAT_R16_SINT;
        case SpvImageFormatR8i: return VK_FORMAT_R8_SINT;
        case SpvImageFormatRgba32ui: return VK_FORMAT_R32G32B32A32_UINT;
        case SpvImageFormatRgba16ui: return VK_FORMAT_R16G16B16A16_UINT;
        case SpvImageFormatRgba8ui: return VK_FORMAT_R8G8B8A8_UINT;
        case SpvImageFormatR32ui: return VK_FORMAT_R32_UINT;
        case SpvImageFormatRgb10a2ui: return VK_FORMAT_A2R10G10B10_UINT_PACK32;
        case SpvImageFormatRg32ui: return VK_FORMAT_R32G32_UINT;
        case SpvImageFormatRg16ui: return VK_FORMAT_R16G16_UINT;
        case SpvImageFormatRg8ui: return VK_FORMAT_R8G8_UINT;
        case SpvImageFormatR16ui: return VK_FORMAT_R16_UINT;
        case SpvImageFormatR8ui: return VK_FORMAT_R8_UINT;
        case SpvImageFormatR64ui: return VK_FORMAT_R64_UINT;
        case SpvImageFormatR64i: return VK_FORMAT_R64_SINT;
        case SpvImageFormatMax: return VK_FORMAT_UNDEFINED;
        }
        return VK_FORMAT_UNDEFINED;
    }

    SamplerNumericDomain ProgramFactory::UniformTypeToSamplerNumericDomain(GLenum glType) {
        switch (glType) {
        case GL_INT_SAMPLER_1D:
        case GL_INT_SAMPLER_2D:
        case GL_INT_SAMPLER_3D:
        case GL_INT_SAMPLER_CUBE:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_INT_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
            return SamplerNumericDomain::SignedInteger;
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
            return SamplerNumericDomain::UnsignedInteger;
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_BUFFER:
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            return SamplerNumericDomain::Float;
        default:
            return SamplerNumericDomain::Unknown;
        }
    }

    ProgramFactory::HashType ProgramFactory::ComputeHash(const MG_State::GLState::ProgramObject& program,
                                                         CompileOptionFlags flags) const {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));
        // We expect shader stages in program object are sorted
        const auto& spirvs = program.GetGeneratedSpirv();
        for (const auto& spv : spirvs) {
            XXHASH_VERIFY(XXH64_update(m_hashState, spv.data(), spv.size() * sizeof(Uint)));
        }
        XXHASH_VERIFY(XXH64_update(m_hashState, &flags, sizeof(CompileOptionFlags)));
        // Only FragCoordYFlip variants bake the height in, so mixing it unconditionally would
        // re-key every program in the cache on a resize for no reason.
        if (flags & CompileOptionBit::FragCoordYFlip) {
            XXHASH_VERIFY(XXH64_update(m_hashState, &m_defaultFramebufferHeight,
                                        sizeof(m_defaultFramebufferHeight)));
        }

        // Include UBO block bindings in hash so different binding configurations produce different entries
        const Uint32 blockCount = static_cast<Uint32>(program.GetActiveUniformBlocksCount());
        XXHASH_VERIFY(XXH64_update(m_hashState, &blockCount, sizeof(blockCount)));
        for (Uint32 i = 0; i < blockCount; ++i) {
            const Uint32 binding = program.GetUniformBlockBinding(i);
            XXHASH_VERIFY(XXH64_update(m_hashState, &binding, sizeof(binding)));
        }

        // The transform feedback capture layout is baked into the modules by
        // XfbCaptureDecoratePass rather than coming from the SPIR-V, so it has to be part of
        // the key: two programs can share every shader and still capture differently, which
        // is exactly what changing the buffer mode does (glTransformFeedbackVaryings with the
        // same varyings but GL_SEPARATE_ATTRIBS instead of GL_INTERLEAVED_ATTRIBS). Only
        // hashed for a capturing compile, so nothing else changes key.
        if (flags & CompileOptionBit::XfbCapture) {
            for (const auto& varying : program.GetTransformFeedbackVaryings()) {
                XXHASH_VERIFY(XXH64_update(m_hashState, varying.name.data(), varying.name.size()));
                XXHASH_VERIFY(XXH64_update(m_hashState, &varying.bufferIndex, sizeof(varying.bufferIndex)));
                XXHASH_VERIFY(XXH64_update(m_hashState, &varying.offsetBytes, sizeof(varying.offsetBytes)));
            }
            const SizeT bufferCount = program.GetTransformFeedbackBufferCount();
            for (SizeT i = 0; i < bufferCount; ++i) {
                const Uint32 stride = program.GetTransformFeedbackStride(static_cast<Uint32>(i));
                XXHASH_VERIFY(XXH64_update(m_hashState, &stride, sizeof(stride)));
            }
        }

        HashType hash = XXH64_digest(m_hashState);
        return hash;
    }

    TextureTarget ProgramFactory::UniformTypeToTextureTarget(GLenum glType) {
        switch (glType) {
        case GL_SAMPLER_1D:
        case GL_INT_SAMPLER_1D:
        case GL_UNSIGNED_INT_SAMPLER_1D:
        case GL_IMAGE_1D:
        case GL_INT_IMAGE_1D:
        case GL_UNSIGNED_INT_IMAGE_1D:
            return TextureTarget::Texture1D;
        case GL_SAMPLER_3D:
        case GL_INT_SAMPLER_3D:
        case GL_UNSIGNED_INT_SAMPLER_3D:
        case GL_IMAGE_3D:
        case GL_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_3D:
            return TextureTarget::Texture3D;
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_IMAGE_CUBE:
        case GL_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
            return TextureTarget::TextureCubeMap;
        case GL_SAMPLER_2D_MULTISAMPLE:
        case GL_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE:
        case GL_IMAGE_2D_MULTISAMPLE:
        case GL_INT_IMAGE_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE:
            return TextureTarget::Texture2DMultisample;
        case GL_SAMPLER_BUFFER:
        case GL_INT_SAMPLER_BUFFER:
        case GL_UNSIGNED_INT_SAMPLER_BUFFER:
        case GL_IMAGE_BUFFER:
        case GL_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
            return TextureTarget::TextureBuffer;
        case GL_SAMPLER_1D_ARRAY:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_INT_SAMPLER_1D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY:
        case GL_IMAGE_1D_ARRAY:
        case GL_INT_IMAGE_1D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_1D_ARRAY:
            return TextureTarget::Texture1DArray;
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        case GL_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
            return TextureTarget::Texture2DArray;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY:
        case GL_IMAGE_2D_MULTISAMPLE_ARRAY:
        case GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
            return TextureTarget::Texture2DMultisampleArray;
        case GL_SAMPLER_2D_RECT:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_INT_SAMPLER_2D_RECT:
        case GL_UNSIGNED_INT_SAMPLER_2D_RECT:
        case GL_IMAGE_2D_RECT:
        case GL_INT_IMAGE_2D_RECT:
        case GL_UNSIGNED_INT_IMAGE_2D_RECT:
            return TextureTarget::TextureRectangle;
        case GL_SAMPLER_2D:
        case GL_SAMPLER_2D_SHADOW:
        case GL_INT_SAMPLER_2D:
        case GL_UNSIGNED_INT_SAMPLER_2D:
        case GL_IMAGE_2D:
        case GL_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        default:
            return TextureTarget::Texture2D;
        }
    }

    void ProgramFactory::ReflectVertexInputs(const Vector<SharedPtr<MG_State::GLState::ShaderObject>>& shaders,
                                             const Vector<Vector<Uint>>& spirv,
                                             VkProgramObject& entry) const {
        entry.activeVertexInputLocationMask = 0;
        entry.vertexInputTypes.fill(0);

        for (SizeT moduleIndex = 0; moduleIndex < shaders.size() && moduleIndex < spirv.size(); ++moduleIndex) {
            if (!shaders[moduleIndex] || shaders[moduleIndex]->GetShaderStage() != ShaderStage::Vertex) {
                continue;
            }

            const auto& module = spirv[moduleIndex];
            if (module.empty()) {
                continue;
            }

            SpvReflectShaderModule reflectModule{};
            const SpvReflectResult createResult =
                spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
            MOBILEGL_ASSERT(createResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectVertexInputs: failed to create reflection module (result=%d)",
                            static_cast<Int>(createResult));
            if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
                continue;
            }

            if (!m_shaderDrawParametersEnabled && ReflectedReadsInstanceIndexBuiltin(reflectModule)) {
                static Bool s_warnedInstanceIndexUnsupported = false;
                if (!s_warnedInstanceIndexUnsupported) {
                    s_warnedInstanceIndexUnsupported = true;
                    MGLOG_W("ProgramFactory: shaderDrawParameters is unavailable; gl_InstanceID cannot be "
                            "rebased and instanced draws with a non-zero baseInstance may render incorrectly");
                }
            }

            uint32_t inputCount = 0;
            SpvReflectResult reflectResult = spvReflectEnumerateInputVariables(&reflectModule, &inputCount, nullptr);
            MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectVertexInputs: failed to enumerate input variables (result=%d)",
                            static_cast<Int>(reflectResult));
            Vector<SpvReflectInterfaceVariable*> inputs(inputCount);
            if (reflectResult == SPV_REFLECT_RESULT_SUCCESS && inputCount > 0) {
                reflectResult = spvReflectEnumerateInputVariables(&reflectModule, &inputCount, inputs.data());
                MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                                "ProgramFactory::ReflectVertexInputs: failed to fetch input variables (result=%d)",
                                static_cast<Int>(reflectResult));
            }

            if (reflectResult == SPV_REFLECT_RESULT_SUCCESS) {
                for (auto* input : inputs) {
                    if (input == nullptr || (input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                        continue;
                    }

                    const GLenum locationType = GetReflectInterfaceLocationType(*input);
                    MOBILEGL_ASSERT(locationType != GL_FALSE,
                                    "ProgramFactory::ReflectVertexInputs: unsupported vertex input type at location=%u name='%s'",
                                    input->location,
                                    input->name ? input->name : "<null>");
                    const Uint32 locationSpan = GetReflectInterfaceLocationSpan(*input);
                    for (Uint32 locationOffset = 0; locationOffset < locationSpan; ++locationOffset) {
                        const Uint32 expandedLocation = input->location + locationOffset;
                        if (expandedLocation >= VkProgramObject::kMaxVertexInputLocations) {
                            break;
                        }

                        entry.activeVertexInputLocationMask |= (1u << expandedLocation);
                        entry.vertexInputTypes[expandedLocation] = locationType;
                    }
                }
            }

            spvReflectDestroyShaderModule(&reflectModule);
            break;
        }
    }

    void ProgramFactory::ReflectFragmentOutputs(const Vector<SharedPtr<MG_State::GLState::ShaderObject>>& shaders,
                                                const Vector<Vector<Uint>>& spirv,
                                                VkProgramObject& entry) const {
        entry.activeFragmentOutputLocationMask = 0;
        entry.fragmentOutputTypes.fill(0);
        entry.fragmentReplacesDepth = false;

        for (SizeT moduleIndex = 0; moduleIndex < shaders.size() && moduleIndex < spirv.size(); ++moduleIndex) {
            if (!shaders[moduleIndex] || shaders[moduleIndex]->GetShaderStage() != ShaderStage::Fragment) {
                continue;
            }

            const auto& module = spirv[moduleIndex];
            if (module.empty()) {
                continue;
            }

            SpvReflectShaderModule reflectModule{};
            const SpvReflectResult createResult =
                spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
            MOBILEGL_ASSERT(createResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectFragmentOutputs: failed to create reflection module (result=%d)",
                            static_cast<Int>(createResult));
            if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
                // Fail toward the exemption: stripping a genuine gl_FragDepth writer would
                // corrupt its depth output outright, while wrongly exempting an accumulation
                // pass merely reverts that one program to the pre-quirk behavior.
                entry.fragmentReplacesDepth = true;
                continue;
            }

            entry.fragmentReplacesDepth = ReflectedFragmentReplacesDepth(reflectModule);

            uint32_t outputCount = 0;
            SpvReflectResult reflectResult = spvReflectEnumerateOutputVariables(&reflectModule, &outputCount, nullptr);
            MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectFragmentOutputs: failed to enumerate output variables (result=%d)",
                            static_cast<Int>(reflectResult));
            Vector<SpvReflectInterfaceVariable*> outputs(outputCount);
            if (reflectResult == SPV_REFLECT_RESULT_SUCCESS && outputCount > 0) {
                reflectResult = spvReflectEnumerateOutputVariables(&reflectModule, &outputCount, outputs.data());
                MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                                "ProgramFactory::ReflectFragmentOutputs: failed to fetch output variables (result=%d)",
                                static_cast<Int>(reflectResult));
            }

            if (reflectResult == SPV_REFLECT_RESULT_SUCCESS) {
                for (auto* output : outputs) {
                    if (output == nullptr || (output->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                        continue;
                    }

                    const GLenum locationType = GetReflectInterfaceLocationType(*output);
                    MOBILEGL_ASSERT(locationType != GL_FALSE,
                                    "ProgramFactory::ReflectFragmentOutputs: unsupported fragment output type at location=%u name='%s'",
                                    output->location,
                                    output->name ? output->name : "<null>");
                    const Uint32 locationSpan = GetReflectInterfaceLocationSpan(*output);
                    for (Uint32 locationOffset = 0; locationOffset < locationSpan; ++locationOffset) {
                        const Uint32 expandedLocation = output->location + locationOffset;
                        if (expandedLocation >= VkProgramObject::kMaxVertexInputLocations) {
                            break;
                        }

                        entry.activeFragmentOutputLocationMask |= (1u << expandedLocation);
                        entry.fragmentOutputTypes[expandedLocation] = locationType;
                    }
                }
            }

            spvReflectDestroyShaderModule(&reflectModule);
            break;
        }
    }

    void ProgramFactory::ReflectLayout(const MG_State::GLState::ProgramObject& program,
                                       const Vector<Vector<Uint>>& spirv, VkProgramObject& entry) const {
        // Initialize layout vectors
        entry.bindingKinds.assign(m_maxBindings, DescriptorBindingKind::None);
        entry.uniformBlockIndexByBinding.assign(m_maxBindings, -1);
        entry.samplerNameByBinding.assign(m_maxBindings, String());
        entry.samplerUniformLocationByBinding.assign(m_maxBindings, -1);
        entry.samplerTextureTargetByBinding.assign(m_maxBindings, TextureTarget::Texture2D);
        entry.samplerNumericDomainByBinding.assign(m_maxBindings, SamplerNumericDomain::Unknown);
        entry.storageImageFormatByBinding.assign(m_maxBindings, VK_FORMAT_UNDEFINED);
        entry.storageImageUsesBindingFormatByBinding.assign(m_maxBindings, false);
        entry.storageBlockNameByBinding.assign(m_maxBindings, String());
        entry.storageBlockIndexByBinding.assign(m_maxBindings, -1);
        entry.globalUboBinding = -1;
        entry.dynamicBindings.clear();
        entry.bindingDescriptorCounts.assign(m_maxBindings, 1);
        entry.arrayedUniformBlockIndicesByBinding.clear();

        // Use SpvcSession (Reflection mode) to reflect all SPIR-V modules in a single pass per module
        for (const auto& module : spirv) {
            if (module.empty()) {
                continue;
            }

            SpvcSession session(module, SessionUsageBit::Reflection);
            SpvReflectShaderModule reflectModule{};
            const SpvReflectResult createReflectResult =
                spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
            MOBILEGL_ASSERT(createReflectResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectLayout: failed to create reflection module (result=%d)",
                            static_cast<Int>(createReflectResult));

            // Descriptor counts per binding (UBO instance arrays reflect count > 1).
            UnorderedMap<Uint32, Uint32> descriptorCountByBinding;
            {
                uint32_t countProbe = 0;
                if (spvReflectEnumerateDescriptorBindings(&reflectModule, &countProbe, nullptr) ==
                        SPV_REFLECT_RESULT_SUCCESS &&
                    countProbe > 0) {
                    Vector<SpvReflectDescriptorBinding*> probeBindings(countProbe);
                    if (spvReflectEnumerateDescriptorBindings(&reflectModule, &countProbe,
                                                              probeBindings.data()) ==
                        SPV_REFLECT_RESULT_SUCCESS) {
                        for (const auto* probeBinding : probeBindings) {
                            if (probeBinding != nullptr) {
                                descriptorCountByBinding[probeBinding->binding] =
                                    std::max<Uint32>(1, probeBinding->count);
                            }
                        }
                    }
                }
            }

            // Reflect uniform buffers
            auto ubos = session.GetShaderInterface(SPVC_RESOURCE_TYPE_UNIFORM_BUFFER);
            for (const auto& ubo : ubos) {
                const Uint32 binding = ubo.location; // GetShaderInterface stores binding in location field
                MOBILEGL_ASSERT(binding < m_maxBindings,
                                "ProgramFactory::ReflectLayout: UBO binding %u exceeds maxBindings=%u for '%s'",
                                binding, m_maxBindings, ubo.name.c_str());

                // Check for global UBO
                if (std::strstr(ubo.name.c_str(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != nullptr) {
                    MOBILEGL_ASSERT(entry.bindingKinds[binding] == DescriptorBindingKind::None ||
                                        entry.bindingKinds[binding] == DescriptorBindingKind::UniformBufferDynamic,
                                    "ProgramFactory::ReflectLayout: descriptor binding %u has conflicting kinds for UBO '%s'",
                                    binding, ubo.name.c_str());
                    entry.bindingKinds[binding] = DescriptorBindingKind::UniformBufferDynamic;
                    MOBILEGL_ASSERT(entry.globalUboBinding < 0 || entry.globalUboBinding == static_cast<Int>(binding),
                                    "ProgramFactory::ReflectLayout: global UBO binding mismatch (%d vs %u)",
                                    entry.globalUboBinding, binding);
                    MOBILEGL_ASSERT(entry.uniformBlockIndexByBinding[binding] < 0,
                                    "ProgramFactory::ReflectLayout: global UBO shares binding %u with regular UBO index %d",
                                    binding, entry.uniformBlockIndexByBinding[binding]);
                    entry.globalUboBinding = static_cast<Int>(binding);
                    continue;
                }

                const auto countIt = descriptorCountByBinding.find(binding);
                const Uint32 descriptorCount =
                    countIt != descriptorCountByBinding.end() ? countIt->second : 1u;

                if (descriptorCount <= 1) {
                    const Uint blockIndex = program.GetUniformBlockIndex(ubo.name.c_str());
                    if (blockIndex == 0xFFFFFFFFu) {
                        MGLOG_D("ProgramFactory::ReflectLayout: skipping inactive UBO '%s' at binding %u",
                                ubo.name.c_str(), binding);
                        continue;
                    }

                    MOBILEGL_ASSERT(entry.bindingKinds[binding] == DescriptorBindingKind::None ||
                                        entry.bindingKinds[binding] == DescriptorBindingKind::UniformBufferDynamic,
                                    "ProgramFactory::ReflectLayout: descriptor binding %u has conflicting kinds for UBO '%s'",
                                    binding, ubo.name.c_str());
                    entry.bindingKinds[binding] = DescriptorBindingKind::UniformBufferDynamic;
                    MOBILEGL_ASSERT(entry.globalUboBinding != static_cast<Int>(binding),
                                    "ProgramFactory::ReflectLayout: regular UBO '%s' collides with global UBO binding %u",
                                    ubo.name.c_str(), binding);
                    MOBILEGL_ASSERT(entry.uniformBlockIndexByBinding[binding] < 0 ||
                                        entry.uniformBlockIndexByBinding[binding] == static_cast<Int>(blockIndex),
                                    "ProgramFactory::ReflectLayout: descriptor binding %u maps to conflicting UBO blocks (%d vs %u)",
                                    binding, entry.uniformBlockIndexByBinding[binding], blockIndex);
                    entry.uniformBlockIndexByBinding[binding] = static_cast<Int>(blockIndex);
                    continue;
                }

                // UBO instance array: one binding, descriptorCount elements. GL exposes each
                // element as its own active block named "Name[i]"; map every element to its
                // GL block index so the descriptor write can gather per-element buffer ranges.
                if (descriptorCount > m_maxBindings) {
                    MGLOG_E("ProgramFactory::ReflectLayout: UBO array '%s' count %u exceeds maxBindings=%u; "
                            "leaving binding %u unmapped",
                            ubo.name.c_str(), descriptorCount, m_maxBindings, binding);
                    continue;
                }
                Vector<Int> elementBlockIndices;
                elementBlockIndices.reserve(descriptorCount);
                for (Uint32 element = 0; element < descriptorCount; ++element) {
                    String elementName = ubo.name + "[" + std::to_string(element) + "]";
                    Uint elementBlockIndex = program.GetUniformBlockIndex(elementName.c_str());
                    if (elementBlockIndex == 0xFFFFFFFFu && element == 0) {
                        // Some frontends report the first element under the bare block name.
                        elementBlockIndex = program.GetUniformBlockIndex(ubo.name.c_str());
                    }
                    if (elementBlockIndex == 0xFFFFFFFFu) {
                        // Degrade rather than corrupt: reuse element 0's block if we have one,
                        // otherwise give up on the binding (same observable behavior as an
                        // inactive block: wrong values, but no crash).
                        MGLOG_E("ProgramFactory::ReflectLayout: UBO array '%s' element %u has no active "
                                "GL uniform block",
                                ubo.name.c_str(), element);
                        if (!elementBlockIndices.empty()) {
                            elementBlockIndex = static_cast<Uint>(elementBlockIndices.front());
                        } else {
                            break;
                        }
                    }
                    elementBlockIndices.push_back(static_cast<Int>(elementBlockIndex));
                }
                if (elementBlockIndices.size() != descriptorCount) {
                    MGLOG_E("ProgramFactory::ReflectLayout: skipping unresolved UBO array '%s' at binding %u",
                            ubo.name.c_str(), binding);
                    continue;
                }

                MOBILEGL_ASSERT(entry.bindingKinds[binding] == DescriptorBindingKind::None ||
                                    entry.bindingKinds[binding] == DescriptorBindingKind::UniformBufferDynamic,
                                "ProgramFactory::ReflectLayout: descriptor binding %u has conflicting kinds for UBO '%s'",
                                binding, ubo.name.c_str());
                entry.bindingKinds[binding] = DescriptorBindingKind::UniformBufferDynamic;
                entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(descriptorCount);
                entry.uniformBlockIndexByBinding[binding] = elementBlockIndices[0];
                entry.arrayedUniformBlockIndicesByBinding[binding] = Move(elementBlockIndices);
            }

            // Reflect sampled images, storage images, samplerBuffer uniforms, and SSBOs.
            uint32_t reflectedBindingCount = 0;
            SpvReflectResult reflectResult =
                spvReflectEnumerateDescriptorBindings(&reflectModule, &reflectedBindingCount, nullptr);
            MOBILEGL_ASSERT(reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                            "ProgramFactory::ReflectLayout: failed to enumerate descriptor bindings (result=%d)",
                            static_cast<Int>(reflectResult));

            Vector<SpvReflectDescriptorBinding*> reflectedBindings(reflectedBindingCount);
            if (reflectedBindingCount > 0) {
                reflectResult = spvReflectEnumerateDescriptorBindings(&reflectModule, &reflectedBindingCount,
                                                                      reflectedBindings.data());
                MOBILEGL_ASSERT(
                    reflectResult == SPV_REFLECT_RESULT_SUCCESS,
                    "ProgramFactory::ReflectLayout: failed to fetch descriptor bindings (result=%d)",
                    static_cast<Int>(reflectResult));
            }

            for (const auto* sampler : reflectedBindings) {
                if (sampler == nullptr) {
                    continue;
                }
                const auto descriptorKind = ReflectDescriptorTypeToBindingKind(sampler->descriptor_type);
                if (descriptorKind != DescriptorBindingKind::CombinedImageSampler &&
                    descriptorKind != DescriptorBindingKind::UniformTexelBuffer &&
                    descriptorKind != DescriptorBindingKind::StorageImage &&
                    descriptorKind != DescriptorBindingKind::StorageBuffer) {
                    continue;
                }

                const Uint32 binding = sampler->binding;
                const String uniformName = NormalizeDescriptorName(*sampler, descriptorKind);
                MOBILEGL_ASSERT(binding < m_maxBindings,
                                "ProgramFactory::ReflectLayout: sampler binding %u exceeds maxBindings=%u for '%s'",
                                binding, m_maxBindings, uniformName.c_str());

                MOBILEGL_ASSERT(entry.bindingKinds[binding] == DescriptorBindingKind::None ||
                                    entry.bindingKinds[binding] == descriptorKind,
                                "ProgramFactory::ReflectLayout: descriptor binding %u has conflicting kinds for resource '%s'",
                                binding, uniformName.c_str());
                entry.bindingKinds[binding] = descriptorKind;

                if (descriptorKind == DescriptorBindingKind::StorageBuffer) {
                    const GLuint blockIndex = GetShaderStorageBlockIndex(program, uniformName);
                    if (blockIndex == GL_INVALID_INDEX) {
                        MGLOG_D("ProgramFactory::ReflectLayout: skipping inactive SSBO '%s' at binding %u",
                                uniformName.c_str(), binding);
                        entry.bindingKinds[binding] = DescriptorBindingKind::None;
                        continue;
                    }
                    entry.storageBlockNameByBinding[binding] = uniformName;
                    entry.storageBlockIndexByBinding[binding] = static_cast<Int>(blockIndex);

                    // A block INSTANCE array is ONE Vulkan binding carrying `count`
                    // descriptors, while GL assigns its elements consecutive binding points
                    // starting at the declared one (GL 4.6 core 7.8). Recording only element 0 -
                    // which is all this used to do - left the layout claiming descriptorCount 1,
                    // so every element past the first read a descriptor nobody wrote and
                    // `b[1].data.length()` answered from an unconstrained buffer instead of its
                    // own bound range (KHR-GL43.shader_storage_buffer_object.-
                    // advanced-unsizedArrayLength-*).
                    entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(std::max<Uint32>(1u, sampler->count));
                    continue;
                }

                const Int location = program.GetUniformLocation(uniformName);
                if (location < 0) {
                    entry.bindingKinds[binding] = DescriptorBindingKind::None;
                    continue;
                }

                const GLenum uniformType = program.GetUniformType(static_cast<Uint>(location));

                if (descriptorKind == DescriptorBindingKind::StorageImage) {
                    // An ARRAY of image uniforms is ONE binding carrying `count` descriptors,
                    // and the layout has to say so. Leaving it at the default 1 declared
                    // `uniform image2D g_image[4]` as a single-descriptor binding while the
                    // shader indexed descriptors 1..3 of it - an out-of-bounds descriptor
                    // access that lavapipe SIGSEGVs inside the JIT-ed shader thread rather than
                    // reporting (KHR-GL42.shader_image_load_store.advanced-sso-simple). Unlike
                    // a storage BLOCK array, whose elements take consecutive GL binding points
                    // from the declared one, each element of an image array carries its own
                    // independently assigned image unit - see ResolveStorageImageDescriptor.
                    entry.bindingDescriptorCounts[binding] =
                        static_cast<Uint16>(std::max<Uint32>(1u, sampler->count));

                    const VkFormat reflectedFormat =
                        ConvertSpirvImageFormatToVkFormat(sampler->image.image_format);
                    VkFormat& existingFormat = entry.storageImageFormatByBinding[binding];
                    MOBILEGL_ASSERT(existingFormat == VK_FORMAT_UNDEFINED ||
                                        reflectedFormat == VK_FORMAT_UNDEFINED ||
                                        existingFormat == reflectedFormat,
                                    "ProgramFactory::ReflectLayout: storage image binding %u ('%s') has "
                                    "conflicting reflected formats (%d vs %d)",
                                    binding, uniformName.c_str(), static_cast<Int>(existingFormat),
                                    static_cast<Int>(reflectedFormat));
                    if (existingFormat == VK_FORMAT_UNDEFINED) {
                        existingFormat = reflectedFormat;
                    }
                    if (m_unformattedFloatStorageImagesEnabled &&
                        existingFormat == VK_FORMAT_UNDEFINED &&
                        IsFloatStorageImageUniformType(uniformType)) {
                        entry.storageImageUsesBindingFormatByBinding[binding] = true;
                    } else if (reflectedFormat != VK_FORMAT_UNDEFINED) {
                        // A typed declaration in any stage wins for the entire binding. This is
                        // required when another stage reaches the same image through an atomic
                        // path and therefore could not be made formatless.
                        entry.storageImageUsesBindingFormatByBinding[binding] = false;
                    }
                }

                const TextureTarget target = UniformTypeToTextureTarget(uniformType);
                MOBILEGL_ASSERT(target != TextureTarget::Unknown,
                                "ProgramFactory::ReflectLayout: failed to resolve texture target for '%s'",
                                uniformName.c_str());
                if (descriptorKind == DescriptorBindingKind::CombinedImageSampler) {
                    const SamplerNumericDomain numericDomain = UniformTypeToSamplerNumericDomain(uniformType);
                    MOBILEGL_ASSERT(numericDomain != SamplerNumericDomain::Unknown,
                                    "ProgramFactory::ReflectLayout: failed to resolve sampler numeric domain "
                                    "for '%s' (uniformType=0x%x)",
                                    uniformName.c_str(), uniformType);
                    MOBILEGL_ASSERT(entry.samplerNumericDomainByBinding[binding] ==
                                            SamplerNumericDomain::Unknown ||
                                        entry.samplerNumericDomainByBinding[binding] == numericDomain,
                                    "ProgramFactory::ReflectLayout: sampler binding %u ('%s') has conflicting "
                                    "numeric domains (%d vs %d)",
                                    binding, uniformName.c_str(),
                                    static_cast<Int>(entry.samplerNumericDomainByBinding[binding]),
                                    static_cast<Int>(numericDomain));
                    entry.samplerNumericDomainByBinding[binding] = numericDomain;
                }
                MOBILEGL_ASSERT(entry.samplerUniformLocationByBinding[binding] < 0 || location < 0 ||
                                    entry.samplerUniformLocationByBinding[binding] == location,
                                "ProgramFactory::ReflectLayout: texture binding %u maps to conflicting uniform locations (%d vs %d)",
                                binding, entry.samplerUniformLocationByBinding[binding], location);
                MOBILEGL_ASSERT(entry.samplerUniformLocationByBinding[binding] < 0 ||
                                    entry.samplerTextureTargetByBinding[binding] == target,
                                "ProgramFactory::ReflectLayout: texture binding %u maps to conflicting texture targets (%d vs %d)",
                                binding, static_cast<Int>(entry.samplerTextureTargetByBinding[binding]),
                                static_cast<Int>(target));
                MOBILEGL_ASSERT(entry.samplerNameByBinding[binding].empty() ||
                                    entry.samplerNameByBinding[binding] == uniformName,
                                "ProgramFactory::ReflectLayout: texture binding %u maps to conflicting names ('%s' vs '%s')",
                                binding, entry.samplerNameByBinding[binding].c_str(), uniformName.c_str());

                if (location >= 0) {
                    entry.samplerUniformLocationByBinding[binding] = location;
                }
                entry.samplerNameByBinding[binding] = uniformName;
                entry.samplerTextureTargetByBinding[binding] = target;
            }

            spvReflectDestroyShaderModule(&reflectModule);
        }

        // Build Vulkan descriptor set layout and pipeline layout from reflected binding kinds
        Vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(m_maxBindings);
        for (Uint32 binding = 0; binding < m_maxBindings; ++binding) {
            const auto kind = entry.bindingKinds[binding];
            if (kind == DescriptorBindingKind::None) {
                continue;
            }

            VkDescriptorSetLayoutBinding layoutBinding{};
            layoutBinding.binding = binding;
            layoutBinding.descriptorCount = entry.bindingDescriptorCounts[binding];
            layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
            layoutBinding.pImmutableSamplers = nullptr;
            if (kind == DescriptorBindingKind::UniformBufferDynamic) {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                entry.dynamicBindings.push_back(binding);
            } else if (kind == DescriptorBindingKind::UniformTexelBuffer) {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            } else if (kind == DescriptorBindingKind::StorageBuffer) {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            } else if (kind == DescriptorBindingKind::StorageImage) {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                entry.hasStorageImages = true;
            } else {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            bindings.push_back(layoutBinding);
        }

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = static_cast<Uint32>(bindings.size());
        setLayoutInfo.pBindings = bindings.data();
        VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &setLayoutInfo, nullptr, &entry.descriptorSetLayout),
                  "ProgramFactory::ReflectLayout, vkCreateDescriptorSetLayout");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &entry.descriptorSetLayout;
        VK_VERIFY(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &entry.pipelineLayout),
                  "ProgramFactory::ReflectLayout, vkCreatePipelineLayout");

        // Built here rather than where bindingKinds is sized: at that point the vector is only
        // zero-initialised and the kinds are assigned further down, so a list built there would be
        // empty. Ascending by construction because the index walks upward.
        entry.activeBindings.clear();
        for (Uint32 binding = 0; binding < static_cast<Uint32>(entry.bindingKinds.size()); ++binding) {
            if (entry.bindingKinds[binding] != DescriptorBindingKind::None) {
                entry.activeBindings.push_back(binding);
            }
        }
    }

    void ProgramFactory::SetDefaultFramebufferHeight(Uint32 height) {
        if (m_defaultFramebufferHeight == height) {
            return;
        }
        m_defaultFramebufferHeight = height;
        // Both memos key on (program, flags) alone, so neither can tell the two heights apart:
        // drop the lookup memo, and bump the structure epoch so every caller holding a
        // VkProgramObject* re-runs GetOrCreateProgram and lands on the new hash. The cached
        // entries themselves stay - they are keyed by a hash that now includes the old height,
        // so they can only be reached again if that height comes back, and the frame-boundary
        // sweep retires them otherwise.
        m_lastLookup = {};
        ++m_cacheStructureEpoch;
    }

    const ProgramFactory::VkProgramObject& ProgramFactory::GetOrCreateProgram(
        const MG_State::GLState::ProgramObject& program, CompileOptionFlags flags) {
        // Hashing the full SPIR-V of every stage is far too expensive to repeat per draw;
        // reuse the program's memoized hash while its backend state version is unchanged.
        // The memo keys on the flags word, which ComputeHash is no longer a pure function of:
        // a FragCoordYFlip variant also depends on the baked default-framebuffer height, so
        // that height rides in the free high half of the key. Flags occupy the low bits, and a
        // height cannot exceed the 16 bits a swapchain extent fits in.
        const Uint memoKey = (flags & CompileOptionBit::FragCoordYFlip)
                                 ? (flags.GetRaw() | (m_defaultFramebufferHeight << 16))
                                 : flags.GetRaw();
        HashType hash = 0;
        if (!program.GetBackendHashMemo(memoKey, hash)) {
            hash = ComputeHash(program, flags);
            program.SetBackendHashMemo(memoKey, hash);
        }
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            // Every draw/dispatch funnels through this lookup (the renderer memos only
            // skip re-hashing, never the factory lookup), so an actively-used entry is
            // stamped at least once per frame boundary and can never be aged out while
            // any in-flight command buffer still references it.
            it->second.lastUsedFrame = m_frameCounter;
            return it->second;
        }

        // Structural change: the insert below can move every entry of this
        // open-addressing map, so all memoised entry pointers die here.
        ++m_cacheStructureEpoch;
        auto& entry = m_cache[hash];
        entry.hash = hash;
        entry.lastUsedFrame = m_frameCounter;
        auto& shaders = program.GetAttachedShaders();
        auto& spirv = program.GetGeneratedSpirv();
        Vector<Vector<Uint>> moduleSpirvs(spirv.size());

        const ShaderStage fixupStage = PickClipFixupStage(shaders);

        for (SizeT i = 0; i < shaders.size(); ++i) {
            auto& spv = spirv[i];
            if (spv.empty()) continue;

            // Apply position fixup if needed
            if (fixupStage != ShaderStage::Unknown && shaders[i] && shaders[i]->GetShaderStage() == fixupStage) {
                const Vector<Uint>* fixupInput = &spv;
                Vector<Uint> xfbSpirv;
                if ((flags & ProgramFactory::CompileOptionBit::XfbCapture) &&
                    program.GetTransformFeedbackVaryingCount() > 0) {
                    // Decorate BEFORE the position fixup so a captured gl_Position
                    // mirror copies the shader's own (pre-remap) value.
                    if (TransformSpirvForXfbCapture(spv, xfbSpirv, program)) {
                        fixupInput = &xfbSpirv;
                    }
                }
                TransformSpirvForVulkanPositionFix(*fixupInput, moduleSpirvs[i], flags);
            } else {
                moduleSpirvs[i] = spv;
            }

            if ((flags & ProgramFactory::CompileOptionBit::ExplicitLod0Sampling) && shaders[i] &&
                shaders[i]->GetShaderStage() == ShaderStage::Fragment) {
                Vector<Uint> explicitLodSpirv;
                if (TransformSpirvForExplicitLod0Sampling(moduleSpirvs[i], explicitLodSpirv)) {
                    moduleSpirvs[i] = Move(explicitLodSpirv);
                }
            }

            if ((flags & ProgramFactory::CompileOptionBit::FragCoordYFlip) && shaders[i] &&
                shaders[i]->GetShaderStage() == ShaderStage::Fragment) {
                Vector<Uint> fragCoordSpirv;
                if (TransformSpirvForFragCoordYFlip(moduleSpirvs[i], fragCoordSpirv, m_defaultFramebufferHeight)) {
                    moduleSpirvs[i] = Move(fragCoordSpirv);
                }
            }

            // Vulkan's SPIR-V environment has no rectangle image dimension, so a
            // GL_TEXTURE_RECTANGLE lookup has to become the 2D one the texture is really
            // stored as - which addresses [0,1] where the application addressed texels.
            {
                Vector<Uint> rectLoweredSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::LowerRectImages(moduleSpirvs[i], rectLoweredSpirv) &&
                    !rectLoweredSpirv.empty()) {
                    moduleSpirvs[i] = Move(rectLoweredSpirv);
                }
            }

            // GL apps depend on cross-program position invariance for multi-pass equality
            // depth tests (MC 26.3's OIT re-draws the cloud geometry with GEQUAL against the
            // depth its own first pass wrote); decorate Position outputs Invariant so
            // per-pipeline compilers cannot vary the position math between passes.
            {
                Vector<Uint> invariantSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::DecoratePositionInvariantForVulkan(
                        moduleSpirvs[i], invariantSpirv)) {
                    moduleSpirvs[i] = std::move(invariantSpirv);
                } else {
                    // The pass round-trips through SPIRV-Tools IR, so an unparseable module
                    // fails open and keeps the undecorated words - which silently reinstates
                    // the multi-pass invariance bug rather than breaking anything loudly.
                    MGLOG_E("ProgramFactory: position-invariant decoration failed for program %u; "
                            "keeping the original module - multi-pass depth-equality chains "
                            "(e.g. MC 26.3 OIT clouds) may drop primitives on this device",
                            program.GetExternalIndex());
                }
            }

            // glslang's relaxed-Vulkan mode aliases GL's zero-based gl_InstanceID to Vulkan's
            // gl_InstanceIndex, which wrongly includes the draw's baseInstance. Rebase vertex-stage
            // loads to (InstanceIndex - BaseInstance) so shaders observe GL semantics. Reflection
            // below runs on the rebased words so the added BaseInstance builtin stays consistent.
            // The unsupported-device counterpart of this rebase (warning when a shader reads
            // the builtin but shaderDrawParameters is missing) rides along with
            // ReflectVertexInputs, which already reflects this stage.
            if (shaders[i] && shaders[i]->GetShaderStage() == ShaderStage::Vertex &&
                m_shaderDrawParametersEnabled) {
                Vector<Uint> rebasedSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::RebaseInstanceIndexForVulkan(moduleSpirvs[i],
                                                                                            rebasedSpirv)) {
                    moduleSpirvs[i] = std::move(rebasedSpirv);
                } else {
                    MGLOG_E("ProgramFactory: failed to rebase gl_InstanceID for program %u; "
                            "instanced draws with a non-zero baseInstance may render incorrectly",
                            program.GetExternalIndex());
                }
            }

            // A 64-bit vertex input has to arrive as its 32-bit word pair: VK_FORMAT_R64*_SFLOAT is
            // optional and lavapipe advertises none of them at all. The pass is unconditional so it
            // always agrees with the Float64 case in VertexInputStateFactory::ToVkVertexFormat, and
            // ReflectVertexInputs below then sees an ordinary uvec2/uvec4 input.
            //
            // Failure here is not recoverable and must not be swallowed: ToVkVertexFormat has already
            // committed to R32G32{,B32A32}_UINT for the attribute, so a module still declaring
            // `in double` would reconcile to Unknown and build a pipeline with a UINT format under a
            // double input - garbage with no diagnostic anywhere.
            if (shaders[i] && shaders[i]->GetShaderStage() == ShaderStage::Vertex) {
                Vector<Uint> packedSpirv;
                const Bool packOk = MG_Util::ShaderTranspiler::ShaderCompiler::PackDoubleVertexInputsForVulkan(
                    moduleSpirvs[i], packedSpirv);
                MOBILEGL_ASSERT(packOk,
                                "ProgramFactory: 64-bit vertex input packing failed for program %u; the "
                                "vertex-input format and the shader input type now disagree",
                                program.GetExternalIndex());
                if (packOk) {
                    moduleSpirvs[i] = std::move(packedSpirv);
                } else {
                    MGLOG_E("ProgramFactory: failed to pack 64-bit vertex inputs for program %u; "
                            "double-typed vertex attributes will be fetched as uint32 words and not "
                            "reinterpreted",
                            program.GetExternalIndex());
                }
            }

            // When Vulkan can legally access storage images without a statically declared
            // format, let GL's glBindImageTexture format select the runtime image view. This
            // provides desktop-driver-compatible behavior for packs such as iterationRP, whose
            // float image qualifier can disagree with the bound render-target format. Integer
            // storage images remain formatted so r32ui/r32i bit-reinterpretation paths keep the
            // exact descriptor format required by their shader operations.
            if (m_unformattedFloatStorageImagesEnabled) {
                Vector<Uint> unformattedSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(
                        moduleSpirvs[i], unformattedSpirv)) {
                    moduleSpirvs[i] = std::move(unformattedSpirv);
                } else {
                    MGLOG_E("ProgramFactory: failed to make float storage images unformatted for program %u",
                            program.GetExternalIndex());
                }
            }
        }

        const Bool remapOk = RemapDescriptorBindingsForVulkan(moduleSpirvs, m_maxBindings, moduleSpirvs);
        MOBILEGL_ASSERT(remapOk, "ProgramFactory::GetOrCreateProgram: descriptor binding remap failed");

        for (SizeT i = 0; i < shaders.size(); ++i) {
            auto& moduleSpv = moduleSpirvs[i];
            if (moduleSpv.empty()) continue;

#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
            ValidateTransformedSpirv(moduleSpv, shaders[i]->GetShaderStage(), program.GetExternalIndex());
#else
            // Final module the driver receives; also checked in the INFO-level CI/test
            // lanes, where the DEBUG gate above is compiled out.
            if (MG_Util::ShaderTranspiler::ShaderCompiler::SpirvValidationEnabled()) {
                ValidateTransformedSpirv(moduleSpv, shaders[i]->GetShaderStage(), program.GetExternalIndex());
            }
#endif

            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = moduleSpv.size() * sizeof(Uint);
            smci.pCode = moduleSpv.data();

            VkShaderModule module = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateShaderModule(m_device, &smci, nullptr, &module), "vkCreateShaderModule");

            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ShaderStage shaderStage = shaders[i]->GetShaderStage();
            stage.stage = ToVkStage(shaderStage);
            stage.module = module;
            stage.pName = "main";

            entry.modules.push_back(module);
            entry.stages.push_back(stage);
            entry.stageSpirvDigests.push_back(ShaderStageSpirvDigest{
                static_cast<Uint32>(stage.stage), static_cast<Uint32>(moduleSpv.size()),
                XXH64(moduleSpv.data(), moduleSpv.size() * sizeof(Uint), 0)});
        }

        // Reflect and create layout as part of the program object
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        ValidateRasterizationStageInterface(shaders, moduleSpirvs, entry, program.GetExternalIndex());
#endif
        ReflectVertexInputs(shaders, moduleSpirvs, entry);
        ReflectFragmentOutputs(shaders, moduleSpirvs, entry);
        ReflectLayout(program, moduleSpirvs, entry);

        return entry;
    }

    void ProgramFactory::OnFrameBoundary() {
        ++m_frameCounter;

        // Sweep cadence and retire age mirror VkRenderPassManager::OnPresent: an entry
        // idle for more than kRetireAgeFrames frame boundaries cannot be referenced by
        // any in-flight command buffer (frames-in-flight <= MOBILEGL_MAGMA_FRAMESINFLIGHT),
        // so its shader modules and layouts are destroyed immediately - no deferred-
        // destroy machinery needed. Eviction is content-based, never tied to
        // glDeleteProgram: the cache is content-hash-shared across GL programs, so a
        // delete-driven erase could free an entry another live program still resolves.
        // An evicted entry self-heals - the frontend program keeps its generated
        // SPIR-V, so the next GetOrCreateProgram rebuilds it (this also covers the
        // renderer's internal blit/depth-mipmap programs).
        constexpr Uint64 kSweepInterval = 256;
        constexpr Uint64 kRetireAgeFrames = 1024;
        if ((m_frameCounter % kSweepInterval) != 0) {
            return;
        }

        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (m_frameCounter - it->second.lastUsedFrame > kRetireAgeFrames) {
                const HashType hash = it->first;
                const VkDescriptorSetLayout descriptorSetLayout = it->second.descriptorSetLayout;
                MGLOG_D("ProgramFactory::OnFrameBoundary: evicting idle program entry hash=0x%llx",
                        static_cast<unsigned long long>(hash));
                // erase runs ~VkProgramObject (modules/layouts destroyed); notify after
                // so an observer never observes a half-destroyed entry through a lookup.
                // Observers only need the handle values to purge their keyed caches.
                ++m_cacheStructureEpoch; // erase moves/kills entries: memoised pointers die
                it = m_cache.erase(it);
                if (m_evictionObserver != nullptr) {
                    m_evictionObserver->OnProgramEvicted(hash, descriptorSetLayout);
                }
            } else {
                ++it;
            }
        }
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
