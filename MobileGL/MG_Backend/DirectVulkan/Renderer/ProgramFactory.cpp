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
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
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

        // Local size of a compute module, read from OpExecutionMode LocalSize; all-zero
        // when absent. The compile chain pins SPIR-V 1.3, where a literal local size
        // always reaches the module as this execution mode (LocalSizeId does not exist
        // yet).
        struct ComputeLocalSize {
            Uint32 x = 0;
            Uint32 y = 0;
            Uint32 z = 0;
            Uint64 Total() const { return static_cast<Uint64>(x) * y * z; }
        };
        ComputeLocalSize TryGetComputeLocalSize(const Vector<Uint>& spirv) {
            constexpr SizeT kHeaderWords = 5;
            constexpr Uint32 kOpExecutionMode = 16;
            constexpr Uint32 kModeLocalSize = 17;
            for (SizeT offset = kHeaderWords; offset < spirv.size();) {
                const Uint32 wordCount = spirv[offset] >> 16u;
                const Uint32 opcode = spirv[offset] & 0xffffu;
                if (wordCount == 0 || offset + wordCount > spirv.size()) break;
                if (opcode == kOpExecutionMode && wordCount >= 6 && spirv[offset + 2] == kModeLocalSize) {
                    return {spirv[offset + 3], spirv[offset + 4], spirv[offset + 5]};
                }
                offset += wordCount;
            }
            return {};
        }

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

        // Where a gl_PerVertex built-in output lives, resolved from the module's annotations.
        // Named for gl_Position because the clip-space fixup is what it was written for, and it
        // is still the only shape that pass accepts - but the transform-feedback capture pass
        // resolves gl_PointSize through the same struct, in which case `vectorTypeId` /
        // `vectorPtrTypeId` hold the SCALAR float type and its Output pointer rather than a vec4.
        struct PositionTargetInfo {
            Uint32 variableId = 0;
            Uint32 vectorTypeId = 0;
            Uint32 floatTypeId = 0;
            Uint32 vectorPtrTypeId = 0;
            Uint32 memberIndex = 0;
            Bool isMember = false;
        };

        ShaderStage PickClipFixupStage(const Vector<ShaderStage>& stages);

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

        // gl_PointSize's counterpart to IsVec4Float32. The two are the only shapes any
        // gl_PerVertex member this file resolves can have, and each resolver takes whichever
        // one its built-in is declared with, so a mismatched type declines rather than
        // producing a mirror the driver would reject.
        Bool IsFloat32Scalar(spvtools::opt::IRContext* context, Uint32 typeId, Uint32* outFloatTypeId) {
            auto* floatInst = context->get_def_use_mgr()->GetDef(typeId);
            if (!floatInst || floatInst->opcode() != spv::Op::OpTypeFloat) return false;
            if (floatInst->GetSingleWordInOperand(0) != 32) return false;

            if (outFloatTypeId) *outFloatTypeId = typeId;
            return true;
        }

        // Which of the two shapes above a resolver should accept. A plain function pointer
        // rather than a std::function: every call site is one of the two free functions.
        using BuiltInTypeCheckFn = Bool (*)(spvtools::opt::IRContext*, Uint32, Uint32*);

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

        // What a failed validation says, for a caller that wants to put it in its own message.
        struct SpirvValidationFailure {
            String message;
            Int result = 0;
            SizeT index = 0;
        };

        // Returns whether the module validates. The result used to be discarded everywhere: the
        // call was DEBUG-or-env gated and only logged, so an invalid module produced by a backend
        // transform went straight to vkCreateShaderModule. That is not a survivable outcome on
        // this hardware - Mali r54 SIGSEGVs building the pipeline instead of returning an error,
        // the same "not a validating entry point" behaviour PipelineFactory already documents for
        // vkCreateGraphicsPipelines - so the callers that feed the driver now act on it.
        //
        // This function does NOT log the failure at E any more. It used to, unlatched, on the
        // stated grounds that "reaching here already requires the validation switch to be armed,
        // which bounds the volume" - and that premise died when the two GetOrCreateProgram call
        // sites became unconditional: MGLOG_E is live at the production INFO level, and Log.h's
        // own rule is that anything at W or E on a repeatable path must be latched or demoted.
        // The failure text now travels back through `outFailure` so the LATCHED call-site
        // messages carry the VUID instead of an unlatched inner one repeating it; what stays here
        // is the D-level detail and the process-wide counter the test lanes assert on.
        Bool ValidateTransformedSpirv(const Vector<Uint>& spirv, ShaderStage shaderStage, Uint programExternalIndex,
                                      SpirvValidationFailure* outFailure = nullptr) {
            if (outFailure != nullptr) *outFailure = {};
            if (spirv.empty()) {
                return true;
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
                const char* message =
                    diagnostic != nullptr && diagnostic->error != nullptr ? diagnostic->error : "<null>";
                const SizeT index = diagnostic != nullptr ? diagnostic->position.index : 0;
                // The test-lane signal (ShaderCompiler.h documents harnesses snapshotting it and
                // asserting on the delta). Bumped for every failed validation, including one a
                // caller goes on to recover from: a transform that produced an invalid module is
                // a real defect whether or not this run survived it.
                MG_Util::ShaderTranspiler::ShaderCompiler::NoteSpirvValidationFailure();
                if (outFailure != nullptr) {
                    *outFailure = {String(message), static_cast<Int>(result), index};
                }
                MGLOG_D(
                    "ProgramFactory::ValidateTransformedSpirv: validation failed for stage=%d program=%u result=%d index=%zu msg=%s",
                    static_cast<Int>(shaderStage),
                    programExternalIndex,
                    static_cast<Int>(result),
                    index,
                    message);
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
            return result == SPV_SUCCESS;
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
                                   const Vector<ShaderStage>& stages,
                                   const Vector<Vector<Uint>>& spirv,
                                   StageInterfaceSummary& outSummary,
                                   Uint programExternalIndex,
                                   const char* stageLabel) {
            outSummary.slotSignatures.fill(0);

            for (SizeT moduleIndex = 0; moduleIndex < stages.size() && moduleIndex < spirv.size(); ++moduleIndex) {
                if (stages[moduleIndex] != targetStage) {
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

        void ValidateRasterizationStageInterface(const Vector<ShaderStage>& stages,
                                                 const Vector<Vector<Uint>>& spirv,
                                                 ProgramFactory::VkProgramObject& entry,
                                                 Uint programExternalIndex) {
            const ShaderStage producerStage = PickClipFixupStage(stages);
            entry.rasterizationProducerStage = producerStage;
            entry.producerOutputComponentCount = 0;
            entry.fragmentInputComponentCount = 0;
            if (producerStage == ShaderStage::Unknown) {
                return;
            }

            Bool hasFragmentStage = false;
            for (const ShaderStage stage : stages) {
                if (stage == ShaderStage::Fragment) {
                    hasFragmentStage = true;
                    break;
                }
            }
            if (!hasFragmentStage) {
                return;
            }

            StageInterfaceSummary producerOutputs{};
            StageInterfaceSummary fragmentInputs{};
            ReflectStageInterface(producerStage, false, stages, spirv, producerOutputs, programExternalIndex,
                                  "producer");
            ReflectStageInterface(ShaderStage::Fragment, true, stages, spirv, fragmentInputs, programExternalIndex,
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

        Bool ResolveDirectBuiltInTarget(spvtools::opt::IRContext* context, Uint32 variableId,
                                        BuiltInTypeCheckFn typeCheck, PositionTargetInfo* outTarget) {
            auto* varInst = context->get_def_use_mgr()->GetDef(variableId);
            if (!varInst || varInst->opcode() != spv::Op::OpVariable) return false;
            if (varInst->GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) return false;

            auto* ptrTypeInst = context->get_def_use_mgr()->GetDef(varInst->type_id());
            if (!ptrTypeInst || ptrTypeInst->opcode() != spv::Op::OpTypePointer) return false;
            if (ptrTypeInst->GetSingleWordInOperand(0) != static_cast<Uint32>(spv::StorageClass::Output)) return false;

            PositionTargetInfo target{};
            target.variableId = variableId;
            target.vectorTypeId = ptrTypeInst->GetSingleWordInOperand(1);
            if (!typeCheck(context, target.vectorTypeId, &target.floatTypeId)) return false;
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

        Bool ResolveMemberBuiltInTarget(spvtools::opt::IRContext* context, Uint32 structTypeId, Uint32 memberIndex,
                                        BuiltInTypeCheckFn typeCheck, PositionTargetInfo* outTarget) {
            auto* structInst = context->get_def_use_mgr()->GetDef(structTypeId);
            if (!structInst || structInst->opcode() != spv::Op::OpTypeStruct) return false;
            if (memberIndex >= structInst->NumInOperands()) return false;

            const Uint32 vectorTypeId = structInst->GetSingleWordInOperand(memberIndex);
            Uint32 floatTypeId = 0;
            if (!typeCheck(context, vectorTypeId, &floatTypeId)) return false;

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

        // The OUTPUT variable (or gl_PerVertex member) carrying `builtIn`, if the module
        // declares one of the expected type. Annotations are the search space deliberately:
        // they survive the link-time sanitize chain's interface delisting, which is the whole
        // reason EnsureEntryPointInterface exists.
        Bool FindBuiltInTarget(spvtools::opt::IRContext* context, spv::BuiltIn builtIn,
                               BuiltInTypeCheckFn typeCheck, PositionTargetInfo* outTarget) {
            Vector<Pair<Uint32, Uint32>> memberCandidates;
            constexpr auto kDecorationBuiltIn = static_cast<Uint32>(spv::Decoration::BuiltIn);
            const auto wantedBuiltIn = static_cast<Uint32>(builtIn);

            for (auto& inst : context->module()->annotations()) {
                if (inst.opcode() == spv::Op::OpDecorate) {
                    if (inst.NumInOperands() < 3) continue;
                    if (inst.GetSingleWordInOperand(1) != kDecorationBuiltIn) continue;
                    if (inst.GetSingleWordInOperand(2) != wantedBuiltIn) continue;
                    if (ResolveDirectBuiltInTarget(context, inst.GetSingleWordInOperand(0), typeCheck, outTarget)) {
                        return true;
                    }
                } else if (inst.opcode() == spv::Op::OpMemberDecorate) {
                    if (inst.NumInOperands() < 4) continue;
                    if (inst.GetSingleWordInOperand(2) != kDecorationBuiltIn) continue;
                    if (inst.GetSingleWordInOperand(3) != wantedBuiltIn) continue;
                    memberCandidates.emplace_back(inst.GetSingleWordInOperand(0), inst.GetSingleWordInOperand(1));
                }
            }

            for (const auto& [structTypeId, memberIndex] : memberCandidates) {
                if (ResolveMemberBuiltInTarget(context, structTypeId, memberIndex, typeCheck, outTarget)) return true;
            }
            return false;
        }

        Bool FindPositionTarget(spvtools::opt::IRContext* context, PositionTargetInfo* outTarget) {
            return FindBuiltInTarget(context, spv::BuiltIn::Position, IsVec4Float32, outTarget);
        }

        // Put `variableId` back on `entryPoint`'s interface list if it is not already there.
        //
        // SPIR-V requires every Input/Output global an entry point statically uses to be listed on
        // its OpEntryPoint, and spirv-val enforces it ("Interface variable id <N> is used by entry
        // point 'main' id <M>, but is not listed as an interface"). The link-time sanitize chain
        // DELISTS a variable nothing referenced yet - ShaderCompiler::SanitizeAndOptimizeBinary
        // runs CreateAggressiveDCEPass(false), which may never delete an Output, followed by
        // CreateRemoveUnusedInterfaceVariablesPass, which rebuilds the operand list from the
        // variables actually referenced. A TES that redeclares `out gl_PerVertex { vec4
        // gl_Position; }` and never writes it therefore reaches the backend with the OpVariable
        // and its BuiltIn Position decoration intact and its interface slot gone. Any pass that
        // then injects a reference has to put the slot back, or it hands the driver a module no
        // validator accepts - and Mali r54 answers that with a SIGSEGV inside pipeline creation
        // rather than an error return.
        //
        // No SPIR-V version gate here, unlike GlFragCoordYFlipPass's identical call for its
        // injected PRIVATE global: Input and Output belong on the interface in every version,
        // and only 1.4 widened it to the other storage classes.
        Bool EnsureEntryPointInterface(spvtools::opt::IRContext* context, spvtools::opt::Instruction& entryPoint,
                                       Uint32 variableId) {
            // In-operands: 0 = execution model, 1 = entry function id, 2 = name, 3.. = interface.
            constexpr Uint32 kFirstInterfaceOperand = 3;
            if (variableId == 0) return false;
            for (Uint32 operand = kFirstInterfaceOperand; operand < entryPoint.NumInOperands(); ++operand) {
                if (entryPoint.GetSingleWordInOperand(operand) == variableId) return false;
            }
            entryPoint.AddOperand({SPV_OPERAND_TYPE_ID, {variableId}});
            context->AnalyzeUses(&entryPoint);
            return true;
        }

        // Is `pointerId` the position target itself, or an access chain rooted at it?
        Bool PointerReachesPositionTarget(spvtools::opt::IRContext* context, Uint32 pointerId,
                                          const PositionTargetInfo& target) {
            auto* defUse = context->get_def_use_mgr();
            for (Uint32 current = pointerId; current != 0;) {
                if (current == target.variableId) return true;
                const auto* inst = defUse->GetDef(current);
                if (inst == nullptr) return false;
                switch (inst->opcode()) {
                case spv::Op::OpAccessChain:
                case spv::Op::OpInBoundsAccessChain:
                case spv::Op::OpPtrAccessChain:
                case spv::Op::OpInBoundsPtrAccessChain:
                case spv::Op::OpCopyObject:
                    current = inst->GetSingleWordInOperand(0);
                    break;
                default:
                    return false;
                }
            }
            return false;
        }

        // Does anything in the module write the position target?
        //
        // Deliberately conservative - it answers "assume yes" for every shape it cannot read
        // exactly, because a false "no" would silently drop the clip-space fixup from a shader
        // that does write gl_Position, while a false "yes" only reinstates the behaviour this
        // pass has always had. Scans every function rather than just the entry point's: a shader
        // that assigns gl_Position inside a helper is still a shader that writes it, and passing
        // the pointer to a call is a write as far as this can tell.
        Bool ModuleWritesPositionTarget(spvtools::opt::IRContext* context, const PositionTargetInfo& target) {
            for (auto& function : *context->module()) {
                for (auto& block : function) {
                    for (const auto& inst : block) {
                        switch (inst.opcode()) {
                        case spv::Op::OpStore:
                        case spv::Op::OpCopyMemory:
                        case spv::Op::OpCopyMemorySized:
                            if (PointerReachesPositionTarget(context, inst.GetSingleWordInOperand(0), target)) {
                                return true;
                            }
                            break;
                        case spv::Op::OpFunctionCall:
                            // In-operand 0 is the callee; the rest are arguments.
                            for (Uint32 argument = 1; argument < inst.NumInOperands(); ++argument) {
                                if (PointerReachesPositionTarget(context, inst.GetSingleWordInOperand(argument),
                                                                 target)) {
                                    return true;
                                }
                            }
                            break;
                        default:
                            break;
                        }
                    }
                }
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

                // Nothing to remap in a Position the shader never writes. Declining is not just
                // an optimisation: the fixup is load-modify-store, so on an unwritten Position it
                // converts "undefined, never written" into "written with whatever the load
                // returned", and the store is a reference to a variable the link-time sanitize
                // chain has already delisted from the entry-point interface. glslang emits the
                // OpVariable for every DECLARED interface block, so a redeclared-but-unwritten
                // `out gl_PerVertex` is a shape real shaders have.
                if (!ModuleWritesPositionTarget(context(), target)) {
                    MGLOG_D("gl-to-vulkan-position-fix: the shader never writes gl_Position; leaving it alone");
                    return Status::SuccessWithoutChange;
                }

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

                    Bool modifiedThisEntryPoint = false;
                    for (auto& bb : *function) {
                        for (auto instIter = bb.begin(); instIter != bb.end(); ++instIter) {
                            auto* inst = &*instIter;
                            const Bool needsFixup =
                                (model == spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpEmitVertex) ||
                                (model != spv::ExecutionModel::Geometry && inst->opcode() == spv::Op::OpReturn);
                            if (!needsFixup) continue;

                            modifiedThisEntryPoint |=
                                InsertPositionFixup(context(), inst, target, halfConstId, doYFlip, doZRemap,
                                                    doSurfaceRotate90, doSurfaceRotate180, doSurfaceRotate270);
                        }
                    }
                    // Per entry point, and only for one this pass actually injected into: the
                    // injected load/store is a static use of the position variable, so the
                    // variable has to be on THIS entry point's interface list.
                    if (modifiedThisEntryPoint) {
                        EnsureEntryPointInterface(context(), entryPoint, target.variableId);
                    }
                    modified |= modifiedThisEntryPoint;
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
                // gl_PointSize is a gl_PerVertex MEMBER, never a variable of its own, so the
                // debug-name lookup below can never resolve it - it used to fall through to
                // "no SPIR-V variable named 'gl_PointSize'" and leave the frontend's reserved
                // slot unwritten, or, when it was the only capture, leave the module with no
                // Xfb execution mode at all and the whole span declined.
                Bool needsPointSizeMirror = false;
                Uint32 pointSizeBufferIndex = 0;
                Uint32 pointSizeOffset = 0;
                for (const auto& varying : m_varyings) {
                    if (varying.name == "gl_Position") {
                        needsPositionMirror = true;
                        positionBufferIndex = varying.bufferIndex;
                        positionOffset = varying.offsetBytes;
                        continue;
                    }
                    if (varying.name == "gl_PointSize") {
                        needsPointSizeMirror = true;
                        pointSizeBufferIndex = varying.bufferIndex;
                        pointSizeOffset = varying.offsetBytes;
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
                            MGLOG_D("XfbCaptureDecoratePass: block member %u of type %%%u is captured with a "
                                    "non-contiguous element set; the capture layout will differ from GL's",
                                    key.second, key.first);
                            break;
                        }
                    }
                    decorateMemberForXfb(key.first, key.second, group.bufferIndex, group.minOffset);
                    modified = true;
                }

                if (needsPositionMirror) {
                    modified |= MirrorPerVertexBuiltInForCapture(entryFunctionId, *entryPoint,
                                                                 spv::BuiltIn::Position, IsVec4Float32,
                                                                 "gl_Position", positionBufferIndex, positionOffset,
                                                                 decorateForXfb);
                }
                if (needsPointSizeMirror) {
                    modified |= MirrorPerVertexBuiltInForCapture(entryFunctionId, *entryPoint,
                                                                 spv::BuiltIn::PointSize, IsFloat32Scalar,
                                                                 "gl_PointSize", pointSizeBufferIndex,
                                                                 pointSizeOffset, decorateForXfb);
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

            // gl_Position and gl_PointSize are captured the same way and differ only in which
            // built-in is looked up and what type it has, so one injector serves both. Anything
            // else in gl_PerVertex would need its own type check before it could be added here.
            template <typename DecorateFn>
            Bool MirrorPerVertexBuiltInForCapture(Uint32 entryFunctionId, spvtools::opt::Instruction& entryPoint,
                                                  spv::BuiltIn builtIn, BuiltInTypeCheckFn typeCheck,
                                                  const char* glslName, Uint32 bufferIndex, Uint32 offsetBytes,
                                                  const DecorateFn& decorateForXfb) {
                const Uint32 entryPointModel = entryPoint.GetSingleWordInOperand(0);
                using namespace spvtools::opt;
                PositionTargetInfo target{};
                if (!FindBuiltInTarget(context(), builtIn, typeCheck, &target)) {
                    MGLOG_E("XfbCaptureDecoratePass: %s capture requested but no such output found", glslName);
                    return false;
                }
                if (!target.isMember) {
                    // Standalone built-in variable: decorate it directly. It still has to be
                    // on the interface - a transform-feedback decoration on a variable the entry
                    // point does not list captures nothing, and the sanitize chain delists an
                    // unwritten one (see EnsureEntryPointInterface).
                    decorateForXfb(target.variableId, bufferIndex, offsetBytes);
                    EnsureEntryPointInterface(context(), entryPoint, target.variableId);
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
                // The mirror was listed on the entry point above, but the loop just added a READ
                // of the SOURCE block through an access chain, and the interface rule covers
                // reads exactly as it covers writes. A built-in capture on a shader whose
                // block the sanitize chain delisted - a TES that redeclares `out gl_PerVertex`
                // and never writes it, which is what the tessellation_control_to_tessellation_
                // evaluation.gl_MaxPatchVertices_Position_PointSize bodies do - produced an
                // invalid module here for the same reason the position fixup did.
                if (injected) {
                    EnsureEntryPointInterface(context(), entryPoint, target.variableId);
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

        ShaderStage PickClipFixupStage(const Vector<ShaderStage>& stages) {
            Bool hasGeometry = false;
            Bool hasTessEval = false;
            Bool hasVertex = false;

            for (const ShaderStage stage : stages) {
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
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                return ProgramFactory::DescriptorBindingKind::StorageTexelBuffer;
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
                kind == ProgramFactory::DescriptorBindingKind::StorageTexelBuffer ||
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
                    // A descriptor ARRAY occupies one binding with descriptorCount = N, and is
                    // supported for exactly the kinds that have a per-element resolve path in
                    // UniformManager::BindProgramUniformBuffers: UBO instance arrays
                    // (uniform Block {...} b[N];), storage-block instance arrays, image uniform
                    // arrays, and combined-image-sampler arrays (uniform sampler2D s[N];).
                    // Anything else - the two TEXEL buffer kinds are what remain, samplerBuffer[N]
                    // and imageBuffer[N] - must fail program creation cleanly rather than continue
                    // with corrupt state. Their per-draw path writes pTexelBufferView as the
                    // address of a vector element sized for one descriptor per binding, so an
                    // array would not merely be unresolved, it would dangle.
                    //
                    // Getting listed here is not cosmetic: a kind that is rejected leaves
                    // GetOrCreateProgram's MOBILEGL_ASSERT(remapOk) as the only complaint, and
                    // that assert compiles out above DEBUG - so a release build SILENTLY kept
                    // glslang's per-stage auto-mapped binding numbers, skipping the cross-stage
                    // unification and the set->0 normalisation this function exists to do. A
                    // program with an image array plus any second descriptor got aliased
                    // bindings out of that, and a DEBUG build trapped on the same program.
                    // The refusal below is MGLOG_E and per-program-compile, so it reports every
                    // program it declines. It spent time at MGLOG_I because the old level
                    // ordering compiled E out of the builds that ship.
                    const Bool arraySupportedForKind =
                        kind == ProgramFactory::DescriptorBindingKind::UniformBufferDynamic ||
                        kind == ProgramFactory::DescriptorBindingKind::StorageBuffer ||
                        kind == ProgramFactory::DescriptorBindingKind::StorageImage ||
                        kind == ProgramFactory::DescriptorBindingKind::CombinedImageSampler;
                    if (binding->count != 1 && !arraySupportedForKind) {
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
        return ReflectedDeclaresInputBuiltin(reflectModule, SpvBuiltInInstanceIndex);
    }

    // GL's gl_BaseVertex and Vulkan's BaseVertex agree for indexed draws and disagree for every
    // other command, so a program declaring the builtin needs the ZeroBaseVertex variant when a
    // non-indexed draw uses it (see CompileOptionBit::ZeroBaseVertex). "Declares" rather than
    // "reads" is the honest word and the useful one: the zeroing pass keeps the variable, so
    // both variants of a program answer this question identically.
    Bool ProgramFactory::ReflectedReadsBaseVertexBuiltin(const SpvReflectShaderModule& reflectModule) {
        return ReflectedDeclaresInputBuiltin(reflectModule, SpvBuiltInBaseVertex);
    }

    // gl_ViewportIndex on the last pre-rasterization stage. glslang emits it natively for Vulkan
    // (BuiltIn ViewportIndex plus OpCapability MultiViewport), and nothing in the SpirvPasses
    // chain touches it, so a plain reflection of the declared output builtins is the whole test.
    Bool ProgramFactory::ReflectedWritesViewportIndexBuiltin(const SpvReflectShaderModule& reflectModule) {
        return ReflectedDeclaresOutputBuiltin(reflectModule, SpvBuiltInViewportIndex);
    }

    Bool ProgramFactory::ReflectedDeclaresOutputBuiltin(const SpvReflectShaderModule& reflectModule,
                                                       SpvBuiltIn builtin) {
        for (Uint32 entryIndex = 0; entryIndex < reflectModule.entry_point_count; ++entryIndex) {
            const SpvReflectEntryPoint& entryPoint = reflectModule.entry_points[entryIndex];
            for (Uint32 variableIndex = 0; variableIndex < entryPoint.output_variable_count; ++variableIndex) {
                const SpvReflectInterfaceVariable* variable = entryPoint.output_variables[variableIndex];
                if (variable != nullptr &&
                    (variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0 &&
                    variable->built_in == builtin) {
                    return true;
                }
            }
        }
        return false;
    }

    Bool ProgramFactory::ReflectedDeclaresInputBuiltin(const SpvReflectShaderModule& reflectModule,
                                                       SpvBuiltIn builtin) {
        for (Uint32 entryIndex = 0; entryIndex < reflectModule.entry_point_count; ++entryIndex) {
            const SpvReflectEntryPoint& entryPoint = reflectModule.entry_points[entryIndex];
            for (Uint32 variableIndex = 0; variableIndex < entryPoint.input_variable_count; ++variableIndex) {
                const SpvReflectInterfaceVariable* variable = entryPoint.input_variables[variableIndex];
                if (variable != nullptr &&
                    (variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0 &&
                    variable->built_in == builtin) {
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
        // A2**B**10G10R10, matching MGToVk::ConvertTextureInternalFormatToVkFormat's RGB10A2.
        // This value becomes the storage image VIEW's format while the image itself was created
        // from the texture's internal format, so the two must name the same bit layout or the
        // shader reads the texel through a different component order than the host wrote it.
        // GL_RGB10_A2 with GL_UNSIGNED_INT_2_10_10_10_REV puts R in bits 0-9, G in 10-19, B in
        // 20-29 and A in 30-31, which is Vulkan's A2B10G10R10; A2R10G10B10 transposes R and B.
        // KHR-GL43.shader_image_load_store.basic-allFormats-store read back [2,1,0,3] for an
        // rgb10_a2ui image stored as [0,1,2,3] while these two converters disagreed.
        case SpvImageFormatRgb10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
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
        case SpvImageFormatRgb10a2ui: return VK_FORMAT_A2B10G10R10_UINT_PACK32; // see Rgb10A2 above
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

    SamplerNumericDomain ProgramFactory::UniformTypeToImageNumericDomain(GLenum glType) {
        switch (glType) {
        case GL_INT_IMAGE_1D:
        case GL_INT_IMAGE_2D:
        case GL_INT_IMAGE_3D:
        case GL_INT_IMAGE_2D_RECT:
        case GL_INT_IMAGE_CUBE:
        case GL_INT_IMAGE_BUFFER:
        case GL_INT_IMAGE_1D_ARRAY:
        case GL_INT_IMAGE_2D_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_2D_MULTISAMPLE:
        case GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
            return SamplerNumericDomain::SignedInteger;
        case GL_UNSIGNED_INT_IMAGE_1D:
        case GL_UNSIGNED_INT_IMAGE_2D:
        case GL_UNSIGNED_INT_IMAGE_3D:
        case GL_UNSIGNED_INT_IMAGE_2D_RECT:
        case GL_UNSIGNED_INT_IMAGE_CUBE:
        case GL_UNSIGNED_INT_IMAGE_BUFFER:
        case GL_UNSIGNED_INT_IMAGE_1D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE:
        case GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY:
            return SamplerNumericDomain::UnsignedInteger;
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
        case GL_SAMPLER_CUBE_MAP_ARRAY:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
        case GL_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY:
        case GL_IMAGE_CUBE_MAP_ARRAY:
        case GL_INT_IMAGE_CUBE_MAP_ARRAY:
        case GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY:
            return TextureTarget::TextureCubeMapArray;
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

    void ProgramFactory::ReflectVertexInputs(const Vector<ShaderStage>& stages,
                                             const Vector<Vector<Uint>>& spirv,
                                             VkProgramObject& entry) const {
        entry.activeVertexInputLocationMask = 0;
        entry.vertexInputTypes.fill(0);
        entry.readsBaseVertexBuiltin = false;

        for (SizeT moduleIndex = 0; moduleIndex < stages.size() && moduleIndex < spirv.size(); ++moduleIndex) {
            if (stages[moduleIndex] != ShaderStage::Vertex) {
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

            entry.readsBaseVertexBuiltin = ReflectedReadsBaseVertexBuiltin(reflectModule);

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

    // Which pre-rasterization stage assigns gl_ViewportIndex is not fixed: GL 4.1 allows only the
    // geometry stage, ARB_shader_viewport_layer_array/GL 4.6 also the vertex and tessellation
    // evaluation stages. Rather than guess which one is last, every non-fragment, non-compute
    // module is asked - one writer anywhere means this program's draws need a multi-viewport
    // pipeline, and a false positive costs only a wider viewportCount.
    void ProgramFactory::ReflectViewportIndexUsage(const Vector<ShaderStage>& stages,
                                                   const Vector<Vector<Uint>>& spirv,
                                                   VkProgramObject& entry) const {
        entry.writesViewportIndexBuiltin = false;

        for (SizeT moduleIndex = 0; moduleIndex < stages.size() && moduleIndex < spirv.size(); ++moduleIndex) {
            const ShaderStage stage = stages[moduleIndex];
            if (stage == ShaderStage::Fragment || stage == ShaderStage::Compute) continue;

            const auto& module = spirv[moduleIndex];
            if (module.empty()) continue;

            SpvReflectShaderModule reflectModule{};
            const SpvReflectResult createResult =
                spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
            if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
                // Fail toward the wide pipeline. Missing a real gl_ViewportIndex writer would
                // silently collapse every viewport onto 0 (the exact bug this reflection exists
                // to fix); over-declaring costs one extra viewport slot on a program that never
                // uses it.
                MGLOG_E_ONCE("ProgramFactory::ReflectViewportIndexUsage: reflection failed (result=%d); assuming the "
                             "program writes gl_ViewportIndex",
                             static_cast<Int>(createResult));
                entry.writesViewportIndexBuiltin = true;
                continue;
            }

            if (ReflectedWritesViewportIndexBuiltin(reflectModule)) {
                entry.writesViewportIndexBuiltin = true;
            }
            spvReflectDestroyShaderModule(&reflectModule);
        }
    }

    void ProgramFactory::ReflectFragmentOutputs(const Vector<ShaderStage>& stages,
                                                const Vector<Vector<Uint>>& spirv,
                                                VkProgramObject& entry) const {
        entry.activeFragmentOutputLocationMask = 0;
        entry.fragmentOutputTypes.fill(0);
        entry.fragmentReplacesDepth = false;

        for (SizeT moduleIndex = 0; moduleIndex < stages.size() && moduleIndex < spirv.size(); ++moduleIndex) {
            if (stages[moduleIndex] != ShaderStage::Fragment) {
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

    // How many descriptors to declare for an ARRAY of opaque uniforms (samplers, images) at one
    // binding. A returned count is always DECLARED in the descriptor set layout; `outDeclined`
    // says whether the binding can also be RESOLVED at draw time, or whether the program has to
    // be refused instead.
    //
    // Those are deliberately two different things. The layout must keep describing what the
    // shader declares even for a binding MobileGL cannot resolve: a descriptor the shader reads
    // and the layout omits is not a missing draw, it is an undefined descriptor access, and
    // lavapipe segfaults on it inside pipeline creation - in a JIT worker thread, before any
    // draw runs, which is why removing the binding produced a flaky crash rather than a clean
    // refusal. Declining is done by refusing the draw (VkProgramObject::declinedDescriptors),
    // not by shrinking the layout.
    //
    // Two separate things have to hold, and neither is checkable from the SPIR-V alone:
    //
    //  * the count has to fit a VkDescriptorSetLayoutBinding this device will accept, and fit
    //    the Uint16 it is stored in (65536 would narrow to 0) and the scratch the bind path
    //    reserves from it;
    //  * the frontend reflection has to have RESERVED that many consecutive uniform locations
    //    for this uniform, because the per-element resolve paths address element k as
    //    baseLocation + k. SPIRV-Reflect's `count` is the FLATTENED element count, while GL
    //    locations follow the OUTER dimension only (ProgramObject::GetUniformArraySizeByTIndex
    //    answers TType::getOuterArraySize()). For a one-dimensional array the two agree; for
    //    `uniform sampler2D g[2][3]` SPIR-V says 6 where the reflection reserved 2, and
    //    elements 2..5 would silently resolve onto whichever uniform got the next locations.
    //
    // Asking the reflection whether baseLocation and baseLocation + count - 1 are slots of the
    // SAME uniform tests exactly that precondition, without this code having to model how
    // glslang chooses to lay an array of arrays out.
    //
    // That is NOT on its own enough to start supporting the shape, though, and this check must
    // not be relaxed alone: the binding-qualifier unit seeding in ProgramLinkTask looks an
    // opaque uniform up by its name minus a trailing "[0]", so `goku[0][0]` misses the `goku`
    // key and every element of an array of arrays seeds texture unit 0. Resolving those elements
    // would then paint silently-wrong pixels with no diagnostic at all - strictly worse than
    // declining. The decline goes away together with the seeding fix, not before it.
    static Uint32 DescriptorCountForOpaqueUniformArray(const MG_State::GLState::ProgramObject& program,
                                                       const String& uniformName, Uint32 binding, Int baseLocation,
                                                       Uint32 reflectedCount, Uint32 maxBindings,
                                                       const char* kindLabel, Bool& outDeclined) {
        const Uint32 count = std::max<Uint32>(1u, reflectedCount);
        if (count == 1) {
            return 1u;
        }
        if (count > maxBindings) {
            // Nothing legal to declare: the count would not fit a VkDescriptorSetLayoutBinding
            // this device accepts, and it would narrow badly into the Uint16 that carries it
            // (65536 becomes 0). Unlike the extent case below, this one CANNOT keep the layout
            // consistent with the shader, so refusing the draw does not fully protect it - the
            // driver still JITs a shader indexing past the declared count. Declaring as many as
            // the device allows keeps vkCreateDescriptorSetLayout succeeding and the program
            // inert; a device whose binding cap is smaller than a shader's array is not a
            // configuration MobileGL can serve at all. Needs a >maxBindings-element array to
            // reach (256 on desktop, ~16 on mobile).
            MGLOG_D("ProgramFactory::ReflectLayout: %s array '%s' at binding %u has %u elements, past the %u "
                    "this device can describe - declining the program",
                    kindLabel, uniformName.c_str(), binding, count, maxBindings);
            outDeclined = true;
            return maxBindings;
        }
        if (baseLocation < 0 ||
            !program.UniformLocationsAliasSameUniform(baseLocation, baseLocation + static_cast<Int>(count - 1u))) {
            MGLOG_D("ProgramFactory::ReflectLayout: %s array '%s' at binding %u spans %u descriptors but the "
                    "reflection reserved fewer uniform locations for it (base=%d) - a multi-dimensional array "
                    "is the usual cause, and MobileGL declines it rather than resolve elements onto a "
                    "neighbouring uniform",
                    kindLabel, uniformName.c_str(), binding, count, baseLocation);
            outDeclined = true;
        }
        return count;
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
        entry.declinedDescriptors = false;

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
                    descriptorKind != DescriptorBindingKind::StorageTexelBuffer &&
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
                    //
                    // Bounds-checked like every other array kind. The EXTENT rule differs - a
                    // block array's elements take consecutive GL binding points rather than
                    // consecutive uniform locations, so DescriptorCountForOpaqueUniformArray's
                    // location test does not apply here - but the size rule is identical: this
                    // count goes straight into a VkDescriptorSetLayoutBinding and is narrowed to
                    // a Uint16 on the way, where 65536 would silently become 0.
                    const Uint32 storageArrayCount = std::max<Uint32>(1u, sampler->count);
                    if (storageArrayCount > m_maxBindings) {
                        MGLOG_D("ProgramFactory::ReflectLayout: storage block array '%s' at binding %u has %u "
                                "elements, past the %u this device can describe - declining the program",
                                uniformName.c_str(), binding, storageArrayCount, m_maxBindings);
                        entry.declinedDescriptors = true;
                        entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(m_maxBindings);
                        continue;
                    }
                    entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(storageArrayCount);
                    continue;
                }

                const Int location = program.GetUniformLocation(uniformName);
                if (location < 0) {
                    // A uniform with no location is ordinarily one GL never made active, and
                    // dropping it is routine. An ARRAY reaching here is not routine: it is the
                    // multi-dimensional case. `uniform sampler2D g[2][3]` arrives from
                    // SPIRV-Reflect as one binding of 6 descriptors named "g", while the frontend
                    // reflection keys an array of arrays by its full "[0]"-terminated spelling
                    // ("g[0][0]"), so no base location resolves and the per-element paths have
                    // nothing to count from. Declining is the honest answer - but it has to SAY
                    // so at a level that survives a release build, because dropping the binding
                    // leaves the shader reading a descriptor the layout never declared.
                    if (sampler->count > 1) {
                        MGLOG_E("ProgramFactory::ReflectLayout: declining '%s' at binding %u - a %u-element "
                                "descriptor array with no frontend uniform location (a multi-dimensional array "
                                "of samplers or images is the known cause)",
                                uniformName.c_str(), binding, sampler->count);
                        entry.declinedDescriptors = true;
                        // Declared, not resolved - see DescriptorCountForOpaqueUniformArray for
                        // why the layout keeps describing a binding the draw path will refuse.
                        entry.bindingDescriptorCounts[binding] =
                            static_cast<Uint16>(std::min<Uint32>(sampler->count, m_maxBindings));
                        continue;
                    }
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
                    // Bounds- and extent-checked like the UBO array path above; see
                    // DescriptorCountForOpaqueUniformArray for what "declined" costs and why
                    // the reflection's reserved extent - not SPIRV-Reflect's flattened count -
                    // is what the per-element resolve can actually address.
                    const Uint32 imageArrayCount =
                        DescriptorCountForOpaqueUniformArray(program, uniformName, binding, location, sampler->count,
                                                             m_maxBindings, "image", entry.declinedDescriptors);
                    entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(imageArrayCount);

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

                if (descriptorKind == DescriptorBindingKind::StorageTexelBuffer) {
                    // Only the declared format is recorded, and only so the per-draw resolve can
                    // prefer it over the one glBindImageTexture named. Everything the StorageImage
                    // branch above does about ARRAYS is deliberately absent: an imageBuffer array
                    // is refused outright by the array gate in RemapDescriptorBindingsForVulkan,
                    // exactly as a samplerBuffer array is, so bindingDescriptorCounts stays at the
                    // default 1 and the descriptor write below may take the address of a vector
                    // element without reserving room for extra elements.
                    const VkFormat reflectedFormat =
                        ConvertSpirvImageFormatToVkFormat(sampler->image.image_format);
                    VkFormat& existingFormat = entry.storageImageFormatByBinding[binding];
                    MOBILEGL_ASSERT(existingFormat == VK_FORMAT_UNDEFINED ||
                                        reflectedFormat == VK_FORMAT_UNDEFINED ||
                                        existingFormat == reflectedFormat,
                                    "ProgramFactory::ReflectLayout: storage texel buffer binding %u ('%s') "
                                    "has conflicting reflected formats (%d vs %d)",
                                    binding, uniformName.c_str(), static_cast<Int>(existingFormat),
                                    static_cast<Int>(reflectedFormat));
                    if (existingFormat == VK_FORMAT_UNDEFINED) {
                        existingFormat = reflectedFormat;
                    }
                }

                const TextureTarget target = UniformTypeToTextureTarget(uniformType);
                MOBILEGL_ASSERT(target != TextureTarget::Unknown,
                                "ProgramFactory::ReflectLayout: failed to resolve texture target for '%s'",
                                uniformName.c_str());
                if (descriptorKind == DescriptorBindingKind::CombinedImageSampler) {
                    // An ARRAY of sampler uniforms is ONE binding carrying `count` descriptors,
                    // exactly like the image array above, and for the same reason: GLSL 4.20
                    // gives `layout(binding = 1) uniform sampler2D goku[4]` one declaration
                    // spanning texture units 1..4, each element with its own glUniform1i-assigned
                    // unit. Leaving descriptorCount at 1 declared a single-descriptor binding
                    // while the shader indexed descriptors 1..3 of it, and the bind path wrote
                    // only element 0 - so elements 1..N read a descriptor nobody had written
                    // (KHR-GL42.shading_language_420pack.binding_sampler_array; lavapipe faults
                    // inside the JIT-ed shader rather than reporting).
                    const Uint32 samplerArrayCount =
                        DescriptorCountForOpaqueUniformArray(program, uniformName, binding, location, sampler->count,
                                                             m_maxBindings, "sampler", entry.declinedDescriptors);
                    entry.bindingDescriptorCounts[binding] = static_cast<Uint16>(samplerArrayCount);

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
                // Every other opaque kind records its domain too. Only the combined-image-sampler
                // path above needs it to pick a sampled view format; the three below need it to
                // describe the descriptor a binding gets when its unit is UNBOUND, which is legal
                // GL and must not lose the draw (see UniformManager's Resolve*Descriptor). Left
                // Unknown, those placeholders would have no way to tell a `samplerBuffer` from a
                // `usamplerBuffer` - and a texel buffer view whose numeric type disagrees with the
                // shader's is invalid Vulkan, not merely wrong data.
                if (descriptorKind == DescriptorBindingKind::UniformTexelBuffer ||
                    descriptorKind == DescriptorBindingKind::StorageTexelBuffer ||
                    descriptorKind == DescriptorBindingKind::StorageImage) {
                    const SamplerNumericDomain opaqueDomain =
                        descriptorKind == DescriptorBindingKind::UniformTexelBuffer
                            ? UniformTypeToSamplerNumericDomain(uniformType)
                            : UniformTypeToImageNumericDomain(uniformType);
                    MOBILEGL_ASSERT(opaqueDomain != SamplerNumericDomain::Unknown,
                                    "ProgramFactory::ReflectLayout: failed to resolve numeric domain for '%s' "
                                    "(uniformType=0x%x)",
                                    uniformName.c_str(), uniformType);
                    MOBILEGL_ASSERT(entry.samplerNumericDomainByBinding[binding] ==
                                            SamplerNumericDomain::Unknown ||
                                        entry.samplerNumericDomainByBinding[binding] == opaqueDomain,
                                    "ProgramFactory::ReflectLayout: binding %u ('%s') has conflicting numeric "
                                    "domains (%d vs %d)",
                                    binding, uniformName.c_str(),
                                    static_cast<Int>(entry.samplerNumericDomainByBinding[binding]),
                                    static_cast<Int>(opaqueDomain));
                    entry.samplerNumericDomainByBinding[binding] = opaqueDomain;
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
            } else if (kind == DescriptorBindingKind::StorageTexelBuffer) {
                layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
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

        // UPDATE_AFTER_BIND is strictly an optional per-layout acceleration. The GL
        // descriptor model still resolves every sampler uniform element independently
        // (including its texture-unit sampler-object override); selecting this path
        // changes neither that resolution nor the set versioning in UniformManager.
        // A conservative count keeps a layout on ordinary descriptors whenever any
        // relevant update-after-bind limit is not large enough, rather than asking a
        // driver to reject it during vkCreateDescriptorSetLayout.
        Uint32 updateAfterBindSamplers = 0;
        Uint32 updateAfterBindUniformBuffers = 0;
        Uint32 updateAfterBindStorageBuffers = 0;
        Uint32 updateAfterBindSampledImages = 0;
        Uint32 updateAfterBindStorageImages = 0;
        for (Uint32 binding = 0; binding < m_maxBindings; ++binding) {
            const Uint32 count = entry.bindingDescriptorCounts[binding];
            switch (entry.bindingKinds[binding]) {
                case DescriptorBindingKind::UniformBufferDynamic:
                    updateAfterBindUniformBuffers += count;
                    break;
                case DescriptorBindingKind::CombinedImageSampler:
                    updateAfterBindSamplers += count;
                    updateAfterBindSampledImages += count;
                    break;
                case DescriptorBindingKind::UniformTexelBuffer:
                    updateAfterBindSampledImages += count;
                    break;
                case DescriptorBindingKind::StorageBuffer:
                case DescriptorBindingKind::StorageTexelBuffer:
                    updateAfterBindStorageBuffers += count;
                    break;
                case DescriptorBindingKind::StorageImage:
                    updateAfterBindStorageImages += count;
                    break;
                case DescriptorBindingKind::None:
                    break;
            }
        }
        const Uint32 updateAfterBindResources = updateAfterBindUniformBuffers + updateAfterBindStorageBuffers +
                                                updateAfterBindSampledImages + updateAfterBindStorageImages;
        const auto& uab = m_updateAfterBindLimits;
        entry.usesUpdateAfterBind =
            uab.enabled && updateAfterBindSamplers <= uab.maxPerStageSamplers &&
            updateAfterBindUniformBuffers <= uab.maxPerStageUniformBuffers &&
            updateAfterBindStorageBuffers <= uab.maxPerStageStorageBuffers &&
            updateAfterBindSampledImages <= uab.maxPerStageSampledImages &&
            updateAfterBindStorageImages <= uab.maxPerStageStorageImages &&
            updateAfterBindResources <= uab.maxPerStageResources &&
            updateAfterBindSamplers <= uab.maxSetSamplers &&
            updateAfterBindUniformBuffers <= uab.maxSetUniformBuffers &&
            updateAfterBindUniformBuffers <= uab.maxSetUniformBuffersDynamic &&
            updateAfterBindStorageBuffers <= uab.maxSetStorageBuffers &&
            updateAfterBindStorageBuffers <= uab.maxSetStorageBuffersDynamic &&
            updateAfterBindSampledImages <= uab.maxSetSampledImages &&
            updateAfterBindStorageImages <= uab.maxSetStorageImages;

        Vector<VkDescriptorBindingFlags> bindingFlags;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        if (entry.usesUpdateAfterBind) {
            bindingFlags.assign(bindings.size(), VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
            bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
            bindingFlagsInfo.bindingCount = static_cast<Uint32>(bindingFlags.size());
            bindingFlagsInfo.pBindingFlags = bindingFlags.data();
        }

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.flags = entry.usesUpdateAfterBind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
        setLayoutInfo.pNext = entry.usesUpdateAfterBind ? &bindingFlagsInfo : nullptr;
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
        //
        // "The low bits" is load-bearing and was until now only a comment: a flag that reached
        // bit 16 would alias the height and two different variants would share one memo slot.
        static_assert(static_cast<Uint>(CompileOptionBit::ZeroBaseVertex) < (1u << 16),
                      "CompileOptionBit values must stay below bit 16: GetOrCreateProgram packs the "
                      "default-framebuffer height into the high half of the same memo key");
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
        // The EXECUTABLE's stage list, not GetAttachedShaders(): `spirv` is a link artifact with
        // one module per linked stage, while the attach list is live and grows on
        // glAttachShader, which GL 4.6 core 7.3 says does not reach the executable until the
        // next link. Sizing this loop by the attach list therefore ran it past the end of both
        // `spirv` and `moduleSpirvs` for any program attached to after it linked.
        const Vector<ShaderStage> stages = program.GetLinkedShaderStages();
        auto& spirv = program.GetGeneratedSpirv();
        Vector<Vector<Uint>> moduleSpirvs(spirv.size());
        const Bool enableSpirvValidation = program.GetSpirvValidationEnabled();
        // Unconditional now: the two ValidateTransformedSpirv calls below run in every build,
        // not only when the switch is armed, so the validator's static tables have to be pinned
        // against process exit in every build too.
        MG_Util::ShaderTranspiler::ShaderCompiler::PrepareSpirvValidation();

        const ShaderStage fixupStage = PickClipFixupStage(stages);

        // Both lists come from the same Link(), so they agree by construction; the min() is what
        // makes that an assumption this loop does not have to bet the process on.
        const SizeT moduleCount = std::min(stages.size(), spirv.size());
        for (SizeT i = 0; i < moduleCount; ++i) {
            auto& spv = spirv[i];
            if (spv.empty()) continue;

            // Apply position fixup if needed
            if (fixupStage != ShaderStage::Unknown && stages[i] == fixupStage) {
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
                // These two passes INJECT references - a store for the clip fixup, an access
                // chain and a load for the gl_Position capture mirror - and a reference to a
                // variable the link-time sanitize chain delisted from the entry-point interface
                // is invalid SPIR-V that Mali r54 turns into a SIGSEGV inside pipeline creation
                // rather than an error return. EnsureEntryPointInterface keeps them honest; this
                // is the backstop.
                //
                // The fallback UNWINDS ONE PASS AT A TIME, which matters because the two passes
                // are not equally optional. Rewinding straight to `spv` would also throw away the
                // XfbBuffer/XfbStride/Offset decorations, the TransformFeedback capability and the
                // Xfb execution mode - while the renderer decides to call
                // vkCmdBeginTransformFeedbackEXT purely from GL state and never looks at the
                // module. That ships a pipeline whose last pre-rasterization stage has no Xfb mode
                // into a transform-feedback span, violating
                // VUID-vkCmdBeginTransformFeedbackEXT-None-04128 on exactly the driver class this
                // guard exists for. So: try the post-XFB, pre-clip-fixup module first, which keeps
                // capture working and costs only the clip-space remap.
                //
                // Once per program on a cache miss, and only for the single stage that carries the
                // fixups - not per draw and not per module.
                SpirvValidationFailure fixupFailure{};
                if (!ValidateTransformedSpirv(moduleSpirvs[i], stages[i], program.GetExternalIndex(),
                                              &fixupFailure)) {
                    SpirvValidationFailure xfbFailure{};
                    if (fixupInput != &spv &&
                        ValidateTransformedSpirv(*fixupInput, stages[i], program.GetExternalIndex(), &xfbFailure)) {
                        MGLOG_E_ONCE("ProgramFactory: the clip fixup produced an invalid module for program %u "
                                     "stage %d (%s); keeping the capture-decorated one, so this program draws "
                                     "without the clip-space remap",
                                     program.GetExternalIndex(), static_cast<Int>(stages[i]),
                                     fixupFailure.message.c_str());
                        moduleSpirvs[i] = *fixupInput;
                    } else {
                        MGLOG_E_ONCE("ProgramFactory: the clip/XFB fixups produced an invalid module for program %u "
                                     "stage %d (%s); keeping the untransformed one",
                                     program.GetExternalIndex(), static_cast<Int>(stages[i]),
                                     fixupFailure.message.c_str());
                        moduleSpirvs[i] = spv;
                    }
                }
            } else {
                moduleSpirvs[i] = spv;
            }

            if ((flags & ProgramFactory::CompileOptionBit::ExplicitLod0Sampling) && stages[i] == ShaderStage::Fragment) {
                Vector<Uint> explicitLodSpirv;
                if (TransformSpirvForExplicitLod0Sampling(moduleSpirvs[i], explicitLodSpirv)) {
                    moduleSpirvs[i] = Move(explicitLodSpirv);
                }
            }

            if ((flags & ProgramFactory::CompileOptionBit::FragCoordYFlip) && stages[i] == ShaderStage::Fragment) {
                Vector<Uint> fragCoordSpirv;
                if (TransformSpirvForFragCoordYFlip(moduleSpirvs[i], fragCoordSpirv, m_defaultFramebufferHeight)) {
                    moduleSpirvs[i] = Move(fragCoordSpirv);
                }
            }

            // GL_KHR_shader_subgroup handling (SubgroupSupportPolicy.h). Native subgroup
            // operations execute natively; module repairs keep the GL contract intact
            // around them. The opt-in emulation path replaces them only on devices with no
            // subgroup support at all (MOBILEGL_MAGMA_EMULATE_SUBGROUP).
            if (stages[i] == ShaderStage::Compute) {
                // Program 203 broadcasts the first reduction through
                // prefixSumCache[0], then lets the second reduction overwrite that
                // scratch without first rendezvousing all readers. Patch that exact
                // fingerprint before either native or emulated subgroup lowering.
                if (m_subgroupPolicy.fixIterationRPBarrier) {
                    Vector<Uint> patchedSpirv;
                    if (MG_Util::ShaderTranspiler::ShaderCompiler::FixIterationRPBarrierForVulkan(
                            moduleSpirvs[i], patchedSpirv, enableSpirvValidation)) {
                        moduleSpirvs[i] = std::move(patchedSpirv);
                    } else {
                        MGLOG_E("ProgramFactory: iterationRP barrier patch failed for program %u; "
                                "Program 203 keeps its shared-scratch race",
                                program.GetExternalIndex());
                    }
                }
                if (m_subgroupPolicy.emulateSubgroups) {
                    Vector<Uint> emulatedSpirv;
                    if (MG_Util::ShaderTranspiler::ShaderCompiler::EmulateSubgroupsForVulkan(
                            moduleSpirvs[i], emulatedSpirv,
                            m_subgroupPolicy.maxComputeSharedMemoryBytes, enableSpirvValidation)) {
                        moduleSpirvs[i] = std::move(emulatedSpirv);
                    } else {
                        MGLOG_E("ProgramFactory: subgroup emulation failed for program %u; the "
                                "module keeps subgroup operations the device cannot execute",
                                program.GetExternalIndex());
                    }
                } else {
                    // iterationRP under-declares its cross-subgroup scratch
                    // (prefixSumCache[32] for 512 invocations); on a sub-16-lane device
                    // grow that one fingerprinted array to what the topology needs.
                    if (m_subgroupPolicy.fixIterationRPSubgroupScratch) {
                        Vector<Uint> patchedSpirv;
                        if (MG_Util::ShaderTranspiler::ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(
                                moduleSpirvs[i], patchedSpirv, m_subgroupPolicy.nativeSubgroupSize,
                                m_subgroupPolicy.maxComputeSharedMemoryBytes,
                                enableSpirvValidation)) {
                            moduleSpirvs[i] = std::move(patchedSpirv);
                        } else {
                            MGLOG_E("ProgramFactory: iterationRP subgroup scratch patch failed for "
                                    "program %u; the pack's declared array sizes stay in effect",
                                    program.GetExternalIndex());
                        }
                    }
                    // gl_NumSubgroups must agree with the gl_SubgroupID range GL promises;
                    // derive it from the workgroup dimensions and gl_SubgroupSize instead of
                    // trusting a driver builtin that can disagree with the topology the same
                    // dispatch emits (Adreno reports 1 while emitting IDs 0..7 for a
                    // 512-invocation, 64-wide workgroup). The ceil() partition this derives
                    // is pinned by REQUIRE_FULL_SUBGROUPS at pipeline creation whenever the
                    // workgroup shape makes that flag legal (see the stage setup below).
                    if (m_subgroupPolicy.deriveNumSubgroups) {
                        Vector<Uint> derivedNumSubgroupsSpirv;
                        if (MG_Util::ShaderTranspiler::ShaderCompiler::DeriveNumSubgroupsForVulkan(
                                moduleSpirvs[i], derivedNumSubgroupsSpirv, enableSpirvValidation)) {
                            moduleSpirvs[i] = std::move(derivedNumSubgroupsSpirv);
                        } else {
                            MGLOG_E("ProgramFactory: failed to derive gl_NumSubgroups for program %u; "
                                    "compute shaders may observe a driver-inconsistent subgroup count",
                                    program.GetExternalIndex());
                        }
                    }
                }
            }

            // Vulkan's SPIR-V environment has no rectangle image dimension, so a
            // GL_TEXTURE_RECTANGLE lookup has to become the 2D one the texture is really
            // stored as - which addresses [0,1] where the application addressed texels.
            {
                Vector<Uint> rectLoweredSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::LowerRectImages(moduleSpirvs[i], rectLoweredSpirv, enableSpirvValidation) &&
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
                        moduleSpirvs[i], invariantSpirv, enableSpirvValidation)) {
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
            if (stages[i] == ShaderStage::Vertex && m_shaderDrawParametersEnabled) {
                Vector<Uint> rebasedSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::RebaseInstanceIndexForVulkan(moduleSpirvs[i],
                                                                                            rebasedSpirv, enableSpirvValidation)) {
                    moduleSpirvs[i] = std::move(rebasedSpirv);
                } else {
                    MGLOG_E("ProgramFactory: failed to rebase gl_InstanceID for program %u; "
                            "instanced draws with a non-zero baseInstance may render incorrectly",
                            program.GetExternalIndex());
                }
            }

            // The non-indexed variant of a vertex stage that reads gl_BaseVertex: GL wants zero
            // there, Vulkan's builtin would hand it the draw's firstVertex. Requested per draw
            // through CompileOptionBit::ZeroBaseVertex, so the indexed variant of the same
            // program keeps the native builtin and stays correct for glDrawElementsBaseVertex
            // and for the baseVertex word of an indexed indirect command.
            if (stages[i] == ShaderStage::Vertex && (flags & CompileOptionBit::ZeroBaseVertex)) {
                Vector<Uint> zeroedSpirv;
                if (MG_Util::ShaderTranspiler::ShaderCompiler::ZeroBaseVertexForVulkan(moduleSpirvs[i],
                                                                                       zeroedSpirv, enableSpirvValidation)) {
                    moduleSpirvs[i] = std::move(zeroedSpirv);
                } else {
                    // Failing open keeps the native builtin, which is the pre-fix behavior:
                    // gl_BaseVertex reads firstVertex on a DrawArrays instead of zero.
                    MGLOG_E("ProgramFactory: failed to zero gl_BaseVertex for program %u; non-indexed "
                            "draws will read the draw's first vertex from it instead of zero",
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
            if (stages[i] == ShaderStage::Vertex) {
                Vector<Uint> packedSpirv;
                const Bool packOk = MG_Util::ShaderTranspiler::ShaderCompiler::PackDoubleVertexInputsForVulkan(
                    moduleSpirvs[i], packedSpirv, enableSpirvValidation);
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
                        moduleSpirvs[i], unformattedSpirv, enableSpirvValidation)) {
                    moduleSpirvs[i] = std::move(unformattedSpirv);
                } else {
                    MGLOG_E("ProgramFactory: failed to make float storage images unformatted for program %u",
                            program.GetExternalIndex());
                }
            }
        }

        const Bool remapOk = RemapDescriptorBindingsForVulkan(moduleSpirvs, m_maxBindings, moduleSpirvs);
        MOBILEGL_ASSERT(remapOk, "ProgramFactory::GetOrCreateProgram: descriptor binding remap failed");

        for (SizeT i = 0; i < moduleCount; ++i) {
            auto& moduleSpv = moduleSpirvs[i];
            if (moduleSpv.empty()) continue;

            // Last look at the exact bytes the driver receives, in EVERY build rather than only
            // in DEBUG or with MOBILEGL_ENABLE_SPIRV_VALIDATION armed. This one only reports:
            // by here the descriptor bindings have been remapped and the layout about to be
            // reflected describes the remapped module, so there is no module left that is both
            // valid and consistent with it to fall back to. The recovery lives one step earlier,
            // at the clip/XFB fixups (see the revert there) - which is where a transform can
            // introduce a reference to a delisted interface variable, the failure this whole
            // guard exists for. Anything that reaches this line names itself in the log of a
            // shipping build instead of dying anonymously inside the driver.
            SpirvValidationFailure finalFailure{};
            if (!ValidateTransformedSpirv(moduleSpv, stages[i], program.GetExternalIndex(), &finalFailure)) {
                MGLOG_E_ONCE("ProgramFactory: handing vkCreateShaderModule an INVALID module for program %u stage %d - "
                             "a backend transform after the clip/XFB fixups broke it (%s)",
                             program.GetExternalIndex(), static_cast<Int>(stages[i]),
                             finalFailure.message.c_str());
            }

            // Does the stage the driver will treat as the last pre-rasterization one actually
            // carry Xfb? Asked of the FINAL bytes, so it answers for whatever the whole transform
            // chain produced - a rewound clip/XFB backstop, a capture pass that resolved no
            // varying and changed nothing, anything later that might strip it. The renderer picks
            // its capture commands from GL state alone and would otherwise open a span against a
            // pipeline that cannot feed it.
            if (stages[i] == fixupStage && (flags & ProgramFactory::CompileOptionBit::XfbCapture) &&
                program.GetTransformFeedbackVaryingCount() > 0 &&
                !MG_Util::ShaderTranspiler::ShaderCompiler::ModuleDeclaresTransformFeedback(moduleSpv)) {
                MGLOG_E_ONCE("ProgramFactory: program %u was built as a transform-feedback capture variant but its "
                             "stage %d carries no Xfb execution mode; its capture spans will be declined rather "
                             "than recorded against a pipeline that cannot feed them",
                             program.GetExternalIndex(), static_cast<Int>(stages[i]));
                entry.xfbCaptureDeclined = true;
            }

            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = moduleSpv.size() * sizeof(Uint);
            smci.pCode = moduleSpv.data();

            VkShaderModule module = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateShaderModule(m_device, &smci, nullptr, &module), "vkCreateShaderModule");

            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ShaderStage shaderStage = stages[i];
            stage.stage = ToVkStage(shaderStage);
            stage.module = module;
            stage.pName = "main";
            // Pin the full-subgroup launch the derived gl_NumSubgroups assumes. Legal
            // exactly when the computeFullSubgroups feature is enabled and local_size_x is
            // a multiple of the subgroup size (VUID-VkPipelineShaderStageCreateInfo-
            // flags-02759/-02785), and only worth requesting while the resulting subgroup
            // count fits the device's maxComputeWorkgroupSubgroups (lavapipe caps it at
            // 32, below a 512-invocation dispatch's 64). With the bit set, "Full
            // Subgroups" guarantees every subgroup launches with all invocations active,
            // making the subgroup count exactly invocations / size. Shapes the flag
            // cannot cover (e.g. 32x16 on a 64-wide device) fall back to the driver's
            // own - spec-encouraged - tight partitioning, which the DriverPost witness
            // verifies per device.
            if (shaderStage == ShaderStage::Compute && m_subgroupPolicy.requireFullSubgroups &&
                !m_subgroupPolicy.emulateSubgroups && m_subgroupPolicy.nativeSubgroupSize != 0) {
                const ComputeLocalSize localSize = TryGetComputeLocalSize(moduleSpv);
                const Uint64 fullSubgroupCount =
                    localSize.Total() / m_subgroupPolicy.nativeSubgroupSize;
                if (localSize.x != 0 && localSize.x % m_subgroupPolicy.nativeSubgroupSize == 0 &&
                    fullSubgroupCount <= m_subgroupPolicy.maxComputeWorkgroupSubgroups) {
                    stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
                }
            }

            entry.modules.push_back(module);
            entry.stages.push_back(stage);
            entry.stageSpirvDigests.push_back(ShaderStageSpirvDigest{
                static_cast<Uint32>(stage.stage), static_cast<Uint32>(moduleSpv.size()),
                XXH64(moduleSpv.data(), moduleSpv.size() * sizeof(Uint), 0)});
        }

        // Reflect and create layout as part of the program object
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        ValidateRasterizationStageInterface(stages, moduleSpirvs, entry, program.GetExternalIndex());
#endif
        ReflectVertexInputs(stages, moduleSpirvs, entry);
        ReflectViewportIndexUsage(stages, moduleSpirvs, entry);
        ReflectFragmentOutputs(stages, moduleSpirvs, entry);
        ReflectPassthroughTessControlNeed(stages, moduleSpirvs, entry);
        ReflectLayout(program, moduleSpirvs, entry);
        // A failed remap means the modules kept glslang's per-stage auto-mapped binding numbers -
        // no cross-stage unification, no set->0 normalisation - so the bindings this layout
        // describes are not the bindings the shader reads. That has to stop the program from
        // drawing, and until now nothing did: the MOBILEGL_ASSERT above compiles out of every
        // build past DEBUG, and RemapDescriptorBindingsForVulkan's own refusal message said so at
        // a level an INFO build also drops. Declining is the mechanism that already exists for
        // "the layout and the shader disagree", so route it through that. Set AFTER ReflectLayout,
        // which clears the flag.
        if (!remapOk) {
            MGLOG_E("ProgramFactory::GetOrCreateProgram: declining program %u - its descriptor bindings could not "
                    "be remapped, so the layout does not describe what the shader reads",
                    program.GetExternalIndex());
            entry.declinedDescriptors = true;
        }

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
                // The observer destroys dependent pipelines and frees descriptor sets while
                // this entry still owns its layout. Vulkan requires every descriptor set to be
                // freed before its VkDescriptorSetLayout is destroyed.
                if (m_evictionObserver != nullptr) {
                    m_evictionObserver->OnProgramEvicted(hash, descriptorSetLayout);
                }
                ++m_cacheStructureEpoch; // erase moves/kills entries: memoised pointers die
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    ProgramFactory::~ProgramFactory() {
        for (auto& entry : m_passthroughTessControlStages) {
            if (entry.second.module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_device, entry.second.module, nullptr);
            }
        }
    }

    Uint64 ProgramFactory::ComputePassthroughTessControlKey(Uint32 patchVertices,
                                                            const FloatVec4& defaultOuterLevel,
                                                            const FloatVec2& defaultInnerLevel,
                                                            Uint32 perVertexMembers) {
        // A plain 32-byte blob of exactly what the generator reads, hashed once. Deliberately over
        // the RAW BITS rather than the values: two levels that compare unequal must key apart, and
        // a NaN level - which glPatchParameterfv accepts - compares unequal to itself.
        struct Blob {
            Uint32 patchVertices;
            Uint32 outerBits[4];
            Uint32 innerBits[2];
            Uint32 perVertexMembers;
        } blob{};
        blob.patchVertices = patchVertices;
        for (Uint32 i = 0; i < 4; ++i) blob.outerBits[i] = std::bit_cast<Uint32>(defaultOuterLevel[i]);
        for (Uint32 i = 0; i < 2; ++i) blob.innerBits[i] = std::bit_cast<Uint32>(defaultInnerLevel[i]);
        blob.perVertexMembers = perVertexMembers;
        return XXH64(&blob, sizeof(blob), 0);
    }

    // The member list a gl_PerVertex redeclaration must spell, derived from the mask. Order is
    // glslang's declaration order and is load-bearing: a redeclaration whose members are the same
    // set in a different order is a different block.
    static String BuildPerVertexMemberDeclarations(Uint32 perVertexMembers) {
        using Bit = ProgramFactory::PerVertexMemberBit;
        String members;
        if (perVertexMembers & static_cast<Uint32>(Bit::Position)) members += "    vec4 gl_Position;\n";
        if (perVertexMembers & static_cast<Uint32>(Bit::PointSize)) members += "    float gl_PointSize;\n";
        // Sized at one, not left unsized: an unsized built-in array in a redeclared block is
        // implicitly sized by use, and this stage never indexes either distance array.
        if (perVertexMembers & static_cast<Uint32>(Bit::ClipDistance)) members += "    float gl_ClipDistance[1];\n";
        if (perVertexMembers & static_cast<Uint32>(Bit::CullDistance)) members += "    float gl_CullDistance[1];\n";
        return members;
    }

    Uint32 ProgramFactory::ReflectPerVertexInputMembers(const Vector<Uint>& spirv) {
        // Minimal, self-contained SPIR-V walk. SPIRV-Reflect is deliberately NOT used: for an
        // array of interface blocks it reports built_in == -1 on the block and leaves every
        // member's built_in at 0 (which is SpvBuiltInPosition), so a member walk through it reads
        // "Position, Position, Position" - the same trap ReflectPassthroughTessControlNeed
        // documents. The decorations below are unambiguous.
        constexpr SizeT kHeaderWords = 5;
        constexpr Uint32 kOpName = 5;
        constexpr Uint32 kOpDecorate = 71;
        constexpr Uint32 kOpMemberDecorate = 72;
        constexpr Uint32 kOpTypeArray = 28;
        constexpr Uint32 kOpTypePointer = 32;
        constexpr Uint32 kOpVariable = 59;
        constexpr Uint32 kDecorationBlock = 2;
        constexpr Uint32 kDecorationBuiltIn = 11;
        constexpr Uint32 kStorageClassInput = 1;
        constexpr Uint32 kBuiltInPosition = 0;
        constexpr Uint32 kBuiltInPointSize = 1;
        constexpr Uint32 kBuiltInClipDistance = 3;
        constexpr Uint32 kBuiltInCullDistance = 4;
        (void)kOpName;

        if (spirv.size() <= kHeaderWords) return 0;

        UnorderedMap<Uint32, Uint32> arrayElementType;             // array id   -> element type id
        UnorderedMap<Uint32, Pair<Uint32, Uint32>> pointerPointee; // pointer id -> (storage class, pointee)
        UnorderedMap<Uint32, Uint32> structMembers;                // struct id  -> PerVertexMemberBit mask
        std::set<Uint32> blockStructs;
        Vector<Uint32> inputVariablePointerTypes;

        for (SizeT i = kHeaderWords; i < spirv.size();) {
            const Uint32 wordCount = spirv[i] >> 16;
            const Uint32 opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            const Uint32* words = &spirv[i];
            switch (opcode) {
            case kOpTypeArray:
                if (wordCount >= 4) arrayElementType[words[1]] = words[2];
                break;
            case kOpTypePointer:
                if (wordCount >= 4) pointerPointee[words[1]] = {words[2], words[3]};
                break;
            case kOpVariable:
                if (wordCount >= 4 && words[3] == kStorageClassInput) inputVariablePointerTypes.push_back(words[1]);
                break;
            case kOpDecorate:
                if (wordCount >= 3 && words[2] == kDecorationBlock) blockStructs.insert(words[1]);
                break;
            case kOpMemberDecorate:
                if (wordCount >= 5 && words[3] == kDecorationBuiltIn) {
                    Uint32 bit = 0;
                    switch (words[4]) {
                    case kBuiltInPosition: bit = static_cast<Uint32>(PerVertexMemberBit::Position); break;
                    case kBuiltInPointSize: bit = static_cast<Uint32>(PerVertexMemberBit::PointSize); break;
                    case kBuiltInClipDistance: bit = static_cast<Uint32>(PerVertexMemberBit::ClipDistance); break;
                    case kBuiltInCullDistance: bit = static_cast<Uint32>(PerVertexMemberBit::CullDistance); break;
                    default: break;
                    }
                    structMembers[words[1]] |= bit;
                }
                break;
            default:
                break;
            }
            i += wordCount;
        }

        // The one Input variable whose type is an array of a Block-decorated struct IS gl_in;
        // gl_TessCoord and friends are plain scalars/vectors and never match.
        for (const Uint32 pointerType : inputVariablePointerTypes) {
            const auto pointer = pointerPointee.find(pointerType);
            if (pointer == pointerPointee.end()) continue;
            const auto array = arrayElementType.find(pointer->second.second);
            if (array == arrayElementType.end()) continue;
            if (!blockStructs.contains(array->second)) continue;
            const auto members = structMembers.find(array->second);
            if (members == structMembers.end()) continue;
            return members->second;
        }
        return 0;
    }

    String ProgramFactory::BuildPassthroughTessControlSource(Uint32 patchVertices,
                                                             const FloatVec4& defaultOuterLevel,
                                                             const FloatVec2& defaultInnerLevel,
                                                             Uint32 perVertexMembers) {
        // The stage GL 4.6 core 11.2.2 describes when a program has an evaluation shader and no
        // control shader: "the input patch is passed through unmodified", the output patch has
        // as many vertices as the input one (PATCH_VERTICES), and the levels come from the
        // PATCH_DEFAULT_OUTER_LEVEL / PATCH_DEFAULT_INNER_LEVEL state.
        //
        // Those two levels are baked in as literals - Vulkan has no equivalent dynamic state, so
        // compiling them in is the only way to honour glPatchParameterfv. That makes them part of
        // this module's identity: GetOrCreatePassthroughTessControlStage keys its cache on them,
        // and PipelineFactory hashes them into the pipeline key. The three must move together.
        //
        // gl_out carries gl_Position and nothing else on purpose. The evaluation stage that
        // reads it was linked against the VERTEX stage directly, so its input gl_PerVertex holds
        // exactly the built-ins that stage used, and its user-defined inputs (if any) come
        // straight off the vertex stage's outputs - which a control stage sitting in between
        // would leave unwritten. ReflectPassthroughTessControlNeed refuses those programs rather
        // than let this write a partial interface.
        //
        // All four outer levels and both inner levels are written unconditionally: writing a
        // level the evaluation stage's domain does not use is legal and ignored, and it saves
        // this from having to know the domain.
        String source = "#version 450 core\n";
        source += "layout(vertices = " + std::to_string(patchVertices) + ") out;\n";
        // gl_in and gl_out are redeclared to the exact gl_PerVertex the NEIGHBOURING EVALUATION
        // STAGE carries, because Vulkan matches built-in interface blocks by their whole shape,
        // and the two obvious spellings are both wrong:
        //   * narrowing the block to gl_Position alone makes the evaluation stage read a patch of
        //     zeroes (degenerate triangles, nothing rasterized), and
        //   * taking glslang's DEFAULT block for a standalone control stage yields whatever THIS
        //     source's #version implies, which is unrelated to the evaluation stage's.
        //
        // The member set is a PARAMETER rather than a constant, and that is the whole point: it
        // was hardcoded to {gl_Position, gl_PointSize, gl_ClipDistance[1]}, which is the shape a
        // program carries only below #version 450. glslang appends gl_CullDistance to the block
        // from 450 upward, so every 450/460 program - and every ESSL program, which the source
        // processor rewrites to "#version 460 core" - carried FOUR members against this stage's
        // three and got the black-frame-no-error case described above. The mask comes from
        // ReflectPerVertexInputMembers, read off the evaluation stage's own SPIR-V.
        // PassthroughTessControlTest.MatchesTheFrontendPerVertexBlock is the latch, and it now
        // links the program at both 430 and 460.
        //
        // Only gl_Position is written. gl_PointSize is declared but left alone deliberately:
        // writing it from a tessellation stage requires the shaderTessellationAndGeometryPointSize
        // feature, which this renderer does not enable, so a program whose evaluation stage reads
        // gl_in[].gl_PointSize gets an undefined point size instead of the vertex stage's - a gap
        // this trades for not making every tessellated pipeline depend on an optional feature.
        const String perVertexBody = BuildPerVertexMemberDeclarations(perVertexMembers);
        source += "in gl_PerVertex {\n" + perVertexBody + "} gl_in[gl_MaxPatchVertices];\n";
        source += "out gl_PerVertex {\n" + perVertexBody + "} gl_out[];\n";
        source += "void main() {\n";
        source += "    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n";
        for (Uint32 i = 0; i < 4; ++i) {
            source += "    gl_TessLevelOuter[" + std::to_string(i) +
                      "] = " + MG_Util::ShaderTranspiler::TessellationLevelLiteral(defaultOuterLevel[i]) + ";\n";
        }
        for (Uint32 i = 0; i < 2; ++i) {
            source += "    gl_TessLevelInner[" + std::to_string(i) +
                      "] = " + MG_Util::ShaderTranspiler::TessellationLevelLiteral(defaultInnerLevel[i]) + ";\n";
        }
        source += "}\n";
        return source;
    }

    VkPipelineShaderStageCreateInfo ProgramFactory::GetOrCreatePassthroughTessControlStage(
        Uint32 patchVertices, const FloatVec4& defaultOuterLevel, const FloatVec2& defaultInnerLevel,
        Uint32 perVertexMembers) {
        // Everything compiled into the stage, folded into one key. The patch size alone stopped
        // being enough once glPatchParameterfv could change the levels: two modules that differ
        // only in a baked-in level are different modules, and pipelines built from either may be
        // alive at the same time. The gl_PerVertex member set joins it for the same reason - two
        // programs at different GLSL versions need differently-shaped blocks.
        const Uint64 key =
            ComputePassthroughTessControlKey(patchVertices, defaultOuterLevel, defaultInnerLevel, perVertexMembers);
        // A cached VK_NULL_HANDLE is a remembered failure, not a miss: returning it keeps a
        // generator that cannot compile from re-running glslang on every draw.
        const auto cached = m_passthroughTessControlStages.find(key);
        if (cached != m_passthroughTessControlStages.end()) {
            return cached->second;
        }

        // The key stopped being bounded when the levels joined it: patchVertices alone could only
        // take 32 values, but six unclamped application floats can take any number, and an
        // application that ramps a level per frame would retain one VkShaderModule per frame for
        // the lifetime of the device. Flushed wholesale rather than aged: a module is not
        // referenced by the pipelines built from it (Vulkan copies what it needs at
        // vkCreateGraphicsPipelines), everything here runs on the GL thread, and an application
        // that can overflow this cap is already recompiling every frame - so the flush costs it
        // nothing it was not paying anyway.
        if (m_passthroughTessControlStages.size() >= kMaxPassthroughTessControlStages) {
            MGLOG_D("ProgramFactory: flushing %zu pass-through tessellation control stages; the application has "
                    "used more than %zu distinct (patch size, default level) combinations",
                    m_passthroughTessControlStages.size(), kMaxPassthroughTessControlStages);
            for (auto& entry : m_passthroughTessControlStages) {
                if (entry.second.module != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(m_device, entry.second.module, nullptr);
                }
            }
            m_passthroughTessControlStages.clear();
        }

        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        stage.module = VK_NULL_HANDLE;
        stage.pName = "main";

        using namespace MG_Util::ShaderTranspiler;
        const String source =
            BuildPassthroughTessControlSource(patchVertices, defaultOuterLevel, defaultInnerLevel, perVertexMembers);
        // Same compile configuration as every other stage of every other program: this runs on
        // the GL thread (the draw path), so the live compile env is the right one, and flags=0
        // is the Vulkan-targeting form (CompileForOpenGL is what the GLES backend adds).
        const SharedPtr<const CompileEnv>& env = GetCurrentCompileEnv();
        ShaderAttrib shaderAttrib{.shaderType = GL_TESS_CONTROL_SHADER,
                                  .sourceStr = source,
                                  .flags = 0,
                                  .env = env.get()};
        auto compiled = ShaderCompiler::CompileShader(shaderAttrib);
        if (!compiled) {
            MGLOG_E("ProgramFactory: could not compile the pass-through tessellation control stage for "
                    "patchVertices=%u; a program with an evaluation stage and no control stage cannot draw. %s",
                    patchVertices, compiled.error().log.c_str());
            m_passthroughTessControlStages.emplace(key, stage);
            return stage;
        }

        ProgramAttrib programAttrib{};
        programAttrib.shaders.push_back(compiled.value());
        auto linked = ShaderCompiler::LinkProgram(programAttrib);
        if (!linked) {
            MGLOG_E("ProgramFactory: could not link the pass-through tessellation control stage for "
                    "patchVertices=%u. %s", patchVertices, linked.error().log.c_str());
            m_passthroughTessControlStages.emplace(key, stage);
            return stage;
        }

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_TESS_CONTROL_SHADER}, .program = *linked.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binary || binary.value().empty() || binary.value().front().empty()) {
            MGLOG_E("ProgramFactory: could not generate SPIR-V for the pass-through tessellation control stage "
                    "for patchVertices=%u", patchVertices);
            m_passthroughTessControlStages.emplace(key, stage);
            return stage;
        }

        const Vector<Uint>& spirv = binary.value().front();
        {
            // Still switch-gated, unlike the two in GetOrCreateProgram: this stage is synthesized
            // by MobileGL from a fixed template rather than transformed from application SPIR-V,
            // so a failure here is a MobileGL bug to catch in a validating lane, not something a
            // shipping build can be handed by an application. The message is latched all the same
            // - the pass-through cache is keyed on patchVertices, so a broken template would
            // otherwise re-report once per distinct patch size.
            Bool validateThisOne = false;
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
            validateThisOne = true;
#else
            validateThisOne = m_enableSpirvValidation;
            if (validateThisOne) MG_Util::ShaderTranspiler::ShaderCompiler::PrepareSpirvValidation();
#endif
            SpirvValidationFailure passthroughFailure{};
            if (validateThisOne &&
                !ValidateTransformedSpirv(spirv, ShaderStage::TessControl, 0, &passthroughFailure)) {
                MGLOG_E_ONCE("ProgramFactory: the synthesized pass-through tessellation control stage for "
                             "patchVertices=%u does not validate (%s)",
                             patchVertices, passthroughFailure.message.c_str());
            }
        }

        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = spirv.size() * sizeof(Uint);
        smci.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult result = vkCreateShaderModule(m_device, &smci, nullptr, &module);
        if (result != VK_SUCCESS) {
            MGLOG_E("ProgramFactory: vkCreateShaderModule failed (%d) for the pass-through tessellation control "
                    "stage for patchVertices=%u", static_cast<Int>(result), patchVertices);
            m_passthroughTessControlStages.emplace(key, stage);
            return stage;
        }

        stage.module = module;
        MGLOG_D("ProgramFactory: built the pass-through tessellation control stage for patchVertices=%u "
                "(GL 4.6 11.2.2; Vulkan has no fixed-function equivalent)", patchVertices);
        m_passthroughTessControlStages.emplace(key, stage);
        return stage;
    }

    void ProgramFactory::ReflectPassthroughTessControlNeed(
        const Vector<ShaderStage>& stages,
        const Vector<Vector<Uint>>& spirv,
        VkProgramObject& entry) const {
        entry.needsPassthroughTessControl = false;
        entry.passthroughTessControlEmulatable = false;
        entry.passthroughPerVertexMembers = 0;

        Bool hasTessEval = false;
        Bool hasTessControl = false;
        SizeT tessEvalModuleIndex = 0;
        for (SizeT i = 0; i < stages.size(); ++i) {
            const ShaderStage stage = stages[i];
            if (stage == ShaderStage::TessControl) hasTessControl = true;
            if (stage == ShaderStage::TessEval) {
                hasTessEval = true;
                tessEvalModuleIndex = i;
            }
        }
        if (!hasTessEval || hasTessControl) return;

        entry.needsPassthroughTessControl = true;

        if (tessEvalModuleIndex >= spirv.size() || spirv[tessEvalModuleIndex].empty()) return;
        const auto& module = spirv[tessEvalModuleIndex];

        // The shape the synthesized control stage has to redeclare. Read here because this is the
        // only place that holds the evaluation stage's module; a zero mask means the walk found
        // no input per-vertex block at all, in which case the pre-450 shape is the safe stand-in
        // (it is what every program carried before gl_CullDistance joined the block).
        const Uint32 perVertexMembers = ReflectPerVertexInputMembers(module);
        entry.passthroughPerVertexMembers = perVertexMembers != 0 ? perVertexMembers : kDefaultPerVertexMembers;
        if (perVertexMembers == 0) {
            MGLOG_W("ProgramFactory: could not read the evaluation stage's gl_PerVertex block shape; the "
                    "pass-through control stage falls back to the pre-450 three-member form");
        }

        SpvReflectShaderModule reflectModule{};
        const SpvReflectResult createResult =
            spvReflectCreateShaderModule(module.size() * sizeof(Uint), module.data(), &reflectModule);
        if (createResult != SPV_REFLECT_RESULT_SUCCESS) {
            MGLOG_E("ProgramFactory::ReflectPassthroughTessControlNeed: reflection failed (result=%d); the "
                    "evaluation stage's inputs are unknown, so the pass-through is not offered",
                    static_cast<Int>(createResult));
            return;
        }

        uint32_t inputCount = 0;
        SpvReflectResult reflectResult = spvReflectEnumerateInputVariables(&reflectModule, &inputCount, nullptr);
        Vector<SpvReflectInterfaceVariable*> inputs(inputCount);
        if (reflectResult == SPV_REFLECT_RESULT_SUCCESS && inputCount > 0) {
            reflectResult = spvReflectEnumerateInputVariables(&reflectModule, &inputCount, inputs.data());
        }
        if (reflectResult != SPV_REFLECT_RESULT_SUCCESS) {
            spvReflectDestroyShaderModule(&reflectModule);
            return;
        }

        // The question is only ever "does this stage read anything a control stage would have to
        // forward", and the answer is: does it have a LOCATION. A located input is a user-defined
        // varying (or a per-patch input), which the vertex stage writes today and would stop
        // reaching once a control stage sits in between - the pass-through carries gl_Position and
        // nothing else, so such a program is declined instead of being handed undefined values.
        // Everything without a location is a built-in: gl_in, gl_TessCoord, gl_PatchVerticesIn,
        // gl_PrimitiveID, gl_TessLevel*, all either forwarded or generated for the evaluation
        // stage by the tessellator itself.
        //
        // This deliberately does NOT judge on SpvReflectInterfaceVariable::built_in. gl_in is an
        // array of interface blocks, and for those SPIRV-Reflect reports built_in == -1 on the
        // block AND leaves every member's built_in at 0 - which is SpvBuiltInPosition, so a
        // member walk reads "Position, Position, Position" for a {Position, PointSize,
        // ClipDistance} block and would accept anything on the strength of parse garbage. The
        // location, by contrast, is decorated on the OpVariable and is what SPIRV-Reflect reads
        // straight through.
        constexpr Uint32 kNoLocation = 0xFFFFFFFFu;
        Bool emulatable = true;
        for (auto* input : inputs) {
            if (input == nullptr) continue;
            if (input->location == kNoLocation) continue;
            MGLOG_E("ProgramFactory: a tessellation evaluation stage with no control stage reads the "
                    "user-defined input '%s' at location=%u; a synthesized control stage cannot forward it, so "
                    "this program's draws are declined rather than fed an undefined varying",
                    input->name != nullptr ? input->name : "<null>", input->location);
            emulatable = false;
            break;
        }

        spvReflectDestroyShaderModule(&reflectModule);
        entry.passthroughTessControlEmulatable = emulatable;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
