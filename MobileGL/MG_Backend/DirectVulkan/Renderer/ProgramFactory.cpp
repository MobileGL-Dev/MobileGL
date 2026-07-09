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
#include <cstring>
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

        // Cheap raw-word scan for an `OpDecorate <id> BuiltIn InstanceIndex` decoration. Used
        // only to decide whether to warn when shaderDrawParameters is unavailable; a false
        // negative merely suppresses a diagnostic.
        Bool SpirvDeclaresInstanceIndexBuiltin(const Vector<Uint>& spirv) {
            constexpr Uint32 kSpirvMagicNumber = 0x07230203u;
            constexpr SizeT kHeaderWordCount = 5;
            if (spirv.size() <= kHeaderWordCount || spirv[0] != kSpirvMagicNumber) {
                return false;
            }

            SizeT wordIndex = kHeaderWordCount;
            while (wordIndex < spirv.size()) {
                const Uint32 firstWord = spirv[wordIndex];
                const Uint32 wordCount = firstWord >> 16;
                const auto opcode = static_cast<spv::Op>(firstWord & 0xffffu);
                if (wordCount == 0 || wordIndex + wordCount > spirv.size()) {
                    break;
                }
                if (opcode == spv::Op::OpDecorate && wordCount >= 4 &&
                    static_cast<spv::Decoration>(spirv[wordIndex + 2]) == spv::Decoration::BuiltIn &&
                    static_cast<spv::BuiltIn>(spirv[wordIndex + 3]) == spv::BuiltIn::InstanceIndex) {
                    return true;
                }
                wordIndex += wordCount;
            }
            return false;
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

        spvtools::Optimizer::PassToken CreateGlToVulkanPositionFixPass(
            ProgramFactory::CompileOptionFlags transformFlags) {
            return spvtools::Optimizer::PassToken(MakeUnique<GlToVulkanPositionFixPass>(transformFlags));
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
            options.set_run_validator(false);
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
                    MOBILEGL_ASSERT(binding->count == 1,
                                    "ProgramFactory: descriptor arrays are unsupported (name='%s' count=%u)",
                                    binding->name ? binding->name : "<null>", binding->count);

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
    } // namespace

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

    ProgramFactory::HashType ProgramFactory::ComputeHash(const MG_State::GLState::ProgramObject& program,
                                                         CompileOptionFlags flags) const {
        XXHASH_VERIFY(XXH64_reset(m_hashState, m_config.CacheVersion));
        // We expect shader stages in program object are sorted
        const auto& spirvs = program.GetGeneratedSpirv();
        for (const auto& spv : spirvs) {
            XXHASH_VERIFY(XXH64_update(m_hashState, spv.data(), spv.size() * sizeof(Uint)));
        }
        XXHASH_VERIFY(XXH64_update(m_hashState, &flags, sizeof(CompileOptionFlags)));

        // Include UBO block bindings in hash so different binding configurations produce different entries
        const Uint32 blockCount = static_cast<Uint32>(program.GetActiveUniformBlocksCount());
        XXHASH_VERIFY(XXH64_update(m_hashState, &blockCount, sizeof(blockCount)));
        for (Uint32 i = 0; i < blockCount; ++i) {
            const Uint32 binding = program.GetUniformBlockBinding(i);
            XXHASH_VERIFY(XXH64_update(m_hashState, &binding, sizeof(binding)));
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
                continue;
            }

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
        entry.storageBlockNameByBinding.assign(m_maxBindings, String());
        entry.storageBlockIndexByBinding.assign(m_maxBindings, -1);
        entry.globalUboBinding = -1;
        entry.dynamicBindings.clear();

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
                    continue;
                }

                const Int location = program.GetUniformLocation(uniformName);
                if (location < 0) {
                    entry.bindingKinds[binding] = DescriptorBindingKind::None;
                    continue;
                }

                const TextureTarget target = UniformTypeToTextureTarget(program.GetUniformType(static_cast<Uint>(location)));
                MOBILEGL_ASSERT(target != TextureTarget::Unknown,
                                "ProgramFactory::ReflectLayout: failed to resolve texture target for '%s'",
                                uniformName.c_str());
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
            layoutBinding.descriptorCount = 1;
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
    }

    const ProgramFactory::VkProgramObject& ProgramFactory::GetOrCreateProgram(
        const MG_State::GLState::ProgramObject& program, CompileOptionFlags flags) {
        // Hashing the full SPIR-V of every stage is far too expensive to repeat per draw;
        // reuse the program's memoized hash while its backend state version is unchanged.
        HashType hash = 0;
        if (!program.GetBackendHashMemo(flags.GetRaw(), hash)) {
            hash = ComputeHash(program, flags);
            program.SetBackendHashMemo(flags.GetRaw(), hash);
        }
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            return it->second;
        }

        auto& entry = m_cache[hash];
        entry.hash = hash;
        auto& shaders = program.GetAttachedShaders();
        auto& spirv = program.GetGeneratedSpirv();
        Vector<Vector<Uint>> moduleSpirvs(spirv.size());

        const ShaderStage fixupStage = PickClipFixupStage(shaders);

        for (SizeT i = 0; i < shaders.size(); ++i) {
            auto& spv = spirv[i];
            if (spv.empty()) continue;

            // Apply position fixup if needed
            if (fixupStage != ShaderStage::Unknown && shaders[i] && shaders[i]->GetShaderStage() == fixupStage) {
                TransformSpirvForVulkanPositionFix(spv, moduleSpirvs[i], flags);
            } else {
                moduleSpirvs[i] = spv;
            }

            // glslang's relaxed-Vulkan mode aliases GL's zero-based gl_InstanceID to Vulkan's
            // gl_InstanceIndex, which wrongly includes the draw's baseInstance. Rebase vertex-stage
            // loads to (InstanceIndex - BaseInstance) so shaders observe GL semantics. Reflection
            // below runs on the rebased words so the added BaseInstance builtin stays consistent.
            if (shaders[i] && shaders[i]->GetShaderStage() == ShaderStage::Vertex) {
                if (m_shaderDrawParametersEnabled) {
                    Vector<Uint> rebasedSpirv;
                    if (MG_Util::ShaderTranspiler::ShaderCompiler::RebaseInstanceIndexForVulkan(moduleSpirvs[i],
                                                                                               rebasedSpirv)) {
                        moduleSpirvs[i] = std::move(rebasedSpirv);
                    } else {
                        MGLOG_E("ProgramFactory: failed to rebase gl_InstanceID for program %u; "
                                "instanced draws with a non-zero baseInstance may render incorrectly",
                                program.GetExternalIndex());
                    }
                } else if (SpirvDeclaresInstanceIndexBuiltin(moduleSpirvs[i])) {
                    static Bool s_warnedInstanceIndexUnsupported = false;
                    if (!s_warnedInstanceIndexUnsupported) {
                        s_warnedInstanceIndexUnsupported = true;
                        MGLOG_W("ProgramFactory: shaderDrawParameters is unavailable; gl_InstanceID cannot be "
                                "rebased and instanced draws with a non-zero baseInstance may render incorrectly");
                    }
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
} // namespace MobileGL::MG_Backend::DirectVulkan
