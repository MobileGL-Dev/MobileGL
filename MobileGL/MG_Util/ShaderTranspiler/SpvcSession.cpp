// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SpvcSession.h"

#include <algorithm>

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

            // Write one metadata entry. `name` is already the glslang-reflection spelling the
            // GL uniform locations are keyed on, and `arrayStride`/`sizeInBytes` describe the
            // entry rather than the whole declaration (they differ for a sub-array of an
            // array-of-arrays - see RecordGlobalUboLeaf).
            static void RecordGlobalUboLeafEntry(const SpvReflectBlockVariable& member, const String& name,
                                                 Uint32 offsetInUBO, Uint32 arrayStride, SizeT sizeInBytes,
                                                 SpvcMetadata& metadata) {
                metadata.plainUniformOffsetsInUBO[name] = offsetInUBO;
                metadata.plainUniformMemberSizesInBytes[name] = sizeInBytes;
                metadata.plainUniformArrayStridesInUBO[name] = arrayStride;

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

            // Record one flattened leaf uniform of the global UBO into the metadata maps.
            //
            // SPIRV-Reflect keeps `float u[2][3]` as ONE leaf carrying every dimension in
            // array.dims[] and the INNERMOST element stride in array.stride (the outer
            // ArrayStride decoration is overwritten as ParseType recurses into the element
            // type, which is also why array.size == product(dims) * stride). glslang's
            // reflection - which owns the names GL uniform locations are keyed on - stops at
            // "reflection granularity" instead (reflection.cpp: !type.isArrayOfArrays()), so
            // the same declaration arrives on the GL side as "u[0]" and "u[1]", each a
            // `float[3]` holding its own three locations.
            //
            // Emitting a single "u" leaf here therefore only ever routes the FIRST sub-array:
            // the routing loop stops as soon as a location belongs to a different uniform, and
            // every element from "u[1][0]" on finds no offset and falls through to the fallback
            // scratch storage at the tail of the shadow - bytes the GPU never reads, so those
            // glUniform writes are silently lost
            // (KHR-GLES31.explicit_uniform_location.uniform-loc-arrays-of-arrays). Expand every
            // dimension but the last, exactly as glslang does, and give each sub-array its own
            // byte offset.
            static void RecordGlobalUboLeaf(const SpvReflectBlockVariable& member, const String& name,
                                            Uint32 offsetInUBO, SpvcMetadata& metadata) {
                const Uint32 arrayStride = member.array.dims_count > 0 ? member.array.stride : 0;
                const Bool isArrayOfArrays = member.array.dims_count > 1 && arrayStride > 0;
                if (!isArrayOfArrays) {
                    RecordGlobalUboLeafEntry(member, name, offsetInUBO, arrayStride, member.size, metadata);
                    return;
                }

                // Extent 0 is SPIRV-Reflect's OpTypeRuntimeArray marker, which a plain uniform
                // cannot be - but it must not be expanded (or divided by) if it ever appears.
                Uint32 subArrayCount = 1;
                for (Uint32 dim = 0; dim + 1 < member.array.dims_count; ++dim) {
                    const Uint32 extent = member.array.dims[dim];
                    if (extent == 0) {
                        MGLOG_W_ONCE("RecordGlobalUboLeaf: multi-dimensional uniform '%s' has a non-constant "
                                "dimension, recording the base entry only",
                                name.c_str());
                        RecordGlobalUboLeafEntry(member, name, offsetInUBO, arrayStride, member.size, metadata);
                        return;
                    }
                    subArrayCount *= extent;
                }
                const Uint32 innerExtent = member.array.dims[member.array.dims_count - 1] > 0
                                               ? member.array.dims[member.array.dims_count - 1]
                                               : 1;
                const Uint32 subArrayStride = innerExtent * arrayStride;

                // Row-major odometer over dims[0 .. dims_count-2]: the last dimension varies
                // fastest, so `subArray` counts sub-arrays in exactly memory order.
                Vector<Uint32> indices(member.array.dims_count - 1, 0);
                for (Uint32 subArray = 0; subArray < subArrayCount; ++subArray) {
                    String elementName = name;
                    for (const Uint32 index : indices) {
                        elementName += "[" + std::to_string(index) + "]";
                    }
                    RecordGlobalUboLeafEntry(member, elementName, offsetInUBO + subArray * subArrayStride,
                                             arrayStride, subArrayStride, metadata);
                    for (SizeT dim = indices.size(); dim-- > 0;) {
                        if (++indices[dim] < member.array.dims[dim]) break;
                        indices[dim] = 0;
                    }
                }
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
                    MGLOG_W_ONCE("FlattenGlobalUboMember: multi-dimensional struct array '%s' is not supported, "
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

                    // Every step is checked, and each guards the next: the C API writes its
                    // out-param only on success, so passing a failed step's null handle to the
                    // step after it is a raw dereference (spvc_context_create_compiler does
                    // `parsed_ir->parsed`, spvc_compiler_create_shader_resources does
                    // `compiler->context`). IsTranspileReady() is how a caller asks whether this
                    // sequence got all the way through.
                    if (spvc_context_create(&context) != SPVC_SUCCESS) {
                        context = nullptr;
                        return;
                    }
                    if (spvc_context_parse_spirv(context, p_spirv, word_count, &ir) != SPVC_SUCCESS) {
                        ir = nullptr;
                        return;
                    }
                    if (spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir,
                                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
                        compiler = nullptr;
                        return;
                    }
                    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
                        resources = nullptr;
                        return;
                    }
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

            spvc_result SpvcSession::SetShaderStorageBlockBinding(const UnorderedMap<String, Int>& bindings) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;

                SPVC_CHK_INIT
                const spvc_reflected_resource* list = nullptr;
                size_t count = 0;
                SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(
                    resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &list, &count));
                for (size_t i = 0; i < count; ++i) {
                    auto& resource = list[i];
                    // Two spellings, because neither one alone identifies the block the GL
                    // interface query named. `resource.name` is the block's instance name when
                    // the declaration has one; the block TYPE name (which is what the GL query
                    // reports for a block) lives on base_type_id. An arrayed block collapses to
                    // a single SPIR-V resource while GL enumerates it per element, so the bare
                    // name is also tried with element zero's subscript - the same convention
                    // ProgramObject::GetShaderStorageBlockBindingOverride documents.
                    const char* blockTypeName = spvc_compiler_get_name(compiler, resource.base_type_id);
                    const String candidates[] = {
                        blockTypeName != nullptr ? String(blockTypeName) : String(),
                        resource.name != nullptr ? String(resource.name) : String(),
                    };
                    for (const auto& candidate : candidates) {
                        if (candidate.empty()) continue;
                        auto it = bindings.find(candidate);
                        if (it == bindings.end()) it = bindings.find(candidate + "[0]");
                        if (it == bindings.end()) continue;
                        // Negative is "never rebound" - the declared qualifier still stands.
                        if (it->second < 0) break;
                        spvc_compiler_set_decoration(compiler, resource.id, SpvDecorationBinding,
                                                     static_cast<unsigned>(it->second));
                        break;
                    }
                }
                SPVC_CHK_RETURN
            }

            // "gl_AtomicCounterBlock_5" -> 5, -1 for anything that is not one of those blocks.
            // The suffix is the GL atomic-counter binding the application declared, and after
            // the relaxed lowering it is the only place that number still exists.
            static Int AtomicCounterBlockBinding(const char* blockName) {
                if (blockName == nullptr) return -1;
                const SizeT prefixLength = std::strlen(ATOMIC_COUNTER_BLOCK_PREFIX);
                const String name = blockName;
                if (name.length() <= prefixLength + 1) return -1;
                if (name.compare(0, prefixLength, ATOMIC_COUNTER_BLOCK_PREFIX) != 0) return -1;
                if (name[prefixLength] != '_') return -1;
                Int binding = 0;
                for (SizeT i = prefixLength + 1; i < name.length(); ++i) {
                    if (name[i] < '0' || name[i] > '9') return -1;
                    binding = binding * 10 + (name[i] - '0');
                    if (binding > 0x0FFFFFFF) return -1;
                }
                return binding;
            }

            spvc_result SpvcSession::SetAtomicCounterBlockBindings(Int topBinding, Vector<Int>& outGlBindings) {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;

                SPVC_CHK_INIT
                const spvc_reflected_resource* list = nullptr;
                size_t count = 0;
                SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(
                    resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &list, &count));
                for (size_t i = 0; i < count; ++i) {
                    auto& resource = list[i];
                    // The block TYPE name: glslang gives the synthesized block an EMPTY instance
                    // name, so resource.name carries nothing to match on. Read before Compile(),
                    // which is where SPIRV-Cross renames the reserved "gl_" prefix away.
                    const Int glBinding = AtomicCounterBlockBinding(
                        spvc_compiler_get_name(compiler, resource.base_type_id));
                    if (glBinding < 0) continue;
                    const Int esslBinding = topBinding - glBinding;
                    if (esslBinding < 0) {
                        MGLOG_E_ONCE("Atomic counter binding %d needs more shader storage binding points than this "
                                     "driver has; its counters will not be updated.",
                                     glBinding);
                        continue;
                    }
                    spvc_compiler_set_decoration(compiler, resource.id, SpvDecorationBinding,
                                                 static_cast<unsigned>(esslBinding));
                    outGlBindings.push_back(glBinding);
                }
                SPVC_CHK_RETURN
            }

            spvc_result SpvcSession::DropDefaultFragmentOutputColorIndex() {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;

                SPVC_CHK_INIT
                const spvc_reflected_resource* list = nullptr;
                size_t count = 0;
                SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(
                    resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &list, &count));
                for (size_t i = 0; i < count; ++i) {
                    const spvc_reflected_resource& resource = list[i];
                    if (!spvc_compiler_has_decoration(compiler, resource.id, SpvDecorationIndex)) continue;
                    if (spvc_compiler_get_decoration(compiler, resource.id, SpvDecorationIndex) != 0u) continue;
                    spvc_compiler_unset_decoration(compiler, resource.id, SpvDecorationIndex);
                }
                SPVC_CHK_RETURN
            }

            spvc_result SpvcSession::RelaxReadWriteExclusiveStorageBuffers() {
                if (!(usage & SessionUsageBit::Transpile)) return SPVC_ERROR_INVALID_ARGUMENT;

                SPVC_CHK_INIT
                const spvc_reflected_resource* list = nullptr;
                size_t count = 0;
                SPVC_CHK_RESULT(spvc_resources_get_resource_list_for_type(
                    resources, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &list, &count));
                for (size_t i = 0; i < count; ++i) {
                    const spvc_reflected_resource& resource = list[i];
                    // The variable itself, for a block the application qualified as a whole.
                    if (spvc_compiler_has_decoration(compiler, resource.id, SpvDecorationNonReadable) &&
                        spvc_compiler_has_decoration(compiler, resource.id, SpvDecorationNonWritable)) {
                        spvc_compiler_unset_decoration(compiler, resource.id, SpvDecorationNonReadable);
                        spvc_compiler_unset_decoration(compiler, resource.id, SpvDecorationNonWritable);
                    }
                    // ...and each member, which is where the qualifiers usually sit and where
                    // SPIRV-Cross reads them from before hoisting the ones every member shares.
                    const spvc_type blockType = spvc_compiler_get_type_handle(compiler, resource.base_type_id);
                    if (blockType == nullptr) continue;
                    const unsigned memberCount = spvc_type_get_num_member_types(blockType);
                    for (unsigned member = 0; member < memberCount; ++member) {
                        if (!spvc_compiler_has_member_decoration(compiler, resource.base_type_id, member,
                                                                 SpvDecorationNonReadable) ||
                            !spvc_compiler_has_member_decoration(compiler, resource.base_type_id, member,
                                                                 SpvDecorationNonWritable)) {
                            continue;
                        }
                        spvc_compiler_unset_member_decoration(compiler, resource.base_type_id, member,
                                                              SpvDecorationNonReadable);
                        spvc_compiler_unset_member_decoration(compiler, resource.base_type_id, member,
                                                              SpvDecorationNonWritable);
                    }
                }
                SPVC_CHK_RETURN
            }

            namespace {
                // How many 32-bit components a captured variable occupies, which is what the
                // gl_SkipComponentsN padding below is counted in. Matrices and arrays multiply.
                Uint32 XfbComponentCount(spvc_compiler compiler, spvc_type_id typeId) {
                    const spvc_type type = spvc_compiler_get_type_handle(compiler, typeId);
                    if (type == nullptr) return 0;
                    Uint32 components = spvc_type_get_vector_size(type) * spvc_type_get_columns(type);
                    const unsigned dimensions = spvc_type_get_num_array_dimensions(type);
                    for (unsigned d = 0; d < dimensions; ++d) {
                        const unsigned length = spvc_type_get_array_dimension(type, d);
                        if (length != 0) components *= length;
                    }
                    // A double occupies two component slots per scalar (GL 4.6 core 11.1.2.1).
                    const spvc_basetype base = spvc_type_get_basetype(type);
                    if (base == SPVC_BASETYPE_FP64 || base == SPVC_BASETYPE_INT64 ||
                        base == SPVC_BASETYPE_UINT64) {
                        components *= 2;
                    }
                    return components;
                }
            } // namespace

            Vector<SpirvXfbCapture> SpvcSession::ReflectTransformFeedbackCaptures() const {
                Vector<SpirvXfbCapture> captures;
                if (compiler == nullptr || resources == nullptr) return captures;

                const spvc_reflected_resource* outputs = nullptr;
                SizeT outputCount = 0;
                if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &outputs,
                                                              &outputCount) != SPVC_SUCCESS) {
                    return captures;
                }

                for (SizeT i = 0; i < outputCount; ++i) {
                    const spvc_reflected_resource& output = outputs[i];
                    // XfbBuffer/XfbStride sit on the VARIABLE; Offset sits on the variable for a
                    // plain output and on each MEMBER for a block (which is how a redeclared
                    // gl_PerVertex carries it).
                    const Bool hasBuffer =
                        spvc_compiler_has_decoration(compiler, output.id, SpvDecorationXfbBuffer) == SPVC_TRUE;
                    const Uint32 buffer =
                        hasBuffer ? spvc_compiler_get_decoration(compiler, output.id, SpvDecorationXfbBuffer) : 0u;
                    const Uint32 stride =
                        spvc_compiler_has_decoration(compiler, output.id, SpvDecorationXfbStride) == SPVC_TRUE
                            ? spvc_compiler_get_decoration(compiler, output.id, SpvDecorationXfbStride)
                            : 0u;

                    const spvc_type type = spvc_compiler_get_type_handle(compiler, output.base_type_id);
                    const unsigned memberCount =
                        type != nullptr && spvc_type_get_basetype(type) == SPVC_BASETYPE_STRUCT
                            ? spvc_type_get_num_member_types(type)
                            : 0u;

                    if (memberCount == 0) {
                        if (spvc_compiler_has_decoration(compiler, output.id, SpvDecorationOffset) != SPVC_TRUE) {
                            continue;
                        }
                        SpirvXfbCapture capture;
                        capture.name = output.name ? output.name : "";
                        capture.buffer = buffer;
                        capture.stride = stride;
                        capture.offset = spvc_compiler_get_decoration(compiler, output.id, SpvDecorationOffset);
                        capture.componentCount = XfbComponentCount(compiler, output.type_id);
                        if (!capture.name.empty()) captures.push_back(Move(capture));
                        continue;
                    }

                    for (unsigned member = 0; member < memberCount; ++member) {
                        if (spvc_compiler_has_member_decoration(compiler, output.base_type_id, member,
                                                                SpvDecorationOffset) != SPVC_TRUE) {
                            continue;
                        }
                        const char* memberName =
                            spvc_compiler_get_member_name(compiler, output.base_type_id, member);
                        if (memberName == nullptr || *memberName == '\0') continue;
                        SpirvXfbCapture capture;
                        // A redeclared built-in block contributes its members by their own names
                        // ("gl_Position"), which is how GL's capture interface spells them; an
                        // application block spells them "Block.member".
                        const String blockName = output.name ? String(output.name) : String{};
                        const Bool isBuiltInBlock = blockName.compare(0, 3, "gl_") == 0;
                        capture.name = isBuiltInBlock || blockName.empty()
                                           ? String(memberName)
                                           : blockName + "." + String(memberName);
                        capture.buffer = buffer;
                        capture.stride = stride;
                        capture.offset = spvc_compiler_get_member_decoration(compiler, output.base_type_id, member,
                                                                            SpvDecorationOffset);
                        capture.componentCount = XfbComponentCount(
                            compiler, spvc_type_get_member_type(type, member));
                        captures.push_back(Move(capture));
                    }
                }

                // Capture order IS buffer-then-offset order: that is the order the equivalent
                // glTransformFeedbackVaryings request has to name them in for the frontend's
                // packer to reproduce the declared layout.
                std::stable_sort(captures.begin(), captures.end(),
                                 [](const SpirvXfbCapture& a, const SpirvXfbCapture& b) {
                                     if (a.buffer != b.buffer) return a.buffer < b.buffer;
                                     return a.offset < b.offset;
                                 });
                return captures;
            }

            void SpvcSession::StripTransformFeedbackDecorations() {
                if (compiler == nullptr || resources == nullptr) return;

                const spvc_reflected_resource* outputs = nullptr;
                SizeT outputCount = 0;
                if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &outputs,
                                                              &outputCount) != SPVC_SUCCESS) {
                    return;
                }
                for (SizeT i = 0; i < outputCount; ++i) {
                    const spvc_reflected_resource& output = outputs[i];
                    spvc_compiler_unset_decoration(compiler, output.id, SpvDecorationXfbBuffer);
                    spvc_compiler_unset_decoration(compiler, output.id, SpvDecorationXfbStride);
                    spvc_compiler_unset_decoration(compiler, output.id, SpvDecorationOffset);

                    const spvc_type type = spvc_compiler_get_type_handle(compiler, output.base_type_id);
                    if (type == nullptr || spvc_type_get_basetype(type) != SPVC_BASETYPE_STRUCT) continue;
                    const unsigned memberCount = spvc_type_get_num_member_types(type);
                    for (unsigned member = 0; member < memberCount; ++member) {
                        spvc_compiler_unset_member_decoration(compiler, output.base_type_id, member,
                                                              SpvDecorationOffset);
                        spvc_compiler_unset_member_decoration(compiler, output.base_type_id, member,
                                                              SpvDecorationXfbBuffer);
                        spvc_compiler_unset_member_decoration(compiler, output.base_type_id, member,
                                                              SpvDecorationXfbStride);
                    }
                }
            }

            spvc_result SpvcSession::SetEntryPoint(const char* name, SpvExecutionModel model) {
                // A null compiler or a null/empty name is a FAILURE, not a silent success: the
                // caller is asking for a specific entry point and there is none to give it.
                if (compiler == nullptr || name == nullptr || *name == '\0') return SPVC_ERROR_INVALID_ARGUMENT;
                return spvc_compiler_set_entry_point(compiler, name, model);
            }

            Bool SpvcSession::SetSpecializationConstants(const Vector<Uint32>& constantIds,
                                                         const Vector<Uint32>& constantValues,
                                                         Uint32& outUnknownConstantId) {
                if (constantIds.empty()) return true;
                if (compiler == nullptr) return false;

                const spvc_specialization_constant* declared = nullptr;
                SizeT declaredCount = 0;
                if (spvc_compiler_get_specialization_constants(compiler, &declared, &declaredCount) != SPVC_SUCCESS) {
                    outUnknownConstantId = constantIds.front();
                    return false;
                }

                for (SizeT i = 0; i < constantIds.size(); ++i) {
                    const Uint32 wantedId = constantIds[i];
                    spvc_constant handle = nullptr;
                    for (SizeT j = 0; j < declaredCount; ++j) {
                        if (declared[j].constant_id != wantedId) continue;
                        handle = spvc_compiler_get_constant_handle(compiler, declared[j].id);
                        break;
                    }
                    if (handle == nullptr) {
                        // ARB_gl_spirv: "INVALID_VALUE is generated if any value in pConstantIndex
                        // refers to a specialization constant that does not exist in the shader
                        // module". Reported rather than skipped - a silently ignored id would let
                        // the shader specialize to something the application never asked for.
                        outUnknownConstantId = wantedId;
                        return false;
                    }
                    // The GL side hands over a flat GLuint per constant and ARB_gl_spirv says it
                    // is "interpreted according to the type of the specialization constant", so
                    // the 32-bit PATTERN is what has to be stored, not a converted number.
                    // spvc_constant_set_scalar_u32 writes exactly that pattern into the constant's
                    // scalar union, which SPIRV-Cross then reads back as whatever the constant's
                    // declared type is - the reinterpretation the extension asks for, for free.
                    spvc_constant_set_scalar_u32(handle, 0, 0, constantValues[i]);
                }
                return true;
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
