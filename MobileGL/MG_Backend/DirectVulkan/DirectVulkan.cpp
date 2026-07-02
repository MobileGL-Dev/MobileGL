// MobileGL - MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectVulkan.h"
#include "DirectVulkanResourceState.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ErrorState/ErrorInfo.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Util/Miscellany/IndexGenerator.h"
#include <cstring>
#include <spirv_reflect.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    UniquePtr<VulkanRenderer> pVulkanRenderer = nullptr;

    namespace {
        struct BufferVariableResource {
            String name;
            GLuint blockIndex = 0;
            GLint offset = 0;
            GLint size = 0;
        };

        struct StorageBlockResource {
            String name;
            GLuint binding = 0;
            GLint dataSize = 0;
            Vector<GLuint> activeVariables;
        };

        struct ProgramResourceCache {
            Uint32 backendStateVersion = 0;
            Vector<StorageBlockResource> storageBlocks;
            Vector<BufferVariableResource> bufferVariables;
            GLint computeWorkGroupSize[3] = {1, 1, 1};
        };

        struct DrawElementsIndirectCommand {
            Uint32 count = 0;
            Uint32 instanceCount = 0;
            Uint32 firstIndex = 0;
            Int32 baseVertex = 0;
            Uint32 baseInstance = 0;
        };

        struct DrawArraysIndirectCommand {
            Uint32 count = 0;
            Uint32 instanceCount = 0;
            Uint32 first = 0;
            Uint32 baseInstance = 0;
        };

        UnorderedMap<GLuint, ProgramResourceCache> g_programResourceCaches;

        String NormalizeDescriptorName(const SpvReflectDescriptorBinding& binding) {
            const char* rawName = binding.name;
            if (binding.type_description != nullptr && binding.type_description->type_name != nullptr) {
                rawName = binding.type_description->type_name;
            }
            if (rawName == nullptr) {
                return {};
            }
            String name = rawName;
            const auto arraySuffix = name.find("[0]");
            if (arraySuffix != String::npos) {
                name = name.substr(0, arraySuffix);
            }
            return name;
        }

        void AddBufferVariablesRecursive(const SpvReflectBlockVariable& variable, const String& prefix,
                                         GLuint blockIndex, Vector<BufferVariableResource>& variables,
                                         Vector<GLuint>& activeVariables) {
            for (Uint32 memberIndex = 0; memberIndex < variable.member_count; ++memberIndex) {
                const auto& member = variable.members[memberIndex];
                String name = prefix;
                if (!name.empty()) {
                    name += ".";
                }
                name += member.name ? member.name : "";

                if (member.member_count > 0) {
                    AddBufferVariablesRecursive(member, name, blockIndex, variables, activeVariables);
                    continue;
                }

                BufferVariableResource resource{};
                resource.name = name;
                resource.blockIndex = blockIndex;
                resource.offset = static_cast<GLint>(member.offset);
                resource.size = static_cast<GLint>(member.size);
                const GLuint variableIndex = static_cast<GLuint>(variables.size());
                variables.push_back(resource);
                activeVariables.push_back(variableIndex);
            }
        }

        ProgramResourceCache& GetProgramResourceCache(const MG_State::GLState::ProgramObject& program) {
            auto& cache = g_programResourceCaches[program.GetExternalIndex()];
            const Uint32 backendStateVersion = program.GetBackendStateVersion();
            if (cache.backendStateVersion == backendStateVersion &&
                (!cache.storageBlocks.empty() || !cache.bufferVariables.empty())) {
                return cache;
            }

            cache = {};
            cache.backendStateVersion = backendStateVersion;

            Vector<SpvReflectShaderModule> modules;
            Vector<Bool> validModules;
            const auto& spirvs = program.GetGeneratedSpirv();
            for (const auto& spirv : spirvs) {
                if (spirv.empty()) {
                    continue;
                }
                SpvReflectShaderModule module{};
                const SpvReflectResult result =
                    spvReflectCreateShaderModule(spirv.size() * sizeof(Uint), spirv.data(), &module);
                if (result != SPV_REFLECT_RESULT_SUCCESS) {
                    continue;
                }
                modules.push_back(module);
                validModules.push_back(true);
            }

            for (auto& module : modules) {
                for (Uint32 entryIndex = 0; entryIndex < module.entry_point_count; ++entryIndex) {
                    const auto& entryPoint = module.entry_points[entryIndex];
                    if ((entryPoint.shader_stage & SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) == 0) {
                        continue;
                    }
                    cache.computeWorkGroupSize[0] = static_cast<GLint>(std::max<Uint32>(entryPoint.local_size.x, 1));
                    cache.computeWorkGroupSize[1] = static_cast<GLint>(std::max<Uint32>(entryPoint.local_size.y, 1));
                    cache.computeWorkGroupSize[2] = static_cast<GLint>(std::max<Uint32>(entryPoint.local_size.z, 1));
                }

                uint32_t bindingCount = 0;
                SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
                if (result != SPV_REFLECT_RESULT_SUCCESS || bindingCount == 0) {
                    continue;
                }
                Vector<SpvReflectDescriptorBinding*> bindings(bindingCount);
                result = spvReflectEnumerateDescriptorBindings(&module, &bindingCount, bindings.data());
                if (result != SPV_REFLECT_RESULT_SUCCESS) {
                    continue;
                }
                std::sort(bindings.begin(), bindings.end(), [](const auto* lhs, const auto* rhs) {
                    const String lhsName = lhs ? NormalizeDescriptorName(*lhs) : String();
                    const String rhsName = rhs ? NormalizeDescriptorName(*rhs) : String();
                    if (lhsName != rhsName) return lhsName < rhsName;
                    return lhs->binding < rhs->binding;
                });
                for (const auto* binding : bindings) {
                    if (binding == nullptr ||
                        binding->descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        continue;
                    }
                    const String blockName = NormalizeDescriptorName(*binding);
                    if (blockName.empty()) {
                        continue;
                    }
                    const auto existing = std::find_if(
                        cache.storageBlocks.begin(), cache.storageBlocks.end(),
                        [&](const StorageBlockResource& block) { return block.name == blockName; });
                    if (existing != cache.storageBlocks.end()) {
                        continue;
                    }

                    StorageBlockResource block{};
                    block.name = blockName;
                    block.binding = binding->binding;
                    block.dataSize = static_cast<GLint>(binding->block.size);
                    const GLuint blockIndex = static_cast<GLuint>(cache.storageBlocks.size());
                    AddBufferVariablesRecursive(binding->block, blockName, blockIndex, cache.bufferVariables,
                                                block.activeVariables);
                    cache.storageBlocks.push_back(block);
                }
            }

            for (SizeT i = 0; i < modules.size(); ++i) {
                if (validModules[i]) {
                    spvReflectDestroyShaderModule(&modules[i]);
                }
            }
            return cache;
        }

        MG_State::GLState::ProgramObject* TryGetDirectVulkanProgram(GLuint program) {
            if (!MG_State::pGLContext || !MG_State::pGLContext->ValidateProgramName(program)) {
                return nullptr;
            }
            auto& programObject = MG_State::pGLContext->GetProgramObject(program);
            return programObject.get();
        }

        void CopyResourceName(const String& source, GLsizei bufSize, GLsizei* length, GLchar* name) {
            const GLsizei writtenLength = static_cast<GLsizei>(source.size());
            if (length) {
                *length = writtenLength;
            }
            if (name && bufSize > 0) {
                const GLsizei copyLength = std::min<GLsizei>(bufSize - 1, writtenLength);
                std::memcpy(name, source.data(), static_cast<SizeT>(copyLength));
                name[copyLength] = '\0';
            }
        }

        const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
            auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            if (drawBuffer) {
                drawBuffer->MarkPersistentMappedRangeDirty();
                const auto drawData = drawBuffer->GetDataReadOnly();
                const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
                if (!drawData || commandOffset + requiredBytes > drawData->size()) {
                    MGLOG_E("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                    return nullptr;
                }
                return drawData->data() + commandOffset;
            }

            if (!indirect) {
                MGLOG_E("%s skipped: indirect pointer is null", label);
                return nullptr;
            }

            return reinterpret_cast<const Uint8*>(indirect);
        }

        Vector<GLuint> GetUniformBlockActiveVariables(const MG_State::GLState::ProgramObject& program,
                                                      GLuint blockIndex) {
            Vector<GLuint> activeVariables;
            const Uint uniformCount = program.GetUniformCount();
            activeVariables.reserve(uniformCount);
            for (Uint uniformIndex = 0; uniformIndex < uniformCount; ++uniformIndex) {
                if (program.GetActiveUniformBlockIndex(uniformIndex) == static_cast<Int>(blockIndex)) {
                    activeVariables.push_back(uniformIndex);
                }
            }
            return activeVariables;
        }

        GLuint FindProgramInputIndex(const MG_State::GLState::ProgramObject& program, const String& name) {
            const Int activeCount = program.GetActiveAttributesCount();
            for (Int index = 0; index < activeCount; ++index) {
                if (program.GetActiveAttribName(index) == name) {
                    return static_cast<GLuint>(index);
                }
            }
            return GL_INVALID_INDEX;
        }

        GLuint FindProgramOutputIndex(const MG_State::GLState::ProgramObject& program, const String& name) {
            const Int activeCount = program.GetActiveFragmentOutputCount();
            for (Int index = 0; index < activeCount; ++index) {
                if (program.GetActiveFragmentOutputName(index) == name) {
                    return static_cast<GLuint>(index);
                }
            }
            return GL_INVALID_INDEX;
        }

        GLint GetProgramOutputLocation(const MG_State::GLState::ProgramObject& program, const String& name) {
            const Int activeCount = program.GetActiveFragmentOutputCount();
            for (Int index = 0; index < activeCount; ++index) {
                if (program.GetActiveFragmentOutputName(index) == name) {
                    return program.GetFragmentOutputLocation(index);
                }
            }
            return -1;
        }

        GLint GetProgramResourceActiveCount(const MG_State::GLState::ProgramObject& program, GLenum programInterface,
                                            const ProgramResourceCache& cache) {
            switch (programInterface) {
            case GL_SHADER_STORAGE_BLOCK:
                return static_cast<GLint>(cache.storageBlocks.size());
            case GL_BUFFER_VARIABLE:
                return static_cast<GLint>(cache.bufferVariables.size());
            case GL_UNIFORM_BLOCK:
                return program.GetActiveUniformBlocksCount();
            case GL_UNIFORM:
                return static_cast<GLint>(program.GetUniformCount());
            case GL_PROGRAM_INPUT:
                return program.GetActiveAttributesCount();
            case GL_PROGRAM_OUTPUT:
                return program.GetActiveFragmentOutputCount();
            default:
                return 0;
            }
        }

        GLint GetProgramResourceMaxNameLength(const MG_State::GLState::ProgramObject& program, GLenum programInterface,
                                              const ProgramResourceCache& cache) {
            switch (programInterface) {
            case GL_SHADER_STORAGE_BLOCK: {
                SizeT maxLength = 0;
                for (const auto& block : cache.storageBlocks) maxLength = std::max(maxLength, block.name.size() + 1);
                return static_cast<GLint>(maxLength);
            }
            case GL_BUFFER_VARIABLE: {
                SizeT maxLength = 0;
                for (const auto& var : cache.bufferVariables) maxLength = std::max(maxLength, var.name.size() + 1);
                return static_cast<GLint>(maxLength);
            }
            case GL_UNIFORM_BLOCK:
                return program.GetActiveUniformBlocksMaxNameLength() + 1;
            case GL_UNIFORM:
                return program.GetUniformMaxLength() + 1;
            case GL_PROGRAM_INPUT:
                return program.GetActiveAttributesMaxLength() + 1;
            case GL_PROGRAM_OUTPUT: {
                SizeT maxLength = 0;
                const Int activeCount = program.GetActiveFragmentOutputCount();
                for (Int index = 0; index < activeCount; ++index) {
                    maxLength = std::max(maxLength, program.GetActiveFragmentOutputName(index).size() + 1);
                }
                return static_cast<GLint>(maxLength);
            }
            default:
                return 0;
            }
        }
    } // namespace

    GLuint GetShaderStorageBlockIndex(const MG_State::GLState::ProgramObject& program, const String& name) {
        auto& cache = GetProgramResourceCache(program);
        const auto it = std::find_if(cache.storageBlocks.begin(), cache.storageBlocks.end(),
            [&](const StorageBlockResource& block) { return block.name == name; });
        return it == cache.storageBlocks.end()
            ? GL_INVALID_INDEX
            : static_cast<GLuint>(std::distance(cache.storageBlocks.begin(), it));
    }

    GLuint GetShaderStorageBlockBinding(const MG_State::GLState::ProgramObject& program, GLuint blockIndex) {
        auto& cache = GetProgramResourceCache(program);
        if (blockIndex >= cache.storageBlocks.size()) {
            return 0;
        }
        return cache.storageBlocks[blockIndex].binding;
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        if (!pVulkanRenderer) {
            return;
        }
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferfi called with null GL context");
        pVulkanRenderer->ClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        if (!pVulkanRenderer) {
            return;
        }
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferfv called with null GL context");
        pVulkanRenderer->ClearBufferfv(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        if (!pVulkanRenderer) {
            return;
        }
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferuiv called with null GL context");
        pVulkanRenderer->ClearBufferuiv(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        if (!pVulkanRenderer) {
            return;
        }
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferiv called with null GL context");
        pVulkanRenderer->ClearBufferiv(buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, const GLfloat* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearNamedFramebufferfv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearNamedFramebufferfv called with null GL context");
        pVulkanRenderer->ClearNamedFramebufferfv(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, GLfloat depth, GLint stencil) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearNamedFramebufferfi called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearNamedFramebufferfi called with null GL context");
        pVulkanRenderer->ClearNamedFramebufferfi(framebuffer, buffer, drawbuffer, depth, stencil);
    }

    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElementsIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElementsIndirect called with null GL context");
        pVulkanRenderer->MultiDrawElementsIndirectCount(mode, type, indirect, 0, drawcount, stride);
    }
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawArraysIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawArraysIndirect called with null GL context");

        if (drawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        const auto* commandBytes = ResolveIndirectCommandBytes(
            indirect,
            static_cast<SizeT>(stride) * static_cast<SizeT>(drawcount - 1) + sizeof(DrawArraysIndirectCommand),
            "MultiDrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        for (GLsizei i = 0; i < drawcount; ++i) {
            DrawArraysIndirectCommand cmd{};
            std::memcpy(&cmd, commandBytes + static_cast<SizeT>(i) * stride, sizeof(cmd));
            if (cmd.count == 0 || cmd.instanceCount == 0) {
                continue;
            }

            DrawCmd payload{};
            payload.mode = mode;
            payload.params.vertexCount = cmd.count;
            payload.params.instanceCount = cmd.instanceCount;
            payload.params.firstVertex = cmd.first;
            payload.params.firstInstance = cmd.baseInstance;
            pVulkanRenderer->DrawArrays(payload);
        }
    }
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElementsIndirectCount called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElementsIndirectCount called with null GL context");
        pVulkanRenderer->MultiDrawElementsIndirectCount(mode, type, indirect, drawcount, maxdrawcount, stride);
    }
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawArraysIndirectCount called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawArraysIndirectCount called with null GL context");

        if (maxdrawcount <= 0) {
            return;
        }
        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E("MultiDrawArraysIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!parameterBuffer || drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E("MultiDrawArraysIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        parameterBuffer->MarkPersistentMappedRangeDirty();
        const auto parameterData = parameterBuffer->GetDataReadOnly();
        if (!parameterData) {
            MGLOG_E("MultiDrawArraysIndirectCount skipped: CPU fallback cannot read parameter buffer");
            return;
        }

        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterData->data() + drawcount, sizeof(actualDrawCount));
        actualDrawCount = std::min<Uint32>(actualDrawCount, static_cast<Uint32>(maxdrawcount));
        MultiDrawArraysIndirect(mode, indirect, static_cast<GLsizei>(actualDrawCount), stride);
    }
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        (void)start;
        (void)end;
        DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        (void)start;
        (void)end;
        DrawElements(mode, count, type, indices);
    }
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsInstancedBaseVertexBaseInstance called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElementsInstancedBaseVertexBaseInstance called with null GL context");

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = instancecount;
        payload.params.firstIndex = 0;
        payload.params.vertexOffset = basevertex;
        payload.params.firstInstance = static_cast<Int32>(baseinstance);
        pVulkanRenderer->DrawElements(payload);
    }
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, basevertex, 0);
    }
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, 0, baseinstance);
    }
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, 0, 0);
    }
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElementsIndirect called with null GL context");

        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E("DrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawElementsIndirectCommand), "DrawElementsIndirect");
        if (!commandBytes) {
            return;
        }

        DrawElementsIndirectCommand cmd{};
        std::memcpy(&cmd, commandBytes, sizeof(cmd));
        if (cmd.count == 0 || cmd.instanceCount == 0) {
            return;
        }

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = static_cast<SizeT>(cmd.firstIndex) * indexSize;
        payload.indexBufferView.indexByteSize = static_cast<SizeT>(cmd.count) * indexSize;
        payload.params.indexCount = cmd.count;
        payload.params.instanceCount = cmd.instanceCount;
        payload.params.firstIndex = 0;
        payload.params.vertexOffset = cmd.baseVertex;
        payload.params.firstInstance = static_cast<Int32>(cmd.baseInstance);
        pVulkanRenderer->DrawElements(payload);
    }
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArraysInstancedBaseInstance called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawArraysInstancedBaseInstance called with null GL context");

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.vertexCount = count;
        payload.params.instanceCount = instancecount;
        payload.params.firstVertex = first;
        payload.params.firstInstance = baseinstance;
        pVulkanRenderer->DrawArrays(payload);
    }
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawArraysInstancedBaseInstance(mode, first, count, instancecount, 0);
    }
    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArraysIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawArraysIndirect called with null GL context");

        const auto* commandBytes =
            ResolveIndirectCommandBytes(indirect, sizeof(DrawArraysIndirectCommand), "DrawArraysIndirect");
        if (!commandBytes) {
            return;
        }

        DrawArraysIndirectCommand cmd{};
        std::memcpy(&cmd, commandBytes, sizeof(cmd));
        if (cmd.count == 0 || cmd.instanceCount == 0) {
            return;
        }

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.vertexCount = cmd.count;
        payload.params.instanceCount = cmd.instanceCount;
        payload.params.firstVertex = cmd.first;
        payload.params.firstInstance = cmd.baseInstance;
        pVulkanRenderer->DrawArrays(payload);
    }
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyTexImage2D called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::CopyTexImage2D called with null GL context");
        pVulkanRenderer->CopyTexSubImage2D(target, level, 0, 0, x, y, width, height);
    }
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyTexSubImage2D called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::CopyTexSubImage2D called with null GL context");
        pVulkanRenderer->CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }
    void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyImageSubData called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::CopyImageSubData called with null GL context");
        pVulkanRenderer->CopyImageSubData(srcTexture, srcTarget, srcLevel, srcX, srcY, srcZ,
                                          dstTexture, dstTarget, dstLevel, dstX, dstY, dstZ,
                                          srcWidth, srcHeight, srcDepth);
    }
    void GenerateMipmap(GLenum target) {
        if (!pVulkanRenderer) {
            return;
        }
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::GenerateMipmap called with null GL context");
        pVulkanRenderer->GenerateMipmap(target);
    }

    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DispatchCompute called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DispatchCompute called with null GL context");
        pVulkanRenderer->DispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void DispatchComputeIndirect(GLintptr indirect) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DispatchComputeIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DispatchComputeIndirect called with null GL context");
        pVulkanRenderer->DispatchComputeIndirect(indirect);
    }

    void MemoryBarrier(GLbitfield barriers) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MemoryBarrier called with null VulkanRenderer");
        pVulkanRenderer->MemoryBarrier(barriers);
    }

    void MemoryBarrierByRegion(GLbitfield barriers) {
        MemoryBarrier(barriers);
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        (void)unit;
        (void)texture;
        (void)level;
        (void)layered;
        (void)layer;
        (void)access;
        (void)format;
    }

    void GetIntegeri_v(GLenum target, GLuint index, GLint* data) {
        if (!data) return;
        switch (target) {
        case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
            if (!pVulkanRenderer || index >= 3) {
                *data = 0;
                return;
            }
            *data = static_cast<GLint>(
                pVulkanRenderer->GetPhysicalDevice().properties.limits.maxComputeWorkGroupCount[index]);
            return;
        case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
            if (!pVulkanRenderer || index >= 3) {
                *data = 0;
                return;
            }
            *data = static_cast<GLint>(
                pVulkanRenderer->GetPhysicalDevice().properties.limits.maxComputeWorkGroupSize[index]);
            return;
        case GL_SHADER_STORAGE_BUFFER_BINDING: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            *data = obj ? static_cast<GLint>(obj->GetExternalIndex()) : 0;
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_START: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            *data = static_cast<GLint>(point.GetRange().start);
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_SIZE: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            if (!obj) {
                *data = 0;
                return;
            }
            const auto& range = point.GetRange();
            const auto start = std::min(range.start, obj->GetSize());
            const auto end = std::min(range.end, obj->GetSize());
            *data = static_cast<GLint>(end - start);
            return;
        }
        case GL_IMAGE_BINDING_NAME:
        case GL_IMAGE_BINDING_LEVEL:
        case GL_IMAGE_BINDING_LAYERED:
        case GL_IMAGE_BINDING_LAYER:
        case GL_IMAGE_BINDING_ACCESS:
        case GL_IMAGE_BINDING_FORMAT: {
            if (index >= MG_State::GLState::TextureState::MAX_TEXTURE_IMAGE_UNITS) {
                *data = 0;
                return;
            }
            auto& imageBinding = MG_State::pGLContext->GetImageTextureBinding(static_cast<Int>(index));
            if (target == GL_IMAGE_BINDING_NAME) {
                *data = imageBinding.Texture ? static_cast<GLint>(imageBinding.Texture->GetExternalIndex()) : 0;
            } else if (target == GL_IMAGE_BINDING_LEVEL) {
                *data = imageBinding.Level;
            } else if (target == GL_IMAGE_BINDING_LAYERED) {
                *data = imageBinding.Layered;
            } else if (target == GL_IMAGE_BINDING_LAYER) {
                *data = imageBinding.Layer;
            } else if (target == GL_IMAGE_BINDING_ACCESS) {
                *data = static_cast<GLint>(imageBinding.Access);
            } else {
                *data = static_cast<GLint>(imageBinding.Format);
            }
            return;
        }
        default:
            *data = 0;
            return;
        }
    }

    void GetInteger64i_v(GLenum target, GLuint index, GLint64* data) {
        if (!data) return;
        switch (target) {
        case GL_SHADER_STORAGE_BUFFER_START: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            *data = static_cast<GLint64>(point.GetRange().start);
            return;
        }
        case GL_SHADER_STORAGE_BUFFER_SIZE: {
            auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::ShaderStorage, index);
            auto& obj = point.GetBoundObject();
            if (!obj) {
                *data = 0;
                return;
            }
            const auto& range = point.GetRange();
            const auto start = std::min(range.start, obj->GetSize());
            const auto end = std::min(range.end, obj->GetSize());
            *data = static_cast<GLint64>(end - start);
            return;
        }
        default:
            *data = 0;
            return;
        }
    }

    void GetProgramiv(GLuint program, GLenum pname, GLint* params) {
        if (!params) return;
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject) {
            params[0] = 0;
            return;
        }
        switch (pname) {
        case GL_COMPUTE_WORK_GROUP_SIZE: {
            auto& cache = GetProgramResourceCache(*programObject);
            params[0] = cache.computeWorkGroupSize[0];
            params[1] = cache.computeWorkGroupSize[1];
            params[2] = cache.computeWorkGroupSize[2];
            return;
        }
        default:
            params[0] = 0;
            return;
        }
    }

    void GetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params) {
        if (!params) return;
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject) return;
        auto& cache = GetProgramResourceCache(*programObject);
        switch (pname) {
        case GL_ACTIVE_RESOURCES:
            *params = GetProgramResourceActiveCount(*programObject, programInterface, cache);
            return;
        case GL_MAX_NAME_LENGTH:
            *params = GetProgramResourceMaxNameLength(*programObject, programInterface, cache);
            return;
        case GL_MAX_NUM_ACTIVE_VARIABLES:
            if (programInterface == GL_SHADER_STORAGE_BLOCK) {
                SizeT maxCount = 0;
                for (const auto& block : cache.storageBlocks) {
                    maxCount = std::max(maxCount, block.activeVariables.size());
                }
                *params = static_cast<GLint>(maxCount);
            } else if (programInterface == GL_UNIFORM_BLOCK) {
                GLint maxCount = 0;
                const Int activeBlocks = programObject->GetActiveUniformBlocksCount();
                for (Int index = 0; index < activeBlocks; ++index) {
                    maxCount = std::max(maxCount, programObject->GetUniformBlockActiveUniformCount(index));
                }
                *params = maxCount;
            } else {
                *params = 0;
            }
            return;
        default:
            *params = 0;
            return;
        }
    }

    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        if (!name) return GL_INVALID_INDEX;
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject) return GL_INVALID_INDEX;
        auto& cache = GetProgramResourceCache(*programObject);
        const String resourceName = name;
        if (programInterface == GL_SHADER_STORAGE_BLOCK) {
            return GetShaderStorageBlockIndex(*programObject, name);
        }
        if (programInterface == GL_BUFFER_VARIABLE) {
            const auto it = std::find_if(cache.bufferVariables.begin(), cache.bufferVariables.end(),
                [&](const BufferVariableResource& var) { return var.name == resourceName; });
            return it == cache.bufferVariables.end()
                ? GL_INVALID_INDEX
                : static_cast<GLuint>(std::distance(cache.bufferVariables.begin(), it));
        }
        if (programInterface == GL_UNIFORM_BLOCK) {
            return programObject->GetUniformBlockIndex(name);
        }
        if (programInterface == GL_UNIFORM) {
            const Int activeUniformIndex = programObject->GetActiveUniformIndex(resourceName);
            return activeUniformIndex >= 0 ? static_cast<GLuint>(activeUniformIndex) : GL_INVALID_INDEX;
        }
        if (programInterface == GL_PROGRAM_INPUT) {
            return FindProgramInputIndex(*programObject, resourceName);
        }
        if (programInterface == GL_PROGRAM_OUTPUT) {
            return FindProgramOutputIndex(*programObject, resourceName);
        }
        return GL_INVALID_INDEX;
    }

    void GetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize,
                                GLsizei* length, GLchar* name) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject) return;
        auto& cache = GetProgramResourceCache(*programObject);
        if (programInterface == GL_SHADER_STORAGE_BLOCK && index < cache.storageBlocks.size()) {
            CopyResourceName(cache.storageBlocks[index].name, bufSize, length, name);
            return;
        }
        if (programInterface == GL_BUFFER_VARIABLE && index < cache.bufferVariables.size()) {
            CopyResourceName(cache.bufferVariables[index].name, bufSize, length, name);
            return;
        }
        if (programInterface == GL_UNIFORM_BLOCK && programObject->IsActiveUniformBlock(index)) {
            CopyResourceName(programObject->GetUniformBlockName(index), bufSize, length, name);
            return;
        }
        if (programInterface == GL_UNIFORM && index < programObject->GetUniformCount()) {
            CopyResourceName(programObject->GetActiveUniformName(index), bufSize, length, name);
            return;
        }
        if (programInterface == GL_PROGRAM_INPUT && index < static_cast<GLuint>(programObject->GetActiveAttributesCount())) {
            CopyResourceName(programObject->GetActiveAttribName(index), bufSize, length, name);
            return;
        }
        if (programInterface == GL_PROGRAM_OUTPUT &&
            index < static_cast<GLuint>(programObject->GetActiveFragmentOutputCount())) {
            CopyResourceName(programObject->GetActiveFragmentOutputName(index), bufSize, length, name);
            return;
        }
        if (length) *length = 0;
        if (name && bufSize > 0) name[0] = '\0';
    }

    void GetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                              const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject || !props || !params || bufSize <= 0) return;
        auto& cache = GetProgramResourceCache(*programObject);
        GLsizei written = 0;
        auto writeValue = [&](GLint value) {
            if (written < bufSize) {
                params[written++] = value;
            }
        };

        for (GLsizei propIndex = 0; propIndex < propCount; ++propIndex) {
            const GLenum prop = props[propIndex];
            if (programInterface == GL_SHADER_STORAGE_BLOCK && index < cache.storageBlocks.size()) {
                const auto& block = cache.storageBlocks[index];
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(block.name.size() + 1));
                    break;
                case GL_BUFFER_BINDING:
                    writeValue(static_cast<GLint>(block.binding));
                    break;
                case GL_BUFFER_DATA_SIZE:
                    writeValue(block.dataSize);
                    break;
                case GL_NUM_ACTIVE_VARIABLES:
                    writeValue(static_cast<GLint>(block.activeVariables.size()));
                    break;
                case GL_ACTIVE_VARIABLES:
                    for (const auto variable : block.activeVariables) writeValue(static_cast<GLint>(variable));
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else if (programInterface == GL_BUFFER_VARIABLE && index < cache.bufferVariables.size()) {
                const auto& var = cache.bufferVariables[index];
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(var.name.size() + 1));
                    break;
                case GL_TYPE:
                    writeValue(GL_FLOAT);
                    break;
                case GL_ARRAY_SIZE:
                    writeValue(1);
                    break;
                case GL_OFFSET:
                    writeValue(var.offset);
                    break;
                case GL_BLOCK_INDEX:
                    writeValue(static_cast<GLint>(var.blockIndex));
                    break;
                case GL_ARRAY_STRIDE:
                case GL_MATRIX_STRIDE:
                case GL_TOP_LEVEL_ARRAY_SIZE:
                case GL_TOP_LEVEL_ARRAY_STRIDE:
                case GL_IS_ROW_MAJOR:
                    writeValue(0);
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else if (programInterface == GL_UNIFORM_BLOCK &&
                       programObject->IsActiveUniformBlock(index)) {
                const auto activeVariables = GetUniformBlockActiveVariables(*programObject, index);
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(programObject->GetUniformBlockName(index).size() + 1));
                    break;
                case GL_BUFFER_BINDING:
                    writeValue(static_cast<GLint>(programObject->GetUniformBlockBinding(index)));
                    break;
                case GL_BUFFER_DATA_SIZE:
                    writeValue(static_cast<GLint>(programObject->GetUBOSizeAt(index)));
                    break;
                case GL_NUM_ACTIVE_VARIABLES:
                    writeValue(static_cast<GLint>(activeVariables.size()));
                    break;
                case GL_ACTIVE_VARIABLES:
                    for (const GLuint variableIndex : activeVariables) {
                        writeValue(static_cast<GLint>(variableIndex));
                    }
                    break;
                case GL_REFERENCED_BY_VERTEX_SHADER:
                    writeValue(programObject->IsUniformBlockReferencedByStage(index, EShLangVertex) ? GL_TRUE
                                                                                                     : GL_FALSE);
                    break;
                case GL_REFERENCED_BY_FRAGMENT_SHADER:
                    writeValue(programObject->IsUniformBlockReferencedByStage(index, EShLangFragment) ? GL_TRUE
                                                                                                       : GL_FALSE);
                    break;
                case GL_REFERENCED_BY_COMPUTE_SHADER:
                    writeValue(programObject->IsUniformBlockReferencedByStage(index, EShLangCompute) ? GL_TRUE
                                                                                                      : GL_FALSE);
                    break;
                case GL_REFERENCED_BY_GEOMETRY_SHADER:
                case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
                case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
                    writeValue(GL_FALSE);
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else if (programInterface == GL_UNIFORM && index < programObject->GetUniformCount()) {
                const auto& uniformName = programObject->GetActiveUniformName(index);
                const GLint location = programObject->GetUniformLocation(uniformName);
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(uniformName.size() + 1));
                    break;
                case GL_TYPE:
                    writeValue(static_cast<GLint>(programObject->GetActiveUniformType(index)));
                    break;
                case GL_ARRAY_SIZE:
                    writeValue(programObject->GetActiveUniformArraySize(index));
                    break;
                case GL_BLOCK_INDEX:
                    writeValue(programObject->GetActiveUniformBlockIndex(index));
                    break;
                case GL_LOCATION:
                    writeValue(location);
                    break;
                case GL_OFFSET:
                    writeValue(location >= 0 && programObject->IsValidUniformLocation(location)
                                   ? static_cast<GLint>(programObject->GetUniformOffset(location))
                                   : 0);
                    break;
                case GL_ARRAY_STRIDE:
                case GL_MATRIX_STRIDE:
                case GL_IS_ROW_MAJOR:
                case GL_TOP_LEVEL_ARRAY_SIZE:
                case GL_TOP_LEVEL_ARRAY_STRIDE:
                case GL_REFERENCED_BY_VERTEX_SHADER:
                case GL_REFERENCED_BY_FRAGMENT_SHADER:
                case GL_REFERENCED_BY_COMPUTE_SHADER:
                case GL_REFERENCED_BY_GEOMETRY_SHADER:
                case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
                case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
                    writeValue(0);
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else if (programInterface == GL_PROGRAM_INPUT &&
                       index < static_cast<GLuint>(programObject->GetActiveAttributesCount())) {
                const auto& resourceName = programObject->GetActiveAttribName(index);
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(resourceName.size() + 1));
                    break;
                case GL_TYPE:
                    writeValue(static_cast<GLint>(programObject->GetActiveAttribType(index)));
                    break;
                case GL_ARRAY_SIZE:
                    writeValue(programObject->GetActiveAttribArraySize(index));
                    break;
                case GL_LOCATION:
                    writeValue(programObject->GetAttributeLocation(resourceName));
                    break;
                case GL_REFERENCED_BY_VERTEX_SHADER:
                    writeValue(GL_TRUE);
                    break;
                case GL_REFERENCED_BY_FRAGMENT_SHADER:
                case GL_REFERENCED_BY_COMPUTE_SHADER:
                case GL_REFERENCED_BY_GEOMETRY_SHADER:
                case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
                case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
                case GL_IS_PER_PATCH:
                case GL_LOCATION_INDEX:
                    writeValue(0);
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else if (programInterface == GL_PROGRAM_OUTPUT &&
                       index < static_cast<GLuint>(programObject->GetActiveFragmentOutputCount())) {
                const auto& resourceName = programObject->GetActiveFragmentOutputName(index);
                switch (prop) {
                case GL_NAME_LENGTH:
                    writeValue(static_cast<GLint>(resourceName.size() + 1));
                    break;
                case GL_TYPE:
                    writeValue(static_cast<GLint>(programObject->GetFragmentOutputType(index)));
                    break;
                case GL_ARRAY_SIZE:
                    writeValue(programObject->GetActiveFragmentOutputArraySize(index));
                    break;
                case GL_LOCATION:
                    writeValue(programObject->GetFragmentOutputLocation(index));
                    break;
                case GL_LOCATION_INDEX:
                    writeValue(0);
                    break;
                case GL_REFERENCED_BY_FRAGMENT_SHADER:
                    writeValue(GL_TRUE);
                    break;
                case GL_REFERENCED_BY_VERTEX_SHADER:
                case GL_REFERENCED_BY_COMPUTE_SHADER:
                case GL_REFERENCED_BY_GEOMETRY_SHADER:
                case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
                case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
                case GL_IS_PER_PATCH:
                    writeValue(0);
                    break;
                default:
                    writeValue(0);
                    break;
                }
            } else {
                writeValue(0);
            }
        }
        if (length) *length = written;
    }

    GLint GetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject || !name) return -1;
        if (programInterface == GL_UNIFORM) {
            return programObject->GetUniformLocation(name);
        }
        if (programInterface == GL_PROGRAM_INPUT) {
            return programObject->GetAttributeLocation(name);
        }
        if (programInterface == GL_PROGRAM_OUTPUT) {
            return GetProgramOutputLocation(*programObject, name);
        }
        return -1;
    }

    GLint GetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject || !name) return -1;
        if (programInterface == GL_PROGRAM_OUTPUT) {
            return GetProgramOutputLocation(*programObject, name) >= 0 ? 0 : -1;
        }
        return -1;
    }

    void ShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject) return;
        auto& cache = GetProgramResourceCache(*programObject);
        const Int maxBindings = pActiveBackendObject
            ? pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBufferBindings
            : 0;
        if (storageBlockBinding >= static_cast<GLuint>(maxBindings)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("DirectVulkan", __func__, "Shader storage binding is out of range."));
            return;
        }
        if (storageBlockIndex >= cache.storageBlocks.size()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("DirectVulkan", __func__, "Shader storage block index is not active."));
            return;
        }
        cache.storageBlocks[storageBlockIndex].binding = storageBlockBinding;
    }
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ReadPixels called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ReadPixels called with null GL context");
        pVulkanRenderer->ReadPixels(x, y, width, height, format, type, pixels);
    }
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetTexImage called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::GetTexImage called with null GL context");
        pVulkanRenderer->GetTexImage(target, level, format, type, pixels);
    }
    void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture, TextureUploadTarget uploadTarget,
                         GLint level, GLenum format, GLenum type, GLsizei bufSize, GLvoid* pixels) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetTextureImage called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::GetTextureImage called with null GL context");
        pVulkanRenderer->GetTextureImage(texture, uploadTarget, level, format, type, bufSize, pixels);
    }

    void Clear(GLbitfield mask) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::Clear called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::Clear called with null GL context");
        pVulkanRenderer->Clear(mask);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArrays called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawArrays called with null GL context");

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.firstVertex = first;
        payload.params.vertexCount = count;

        pVulkanRenderer->DrawArrays(payload);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElements called with null GL context");

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = 1;

        pVulkanRenderer->DrawElements(payload);
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElements called with null GL context");

        // Vector<DrawElementCmd> cmds;
        // cmds.reserve(static_cast<SizeT>(drawcount));
        // for (GLsizei i = 0; i < drawcount; ++i) {
        //     if (count[i] == 0) {
        //         continue;
        //     }
        //
        //     DrawElementCmd payload{};
        //     payload.mode = mode;
        //     payload.firstVertex = 0;
        //     payload.indexCount = count[i];
        //     payload.indexType = type;
        //     payload.indexByteOffset = reinterpret_cast<SizeT>(indices[i]);
        //     cmds.push_back(payload);
        // }
        //
        // if (cmds.empty()) {
        //     return;
        // }
        // pVulkanRenderer->MultiDrawElements(cmds);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsBaseVertex called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElementsBaseVertex called with null GL context");
        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = 1;
        payload.params.firstIndex = 0;
        payload.params.vertexOffset = basevertex;
        payload.params.firstInstance = 0;
        pVulkanRenderer->DrawElements(payload);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElements called with null GL context");
        MultiDrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;

        // TODO: allocate draw cmd buf elsewhere
        static Vector<DrawIndexedCmdParam> params;
        params.clear();
        params.resize(drawcount);

        for (GLsizei i = 0; i < drawcount; ++i) {
            if (count[i] == 0) {
                continue;
            }

            // TODO: this index view needs a redesign, now there's a lotta redundant uploads

            payload.indexBufferView.indexByteOffset = 0;
            payload.indexBufferView.indexByteSize =
                    std::max(reinterpret_cast<SizeT>(indices[i]) + count[i] * MG_Util::GetGLTypeSize(type),
                             payload.indexBufferView.indexByteSize);

            auto& param = params[i];

            param.indexCount = count[i];
            param.instanceCount = 1;
            param.firstIndex = reinterpret_cast<SizeT>(indices[i]) / MG_Util::GetGLTypeSize(type);
            param.vertexOffset = basevertex[i];
            param.firstInstance = 0;
        }
        payload.drawCount = drawcount;
        payload.pParams = params.data();
        pVulkanRenderer->MultiDrawElements(payload);
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BlitFramebuffer called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::BlitFramebuffer called with null GL context");
        pVulkanRenderer->BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    }

    void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                              const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                              GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                              GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BlitNamedFramebuffer called with null VulkanRenderer");
        pVulkanRenderer->BlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0,
                                              dstY0, dstX1, dstY1, mask, filter);
    }

    void Present() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::Present called with null VulkanRenderer");
        pVulkanRenderer->Present();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
