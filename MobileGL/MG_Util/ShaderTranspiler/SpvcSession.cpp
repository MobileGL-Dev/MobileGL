// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SpvcSession.h"

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {

            static spvc_basetype MapReflectToSpvcBasetype(const SpvReflectBlockVariable& member) {
                if (!member.type_description) return SPVC_BASETYPE_UNKNOWN;
                auto flags = member.type_description->type_flags;
                auto width = member.numeric.scalar.width;
                auto signedness = member.numeric.scalar.signedness;
                if (flags & SPV_REFLECT_TYPE_FLAG_FLOAT) {
                    switch (width) {
                    case 16: return SPVC_BASETYPE_FP16;
                    case 32: return SPVC_BASETYPE_FP32;
                    case 64: return SPVC_BASETYPE_FP64;
                    default: return SPVC_BASETYPE_UNKNOWN;
                    }
                } else if (flags & SPV_REFLECT_TYPE_FLAG_INT) {
                    if (signedness) {
                        switch (width) {
                        case 8: return SPVC_BASETYPE_INT8;
                        case 16: return SPVC_BASETYPE_INT16;
                        case 32: return SPVC_BASETYPE_INT32;
                        case 64: return SPVC_BASETYPE_INT64;
                        default: return SPVC_BASETYPE_UNKNOWN;
                        }
                    } else {
                        switch (width) {
                        case 8: return SPVC_BASETYPE_UINT8;
                        case 16: return SPVC_BASETYPE_UINT16;
                        case 32: return SPVC_BASETYPE_UINT32;
                        case 64: return SPVC_BASETYPE_UINT64;
                        default: return SPVC_BASETYPE_UNKNOWN;
                        }
                    }
                } else if (flags & SPV_REFLECT_TYPE_FLAG_BOOL) {
                    return SPVC_BASETYPE_BOOLEAN;
                }
                return SPVC_BASETYPE_UNKNOWN;
            }

            // Record one flattened leaf uniform of the global UBO into the metadata maps.
            static void RecordGlobalUboLeaf(const SpvReflectBlockVariable& member, const String& name,
                                            Uint32 offsetInUBO, SpvcMetadata& metadata) {
                metadata.plainUniformOffsetsInUBO[name] = offsetInUBO;
                metadata.plainUniformMemberSizesInBytes[name] = member.size;
                metadata.plainUniformArrayStridesInUBO[name] =
                    member.array.dims_count > 0 ? member.array.stride : 0;

                Uint32 vectorSize = member.numeric.vector.component_count;
                if (vectorSize == 0) vectorSize = 1;
                Uint32 matCol = member.numeric.matrix.column_count;
                if (matCol == 0) matCol = 1;
                metadata.plainUniformMemberTypes[name] = {
                    .basetype = MapReflectToSpvcBasetype(member),
                    .vectorSize = vectorSize,
                    .matCol = matCol,
                };
            }

            // Flatten a (possibly nested struct / struct array) member of the global UBO
            // into leaf entries named the way glslang reflection names plain uniforms:
            // "s[0].b[1].b" for `uniform S s[2]` with `struct T { vec2 b[2]; }` members.
            // glUniform* writes are routed per leaf location, so the state layer needs a
            // byte offset for every leaf, not just for the top-level block members.
            // `baseOffset` accumulates parent offsets; member.offset is relative to the
            // enclosing struct (top-level members: relative to the block start).
            static void FlattenGlobalUboMember(const SpvReflectBlockVariable& member, const String& prefix,
                                               Uint32 baseOffset, SpvcMetadata& metadata) {
                const String name = prefix + (member.name != nullptr ? member.name : "");
                const Uint32 selfOffset = baseOffset + member.offset;

                if (member.member_count == 0 || member.members == nullptr) {
                    RecordGlobalUboLeaf(member, name, selfOffset, metadata);
                    return;
                }

                if (member.array.dims_count == 0) {
                    // Plain nested struct.
                    for (Uint32 j = 0; j < member.member_count; ++j) {
                        FlattenGlobalUboMember(member.members[j], name + ".", selfOffset, metadata);
                    }
                    return;
                }

                if (member.array.dims_count > 1) {
                    // Arrays of arrays of structs cannot be declared in the GL 3.3-era GLSL
                    // MobileGL ingests; record the base so at least element 0 resolves.
                    MGLOG_W("FlattenGlobalUboMember: multi-dimensional struct array '%s' is not supported, "
                            "flattening element 0 only",
                            name.c_str());
                }

                const Uint32 elementCount = member.array.dims[0] > 0 ? member.array.dims[0] : 1;
                const Uint32 elementStride = member.array.stride;
                for (Uint32 element = 0; element < elementCount; ++element) {
                    const String elementPrefix = name + "[" + std::to_string(element) + "].";
                    const Uint32 elementOffset = selfOffset + element * elementStride;
                    for (Uint32 j = 0; j < member.member_count; ++j) {
                        FlattenGlobalUboMember(member.members[j], elementPrefix, elementOffset, metadata);
                    }
                }
            }

            SpvcSession::SpvcSession(const Vector<unsigned int>& spirv, Flags<SessionUsageBit> usage)
                : usage(usage) {
                if (usage & SessionUsageBit::Transpile) {
                    const SpvId* p_spirv = spirv.data();
                    size_t word_count = spirv.size();

                    spvc_context_create(&context);
                    spvc_context_parse_spirv(context, p_spirv, word_count, &ir);
                    spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                                 &compiler);
                    spvc_compiler_create_shader_resources(compiler, &resources);
                } else if (usage & SessionUsageBit::Reflection) {
                    SpvReflectResult result = spvReflectCreateShaderModule(
                        spirv.size() * sizeof(uint32_t), spirv.data(), &reflectModule);
                    reflectModuleValid = (result == SPV_REFLECT_RESULT_SUCCESS);
                }
            }

            SpvcSession::SpvcSession(SpvcSession&& that) {
                std::swap(this->usage, that.usage);
                std::swap(this->context, that.context);
                std::swap(this->compiler, that.compiler);
                std::swap(this->ir, that.ir);
                std::swap(this->compiler_options, that.compiler_options);
                std::swap(this->resources, that.resources);
                std::swap(this->reflectModule, that.reflectModule);
                std::swap(this->reflectModuleValid, that.reflectModuleValid);
                // ParseMetaData() fills `metadata`, and GetMetadata() is read through the
                // moved-to session: leaving it behind silently returns an empty reflection.
                std::swap(this->metadata, that.metadata);
            }

            SpvcSession& SpvcSession::operator=(SpvcSession&& that) {
                std::swap(this->usage, that.usage);
                std::swap(this->context, that.context);
                std::swap(this->compiler, that.compiler);
                std::swap(this->ir, that.ir);
                std::swap(this->compiler_options, that.compiler_options);
                std::swap(this->resources, that.resources);
                std::swap(this->reflectModule, that.reflectModule);
                std::swap(this->reflectModuleValid, that.reflectModuleValid);
                std::swap(this->metadata, that.metadata);
                return *this;
            }

            spvc_result SpvcSession::CreateOptions(spvc_compiler_options* options) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;
                return spvc_compiler_create_compiler_options(compiler, options);
            }

            spvc_result SpvcSession::SetOptions(spvc_compiler_options options) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;
                compiler_options = options;
                return spvc_compiler_install_compiler_options(compiler, options);
            }

            Vector<InterfaceVariable> SpvcSession::GetShaderInterface(spvc_resource_type resource_type) const {
                if (usage & SessionUsageBit::Transpile) {
                    // SPIRV-Cross path
                    const spvc_reflected_resource* list = nullptr;
                    size_t count = 0;
                    spvc_resources_get_resource_list_for_type(resources, resource_type, &list, &count);

                    Vector<InterfaceVariable> variables;
                    for (size_t i = 0; i < count; ++i) {
                        if (spvc_compiler_has_decoration(compiler, list[i].id, SpvDecorationBuiltIn)) {
                            continue;
                        }

                        InterfaceVariable var;
                        var.name = list[i].name;
                        var.location = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
                        variables.push_back(var);
                    }
                    std::sort(variables.begin(), variables.end());
                    return variables;
                }

                // SPIRV-Reflect path (Reflection only, no Transpile)
                if (!reflectModuleValid) return {};

                Vector<InterfaceVariable> variables;
                switch (resource_type) {
                case SPVC_RESOURCE_TYPE_STAGE_INPUT: {
                    uint32_t count = 0;
                    spvReflectEnumerateInputVariables(&reflectModule, &count, nullptr);
                    Vector<SpvReflectInterfaceVariable*> vars(count);
                    spvReflectEnumerateInputVariables(&reflectModule, &count, vars.data());
                    for (uint32_t i = 0; i < count; ++i) {
                        if (vars[i]->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) continue;
                        InterfaceVariable var;
                        var.name = vars[i]->name;
                        var.location = vars[i]->location;
                        variables.push_back(var);
                    }
                    break;
                }
                case SPVC_RESOURCE_TYPE_STAGE_OUTPUT: {
                    uint32_t count = 0;
                    spvReflectEnumerateOutputVariables(&reflectModule, &count, nullptr);
                    Vector<SpvReflectInterfaceVariable*> vars(count);
                    spvReflectEnumerateOutputVariables(&reflectModule, &count, vars.data());
                    for (uint32_t i = 0; i < count; ++i) {
                        if (vars[i]->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) continue;
                        InterfaceVariable var;
                        var.name = vars[i]->name;
                        var.location = vars[i]->location;
                        variables.push_back(var);
                    }
                    break;
                }
                case SPVC_RESOURCE_TYPE_SAMPLED_IMAGE: {
                    uint32_t count = 0;
                    spvReflectEnumerateDescriptorBindings(&reflectModule, &count, nullptr);
                    Vector<SpvReflectDescriptorBinding*> bindings(count);
                    spvReflectEnumerateDescriptorBindings(&reflectModule, &count, bindings.data());
                    for (uint32_t i = 0; i < count; ++i) {
                        if (bindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                            bindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                            InterfaceVariable var;
                            var.name = bindings[i]->name;
                            var.location = bindings[i]->binding;
                            variables.push_back(var);
                        }
                    }
                    break;
                }
                case SPVC_RESOURCE_TYPE_UNIFORM_BUFFER: {
                    uint32_t count = 0;
                    spvReflectEnumerateDescriptorBindings(&reflectModule, &count, nullptr);
                    Vector<SpvReflectDescriptorBinding*> bindings(count);
                    spvReflectEnumerateDescriptorBindings(&reflectModule, &count, bindings.data());
                    for (uint32_t i = 0; i < count; ++i) {
                        if (bindings[i]->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                            InterfaceVariable var;
                            // Use the block/type name (e.g. "MGL_GLOBAL_UBO") rather than
                            // the variable name, which may be empty or meaningless for UBOs.
                            // This is consistent with ParseMetaData() which uses type_description->type_name.
                            if (bindings[i]->type_description && bindings[i]->type_description->type_name) {
                                var.name = bindings[i]->type_description->type_name;
                            } else {
                                var.name = bindings[i]->name;
                            }
                            var.location = bindings[i]->binding;
                            variables.push_back(var);
                        }
                    }
                    break;
                }
                case SPVC_RESOURCE_TYPE_GL_PLAIN_UNIFORM:
                    // GL plain uniforms are a SPIRV-Cross-specific concept.
                    // In reflection-only mode, not available.
                    break;
                default:
                    break;
                }
                std::sort(variables.begin(), variables.end());
                return variables;
            }

            spvc_result SpvcSession::SetVertexAttribLocation(const UnorderedMap<String, Uint>& location) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;

                SPVC_CHK_INIT
                const spvc_reflected_resource* list = nullptr;
                size_t count = 0;
                SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT,
                                                                          &list, &count));
                for (size_t i = 0; i < count; ++i) {
                    auto& resource = list[i];
                    auto it = location.find(resource.name);
                    if (it != location.end()) {
                        spvc_compiler_set_decoration(compiler, resource.id, SpvDecorationLocation, it->second);
                    }
                }
                SPVC_CHK_RETURN
            }

            spvc_result SpvcSession::Compile(const char** result) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;
                SPVC_CHK_INIT
                SPVC_CHK_RESULT(spvc_compiler_compile(compiler, result));
                SPVC_CHK_RETURN
            }

            spvc_result SpvcSession::ParseMetaData() {
                if (usage & SessionUsageBit::Transpile) {
                    // SPIRV-Cross path
                    SPVC_CHK_INIT

                    metadata = SpvcMetadata();

                    const spvc_reflected_resource* list = nullptr;
                    size_t count = 0;

                    SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(
                        resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count);)
                    for (size_t i = 0; i < count; ++i) {
                        if (spvc_compiler_has_decoration(compiler, list[i].id, SpvDecorationBuiltIn)) {
                            continue;
                        }

                        if (strcmp(list[i].name, GLOBAL_UBO_NAME) == 0) {
                            spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].base_type_id);
                            spvc_compiler_get_declared_struct_size(compiler, type, &metadata.globalUboSize);
                            size_t num_members = spvc_type_get_num_member_types(type);
                            for (size_t j = 0; j < num_members; ++j) {
                                const char* memberName =
                                    spvc_compiler_get_member_name(compiler, list[i].base_type_id, j);

                                unsigned memberOffset = 0;
                                SPVC_CHK_RESULT(
                                    spvc_compiler_type_struct_member_offset(compiler, type, j, &memberOffset);)
                                metadata.plainUniformOffsetsInUBO[memberName] = memberOffset;
                                SizeT memberSize = 0;
                                SPVC_CHK_RESULT(
                                    spvc_compiler_get_declared_struct_member_size(compiler, type, j, &memberSize);)
                                metadata.plainUniformMemberSizesInBytes[memberName] = memberSize;

                                auto memberTypeId = spvc_type_get_member_type(type, j);
                                spvc_type memberType = spvc_compiler_get_type_handle(compiler, memberTypeId);
                                spvc_basetype basetype = spvc_type_get_basetype(memberType);
                                auto vectorSize = spvc_type_get_vector_size(memberType);
                                auto matCol = spvc_type_get_columns(memberType);
                                metadata.plainUniformMemberTypes[memberName] = {
                                    .basetype = basetype,
                                    .vectorSize = vectorSize,
                                    .matCol = matCol,
                                };
                            }
                            SPVC_CHK_RETURN
                        }
                    }
                    return SPVC_ERROR_INVALID_SPIRV;
                }

                // SPIRV-Reflect path (Reflection only)
                if (!reflectModuleValid) return SPVC_ERROR_INVALID_SPIRV;

                metadata = SpvcMetadata();

                uint32_t bindingCount = 0;
                spvReflectEnumerateDescriptorBindings(&reflectModule, &bindingCount, nullptr);
                Vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
                spvReflectEnumerateDescriptorBindings(&reflectModule, &bindingCount, bindings.data());

                for (uint32_t i = 0; i < bindingCount; ++i) {
                    auto* binding = bindings[i];
                    if (binding->descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
                    if (strcmp(binding->type_description->type_name, GLOBAL_UBO_NAME) != 0) continue;

                    auto& block = binding->block;
                    metadata.globalUboSize = block.size;

                    for (uint32_t j = 0; j < block.member_count; ++j) {
                        // Recurse into nested structs / struct arrays so every leaf
                        // uniform ("s[0].b[1].b") gets its real byte offset; top-level
                        // scalars/vectors/matrices flatten to themselves.
                        FlattenGlobalUboMember(block.members[j], "", 0, metadata);
                    }
                    return SPVC_SUCCESS;
                }
                return SPVC_ERROR_INVALID_SPIRV;
            }

            const SpvcMetadata& SpvcSession::GetMetadata() const {
                return metadata;
            }

            const char* SpvcSession::GetLastErrorString() const {
                if (context) {
                    return spvc_context_get_last_error_string(context);
                }
                return "";
            }

            SpvcSession::~SpvcSession() {
                spvc_context_destroy(context);
                if (reflectModuleValid) {
                    spvReflectDestroyShaderModule(&reflectModule);
                }
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
