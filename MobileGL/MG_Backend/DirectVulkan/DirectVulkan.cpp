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
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include "MG_Util/Miscellany/IndexGenerator.h"
#include <atomic>
#include <bit>
#include <cstring>
#include <spirv_reflect.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // Leak-at-exit storage; see GlobalObjects.cpp.
    UniquePtr<VulkanRenderer>& pVulkanRenderer = *new UniquePtr<VulkanRenderer>();

    namespace {
        // Generation of the live VulkanRenderer instance, mirroring
        // DirectGLES's g_syncContextGeneration. BackendObject_DirectVulkan
        // bumps it (BumpRendererGeneration) wherever pVulkanRenderer is reset
        // or recreated. Fence and timer-query handles are stamped with the
        // generation they were created under: a stale stamp means the frame
        // serials and query-pool slots the handle refers to belong to a
        // destroyed renderer and must never be dereferenced against the
        // current one (a new renderer restarts its frame-serial counter and
        // reuses pool indices). Atomic because handles may be polled from a
        // thread other than the EGL thread that recreates the renderer.
        std::atomic<Uint64> g_rendererGeneration{1};
    } // namespace

    Uint64 GetRendererGeneration() {
        return g_rendererGeneration.load(std::memory_order_acquire);
    }

    void BumpRendererGeneration() {
        g_rendererGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

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
            // Lifetime id of the program the cached reflection belongs to. GL names are
            // recycled (IndexGenerator hands freed indices straight back), and a
            // recreated program's backendStateVersion restarts at the same small values,
            // so the version alone can collide; the never-reused lifetime id makes the
            // slot's ownership unambiguous.
            Uint64 programLifetimeId = 0;
            Uint32 backendStateVersion = 0;
            // glShaderStorageBlockBinding deliberately does NOT bump the backend state
            // version, and the pipeline composite is unnamed so the in-place patch in
            // DirectVulkan::ShaderStorageBlockBinding can never reach its slot - the
            // mirror replay bumps only the program's block-binding version. Without this
            // key the composite's slot kept serving the pre-rebind block.binding.
            Uint32 blockBindingVersion = 0;
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

        // Keyed by GL program name so the freed-name reuse in IndexGenerator bounds the
        // map at the peak-simultaneous-program high-water mark; each slot's ownership is
        // checked against the program's lifetime id before it is served (see
        // GetProgramResourceCache). Cleared wholesale at EGL teardown via
        // ClearProgramResourceCaches.
        UnorderedMap<GLuint, ProgramResourceCache> g_programResourceCaches;

        void ClearReadPixelsOutput(GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
            if (!pixels || width <= 0 || height <= 0) {
                return;
            }
            const auto inputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
            const auto inputType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);
            const SizeT size = MG_Util::CalculateInputTextureImageSize(inputFormat, inputType,
                                                                       IntVec3(width, height, 1));
            if (size > 0) {
                std::memset(pixels, 0, size);
            }
        }

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
            const Uint64 programLifetimeId = program.GetLifetimeId();
            const Uint32 backendStateVersion = program.GetBackendStateVersion();
            const Uint32 blockBindingVersion = program.GetBlockBindingVersion();
            // The lifetime id must match too: a new program that reuses a deleted
            // program's name and happens to land on the same backendStateVersion (both
            // count from zero) would otherwise be served the dead program's reflection.
            if (cache.programLifetimeId == programLifetimeId &&
                cache.backendStateVersion == backendStateVersion &&
                (!cache.storageBlocks.empty() || !cache.bufferVariables.empty())) {
                if (cache.blockBindingVersion != blockBindingVersion) {
                    // Only the block bindings moved (glShaderStorageBlockBinding, or the
                    // pipeline composite's mirror replay - neither touches the backend
                    // state version): the reflection itself is unchanged, so re-apply the
                    // overrides by name instead of re-running spirv-reflect. Overrides
                    // only ever accumulate, so a block without one still holds its
                    // declared binding.
                    for (auto& block : cache.storageBlocks) {
                        const Int rebound = program.GetShaderStorageBlockBindingOverride(block.name);
                        if (rebound >= 0) block.binding = static_cast<Uint32>(rebound);
                    }
                    cache.blockBindingVersion = blockBindingVersion;
                }
                return cache;
            }

            cache = {};
            cache.programLifetimeId = programLifetimeId;
            cache.backendStateVersion = backendStateVersion;
            cache.blockBindingVersion = blockBindingVersion;

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
                    // glShaderStorageBlockBinding survives every rebuild of this cache: the
                    // authoritative record of a rebound block lives on the program (it is what
                    // GL_BUFFER_BINDING reports), and only the shader's declared binding is
                    // recoverable from the SPIR-V. Without this, any unrelated state-version
                    // bump would silently revert the block to its declared binding.
                    const Int rebound = program.GetShaderStorageBlockBindingOverride(blockName);
                    if (rebound >= 0) block.binding = static_cast<Uint32>(rebound);
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
            if (!MG_State::pGLContext->ValidateProgramName(program)) {
                return nullptr;
            }
            auto& programObject = MG_State::pGLContext->GetProgramObject(program);
            return programObject.get();
        }

        const Uint8* ResolveIndirectCommandBytes(const void* indirect, SizeT requiredBytes, const char* label) {
            auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            if (drawBuffer) {
                drawBuffer->SyncPersistentMappedRange();
                const SizeT commandOffset = reinterpret_cast<SizeT>(indirect);
                if (drawBuffer->MappedData() == nullptr || commandOffset + requiredBytes > drawBuffer->GetSize()) {
                    MGLOG_E_ONCE("%s skipped: invalid GL_DRAW_INDIRECT_BUFFER binding or range", label);
                    return nullptr;
                }
                return drawBuffer->MappedData() + commandOffset;
            }

            if (!indirect) {
                MGLOG_E_ONCE("%s skipped: indirect pointer is null", label);
                return nullptr;
            }

            return reinterpret_cast<const Uint8*>(indirect);
        }

    } // namespace

    void ClearProgramResourceCaches() {
        // Called from EGL teardown while the backend's m_eglStateMutex is held; GL
        // calls are serialized in this codebase (contexts migrate threads but never
        // run concurrently), so no other thread can be inside the unsynchronized map.
        // Live programs in another context self-heal: their entry rebuilds from the
        // retained generated SPIR-V on the next resource query.
        g_programResourceCaches.clear();
    }

    GLuint GetShaderStorageBlockIndex(const MG_State::GLState::ProgramObject& program, const String& name) {
        auto& cache = GetProgramResourceCache(program);
        auto find = [&cache](const String& key) {
            return std::find_if(cache.storageBlocks.begin(), cache.storageBlocks.end(),
                [&](const StorageBlockResource& block) { return block.name == key; });
        };
        auto it = find(name);
        if (it == cache.storageBlocks.end()) {
            // Cache names are normalized (NormalizeDescriptorName drops the array suffix), so
            // an arrayed block that GL enumerates per element - "B[0]", "B[1]" - is one entry
            // here, spelled "B". Retry against the bare name before giving up.
            const auto bracket = name.rfind('[');
            if (bracket == String::npos || name.empty() || name.back() != ']') return GL_INVALID_INDEX;
            it = find(name.substr(0, bracket));
            if (it == cache.storageBlocks.end()) return GL_INVALID_INDEX;
        }
        return static_cast<GLuint>(std::distance(cache.storageBlocks.begin(), it));
    }

    GLuint GetShaderStorageBlockBinding(const MG_State::GLState::ProgramObject& program, GLuint blockIndex) {
        auto& cache = GetProgramResourceCache(program);
        if (blockIndex >= cache.storageBlocks.size()) {
            return 0;
        }
        return cache.storageBlocks[blockIndex].binding;
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferfi called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferfi called with null GL context");
        pVulkanRenderer->ClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferfv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferfv called with null GL context");
        pVulkanRenderer->ClearBufferfv(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferuiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferuiv called with null GL context");
        pVulkanRenderer->ClearBufferuiv(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearBufferiv called with null GL context");
        pVulkanRenderer->ClearBufferiv(buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, const GLfloat* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearNamedFramebufferfv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearNamedFramebufferfv called with null GL context");
        pVulkanRenderer->ClearNamedFramebufferfv(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, const GLint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearNamedFramebufferiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearNamedFramebufferiv called with null GL context");
        pVulkanRenderer->ClearNamedFramebufferiv(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                  GLint drawbuffer, const GLuint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearNamedFramebufferuiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::ClearNamedFramebufferuiv called with null GL context");
        pVulkanRenderer->ClearNamedFramebufferuiv(framebuffer, buffer, drawbuffer, value);
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
        pVulkanRenderer->MultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    }
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawArraysIndirect called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawArraysIndirect called with null GL context");

        if (drawcount <= 0) {
            return;
        }

        // With a bound GL_DRAW_INDIRECT_BUFFER the command parameters may be GPU-written
        // (e.g. by a compute shader), so consume them natively on the GPU.
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (drawBuffer) {
            pVulkanRenderer->MultiDrawArraysIndirect(mode, indirect, drawcount, stride);
            return;
        }

        if (stride == 0) {
            stride = sizeof(DrawArraysIndirectCommand);
        }
        if (stride < static_cast<GLsizei>(sizeof(DrawArraysIndirectCommand))) {
            MGLOG_E_ONCE("MultiDrawArraysIndirect skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        // No indirect buffer bound: the pointer refers to client memory.
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
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: stride %d is smaller than command size %zu",
                    stride, sizeof(DrawArraysIndirectCommand));
            return;
        }

        auto parameterBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!parameterBuffer || drawcount < 0 || static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: invalid GL_PARAMETER_BUFFER binding or range");
            return;
        }

        parameterBuffer->SyncPersistentMappedRange();
        if (parameterBuffer->MappedData() == nullptr) {
            MGLOG_E_ONCE("MultiDrawArraysIndirectCount skipped: CPU fallback cannot read parameter buffer");
            return;
        }

        Uint32 actualDrawCount = 0;
        std::memcpy(&actualDrawCount, parameterBuffer->MappedData() + drawcount, sizeof(actualDrawCount));
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
            MGLOG_E_ONCE("DrawElementsIndirect skipped: unsupported index type 0x%x", type);
            return;
        }

        // With a bound GL_DRAW_INDIRECT_BUFFER the command parameters may be GPU-written
        // (e.g. by a compute shader), so consume them natively on the GPU.
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (drawBuffer) {
            pVulkanRenderer->MultiDrawElementsIndirect(mode, type, indirect, 1, 0);
            return;
        }

        // No indirect buffer bound: the pointer refers to client memory.
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

        // With a bound GL_DRAW_INDIRECT_BUFFER the command parameters may be GPU-written
        // (e.g. by a compute shader), so consume them natively on the GPU.
        auto drawBuffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (drawBuffer) {
            pVulkanRenderer->MultiDrawArraysIndirect(mode, indirect, 1, 0);
            return;
        }

        // No indirect buffer bound: the pointer refers to client memory.
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
    void CopyImageSubData(const CopyImageEndpoint& src,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const CopyImageEndpoint& dst,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyImageSubData called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::CopyImageSubData called with null GL context");
        pVulkanRenderer->CopyImageSubData(src, srcTarget, srcLevel, srcX, srcY, srcZ,
                                          dst, dstTarget, dstLevel, dstX, dstY, dstZ,
                                          srcWidth, srcHeight, srcDepth);
    }
    void GenerateMipmap(GLenum target) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GenerateMipmap called with null VulkanRenderer");
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
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MemoryBarrier called with null GL context");
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
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetIntegeri_v called with null VulkanRenderer");
        switch (target) {
        case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
            if (index >= 3) {
                *data = 0;
                return;
            }
            *data = static_cast<GLint>(
                pVulkanRenderer->GetPhysicalDevice().properties.limits.maxComputeWorkGroupCount[index]);
            return;
        case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
            if (index >= 3) {
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

    void ShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding) {
        auto* programObject = TryGetDirectVulkanProgram(program);
        if (!programObject || storageBlockName == nullptr) return;
        const Int maxBindings = pActiveBackendObject
            ? pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBufferBindings
            : 0;
        if (storageBlockBinding >= static_cast<GLuint>(maxBindings)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("DirectVulkan", __func__, "Shader storage binding is out of range."));
            return;
        }
        // The frontend already validated that the name denotes an active block, and has
        // already recorded the new binding on the program - which is what reseeds this cache
        // whenever it is rebuilt. Writing the entry here as well keeps an ALREADY-BUILT cache
        // (the common case: the very next draw reads it) from having to be thrown away.
        //
        // Resolve the index BEFORE taking the reference, and bounds-check the way the
        // sibling getter does. GetShaderStorageBlockIndex re-enters GetProgramResourceCache,
        // which indexes g_programResourceCaches and can therefore insert - and that map is
        // open-addressed, so a rehash MOVES its entries and a reference taken before the
        // call is left dangling. Binding a program's storage block
        // while another program's entry was still absent from the cache was a reproducible
        // segfault (ProgramPipelineScenario's two storage-block cases, in one process).
        const GLuint blockIndex = GetShaderStorageBlockIndex(*programObject, storageBlockName);
        if (blockIndex == GL_INVALID_INDEX) return;
        auto& cache = GetProgramResourceCache(*programObject);
        if (blockIndex >= cache.storageBlocks.size()) return;
        cache.storageBlocks[blockIndex].binding = storageBlockBinding;
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

    // Vulkan has no LINE_LOOP topology; rewrite the draw as an indexed LINE_STRIP
    // whose synthesized index list revisits the first vertex at the end.
    static void DrawLineLoopAsIndexedStrip(const Vector<Uint32>& closedIndices, GLint basevertex) {
        DrawIndexedCmd payload{};
        payload.mode = GL_LINE_STRIP;
        payload.indexBufferView.indexType = GL_UNSIGNED_INT;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(closedIndices.data());
        payload.indexBufferView.indexByteSize = closedIndices.size() * sizeof(Uint32);
        payload.indexBufferView.forceClientMemory = true;
        payload.params.indexCount = static_cast<Uint32>(closedIndices.size());
        payload.params.instanceCount = 1;
        payload.params.vertexOffset = basevertex;
        pVulkanRenderer->DrawElements(payload);
    }

    // Resolve a DrawElements index list (bound element-array buffer or client
    // memory) into uint32 values with the loop-closing first index appended.
    static Bool BuildClosedLineLoopIndices(GLsizei count, GLenum type, const void* indices,
                                           Vector<Uint32>& outIndices) {
        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0 || count < 2) {
            return false;
        }
        const Uint8* indexBytes = nullptr;
        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        const auto& indexBufferShared = vao.GetIndexBufferBindingSlot().GetBoundObject();
        if (indexBufferShared != nullptr) {
            const SizeT offset = reinterpret_cast<SizeT>(indices);
            const SizeT bufferSize = indexBufferShared->GetSize();
            if (indexBufferShared->MappedData() == nullptr || offset > bufferSize ||
                static_cast<SizeT>(count) * indexSize > bufferSize - offset) {
                return false;
            }
            indexBufferShared->SyncPersistentMappedRange();
            indexBytes = indexBufferShared->MappedData() + offset;
        } else {
            indexBytes = static_cast<const Uint8*>(indices);
            if (indexBytes == nullptr) {
                return false;
            }
        }
        outIndices.resize(static_cast<SizeT>(count) + 1);
        for (GLsizei i = 0; i < count; ++i) {
            switch (indexSize) {
            case 1: outIndices[i] = indexBytes[i]; break;
            case 2: outIndices[i] = reinterpret_cast<const Uint16*>(indexBytes)[i]; break;
            default: outIndices[i] = reinterpret_cast<const Uint32*>(indexBytes)[i]; break;
            }
        }
        outIndices[count] = outIndices[0];
        return true;
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArrays called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawArrays called with null GL context");

        if (mode == GL_LINE_LOOP) {
            if (count < 2) {
                return;
            }
            Vector<Uint32> closedIndices(static_cast<SizeT>(count) + 1);
            for (GLsizei i = 0; i < count; ++i) {
                closedIndices[i] = static_cast<Uint32>(first + i);
            }
            closedIndices[count] = static_cast<Uint32>(first);
            DrawLineLoopAsIndexedStrip(closedIndices, 0);
            return;
        }

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.firstVertex = first;
        payload.params.vertexCount = count;

        pVulkanRenderer->DrawArrays(payload);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElements called with null GL context");

        if (mode == GL_LINE_LOOP) {
            Vector<Uint32> closedIndices;
            if (BuildClosedLineLoopIndices(count, type, indices, closedIndices)) {
                DrawLineLoopAsIndexedStrip(closedIndices, 0);
            }
            return;
        }

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = 1;

        pVulkanRenderer->DrawElements(payload);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawArrays called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawArrays called with null GL context");
        if (drawcount <= 0) {
            return;
        }

        MultiDrawCmd payload{};
        payload.mode = mode;

        // TODO: allocate draw cmd buf elsewhere
        static Vector<DrawCmdParam> params;
        params.clear();
        params.resize(drawcount);

        for (GLsizei i = 0; i < drawcount; ++i) {
            auto& param = params[i];
            param.vertexCount = count[i] > 0 ? static_cast<Uint32>(count[i]) : 0;
            param.instanceCount = 1;
            param.firstVertex = first[i] > 0 ? static_cast<Uint32>(first[i]) : 0;
            param.firstInstance = 0;
        }
        payload.drawCount = static_cast<Uint32>(drawcount);
        payload.pParams = params.data();
        pVulkanRenderer->MultiDrawArrays(payload);
    }

    // Shared body of glMultiDrawElements (basevertex == nullptr) and
    // glMultiDrawElementsBaseVertex: identical calls except for the per-draw
    // vertex offset, which VkMultiDrawIndexedInfoEXT / VkDrawIndexedIndirectCommand /
    // vkCmdDrawIndexed all carry natively.
    static void MultiDrawElementsImpl(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                      GLsizei drawcount, const GLint* basevertex) {
        if (drawcount <= 0) {
            return;
        }

        // With no element-array buffer bound, every indices[i] is a client pointer into a
        // separate CPU allocation, not an offset into one shared buffer. The batched payload
        // below cannot express that: it carries ONE index-buffer view for the whole batch and
        // turns each pointer into a firstIndex relative to it. Replay the sub-draws through
        // the single-draw entry point instead - it snapshots each client range into its own
        // transient slice, which is exactly what the unrolled draws this must match do.
        // (The batch used to be built this way; the shared-view rewrite that added
        // MultiDrawIndexedCmd left the client-memory shape addressing a view whose byte
        // offset is a hardcoded 0, so UploadAndBindIndexBuffer saw a null client pointer,
        // declined the whole batch and painted nothing.)
        const auto& vao = *MG_State::pGLContext->GetBoundVertexArray();
        if (vao.GetIndexBufferBindingSlot().GetBoundObject() == nullptr) {
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] <= 0) {
                    continue;
                }
                DrawElementsBaseVertex(mode, count[i], type, indices[i],
                                       basevertex != nullptr ? basevertex[i] : 0);
            }
            return;
        }

        MultiDrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;

        // Loop-invariant: the index type is fixed for the whole multi-draw, so resolve
        // its byte size once instead of twice per sub-draw (a cross-TU switch that
        // showed up in per-frame profiles of sodium-style 132x32 multi-draws). Index
        // sizes are 1/2/4, so the per-sub-draw offset division below reduces to a
        // shift - the hardware divide was the hottest instruction of this loop.
        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElements skipped: unsupported index type 0x%x", type);
            return;
        }
        const Uint32 indexSizeShift = static_cast<Uint32>(std::countr_zero(indexSize));

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
                    std::max(reinterpret_cast<SizeT>(indices[i]) + count[i] * indexSize,
                             payload.indexBufferView.indexByteSize);

            auto& param = params[i];

            param.indexCount = count[i];
            param.instanceCount = 1;
            param.firstIndex = reinterpret_cast<SizeT>(indices[i]) >> indexSizeShift;
            param.vertexOffset = basevertex != nullptr ? basevertex[i] : 0;
            param.firstInstance = 0;
        }
        payload.drawCount = drawcount;
        payload.pParams = params.data();
        pVulkanRenderer->MultiDrawElements(payload);
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElements called with null GL context");
        MultiDrawElementsImpl(mode, count, type, indices, drawcount, nullptr);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsBaseVertex called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::DrawElementsBaseVertex called with null GL context");
        if (mode == GL_LINE_LOOP) {
            Vector<Uint32> closedIndices;
            if (BuildClosedLineLoopIndices(count, type, indices, closedIndices)) {
                DrawLineLoopAsIndexedStrip(closedIndices, basevertex);
            }
            return;
        }
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
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElementsBaseVertex called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_State::pGLContext, "DirectVulkan::MultiDrawElementsBaseVertex called with null GL context");
        MultiDrawElementsImpl(mode, count, type, indices, drawcount, basevertex);
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

    namespace {
        // Backend fence handle: the queue-submission index captured at fence
        // creation (see VulkanRenderer::GetSyncPointSubmitIndex). The fence is
        // signaled once that submission's VkFence has been observed signaled,
        // so completion tracks the GPU itself rather than the frame-count
        // inference; MC 1.21.5's fence-paced ring buffers depend on this to
        // recycle their space instead of growing without bound.
        struct VulkanSyncObject {
            Uint64 submitIndex = 0;
            // Renderer generation the index was issued under (see
            // g_rendererGeneration). A stale generation reports the fence
            // signaled: renderer destruction waits for device idle, so the
            // old renderer's GPU work is long complete, and the index must
            // not be compared against the new renderer's restarted counter.
            Uint64 rendererGeneration = 0;
        };
    } // namespace

    BackendSyncHandle FenceSync() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::FenceSync called with null VulkanRenderer");
        return new VulkanSyncObject{pVulkanRenderer->GetSyncPointSubmitIndex(), GetRendererGeneration()};
    }

    GLenum ClientWaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        const auto* sync = static_cast<VulkanSyncObject*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClientWaitSync called with null VulkanRenderer");
        if (sync == nullptr || sync->rendererGeneration != GetRendererGeneration()) {
            return GL_ALREADY_SIGNALED;
        }
        if (pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex)) {
            return GL_ALREADY_SIGNALED;
        }
        // GL_SYNC_FLUSH_COMMANDS_BIT: flush regardless of timeout, so a
        // zero-timeout poll loop makes progress across calls - but only when
        // the sync's batch is still unsubmitted; flushing for an already
        // submitted fence cannot advance it and would split the frame's
        // render pass on every poll.
        if ((flags & GL_SYNC_FLUSH_COMMANDS_BIT) != 0) {
            pVulkanRenderer->FlushForSyncPoint(sync->submitIndex);
        }
        if (timeout == 0) {
            return pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex) ? GL_ALREADY_SIGNALED
                                                                             : GL_TIMEOUT_EXPIRED;
        }
        // Blocking wait: flush even without the flush bit - the sync's batch
        // can only be submitted from this thread, so waiting on an unflushed
        // fence would otherwise burn the full timeout with no chance of
        // success.
        return pVulkanRenderer->WaitForSubmitIndex(sync->submitIndex, timeout, /*flushIfPending=*/true)
                   ? GL_CONDITION_SATISFIED
                   : GL_TIMEOUT_EXPIRED;
    }

    void WaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        // Server-side waits are implicit: the single graphics queue executes
        // submissions in order, so later GPU work already observes everything
        // recorded before the fence.
        (void)handle;
        (void)flags;
        (void)timeout;
    }

    void DeleteSync(BackendSyncHandle handle) {
        delete static_cast<VulkanSyncObject*>(handle);
    }

    Bool GetSyncStatus(BackendSyncHandle handle) {
        const auto* sync = static_cast<VulkanSyncObject*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetSyncStatus called with null VulkanRenderer");
        if (sync == nullptr || sync->rendererGeneration != GetRendererGeneration()) {
            return true;
        }
        // Pure status read (glGetSynciv must not flush).
        return pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex);
    }

    namespace {
        // Backend timer-query handle: a TIME_ELAPSED span holds a begin and an
        // end timestamp record; a GL_TIMESTAMP one-shot holds only `end`. The
        // records are shared (SharedPtr) with the owning pool's pending list,
        // so deleting the query while results are still in flight is safe.
        struct VulkanTimerQuery {
            enum class Kind : Uint8 { Timer, Occlusion, XfbWritten, XfbGenerated };
            Kind kind = Kind::Timer;
            SharedPtr<VkTimerQueryManager::TimestampRecord> begin;
            SharedPtr<VkTimerQueryManager::TimestampRecord> end;
            // Kind::Occlusion - pool slots recorded between Begin/End; summed at result time.
            Vector<Uint32> occlusionSlots;
            // Renderer generation the records were written under (see
            // g_rendererGeneration). A stale generation resolves as available
            // with a final zero result: the records' pool indices and frame
            // serials refer to a destroyed renderer and must never be handed
            // to the current one. DeleteBackendQuery only frees the wrapper
            // (and, via the SharedPtrs, the records), never pool slots, so
            // stale queries are always safe to delete.
            Uint64 rendererGeneration = 0;
            // Kind::XfbGenerated - the frontend's paused-draw primitive counter when the
            // query began. VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT counts only what the
            // capture saw, so a draw made while the span was paused is invisible to it -
            // but GL_PRIMITIVES_GENERATED counts what the last vertex processing stage
            // emitted regardless. The delta closes that gap at result time.
            Uint64 pausedPrimitiveSnapshot = 0;
        };
    } // namespace

    Bool IsTimerQuerySupported() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::IsTimerQuerySupported called with null VulkanRenderer");
        return pVulkanRenderer->IsTimerQuerySupported();
    }

    BackendQueryHandle BeginTimeElapsedQuery() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginTimeElapsedQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->IsTimerQuerySupported()) {
            return nullptr;
        }
        auto begin = pVulkanRenderer->WriteTimerQueryTimestamp();
        if (!begin) {
            // Pool exhausted this frame; the frontend falls back on a null handle.
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->begin = std::move(begin);
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    void EndTimeElapsedQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndTimeElapsedQuery called with null VulkanRenderer");
        if (query == nullptr) {
            return;
        }
        if (query->rendererGeneration != GetRendererGeneration()) {
            // The span began under a renderer that has since been destroyed;
            // never write into the new renderer's pools on its behalf. The
            // query resolves as available with a zero result.
            return;
        }
        // May be null on pool exhaustion; the query then reads back as 0.
        query->end = pVulkanRenderer->WriteTimerQueryTimestamp();
    }

    BackendQueryHandle QueryCounterTimestamp() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::QueryCounterTimestamp called with null VulkanRenderer");
        if (!pVulkanRenderer->IsTimerQuerySupported()) {
            return nullptr;
        }
        auto record = pVulkanRenderer->WriteTimerQueryTimestamp();
        if (!record) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->end = std::move(record);
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    Bool IsQueryResultAvailable(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        // Degraded/stale handles report available; GetQueryResult64 then
        // resolves them with a final zero result.
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::IsQueryResultAvailable called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return true;
        }
        if (query->begin && !pVulkanRenderer->IsTimerQueryResultReady(*query->begin)) {
            return false;
        }
        if (query->end && !pVulkanRenderer->IsTimerQueryResultReady(*query->end)) {
            return false;
        }
        return true;
    }

    Bool GetQueryResult64(BackendQueryHandle handle, Bool wait, Uint64* outNanoseconds) {
        *outNanoseconds = 0;
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetQueryResult64 called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            // The records belong to a destroyed renderer: no real value can
            // ever be produced, so resolve with a final 0.
            return true;
        }
        if (query->kind == VulkanTimerQuery::Kind::Occlusion) {
            Uint64 samples = 0;
            if (!pVulkanRenderer->ResolveOcclusionQueryResult(query->occlusionSlots, samples)) {
                return false;
            }
            query->occlusionSlots.clear(); // slots are recycled by the resolve
            *outNanoseconds = samples;
            return true;
        }
        if (query->kind == VulkanTimerQuery::Kind::XfbWritten ||
            query->kind == VulkanTimerQuery::Kind::XfbGenerated) {
            Uint64 primitives = 0;
            if (!pVulkanRenderer->ResolveXfbQueryResult(query->occlusionSlots,
                                                        query->kind == VulkanTimerQuery::Kind::XfbGenerated,
                                                        primitives)) {
                return false;
            }
            if (query->kind == VulkanTimerQuery::Kind::XfbGenerated && MG_State::pGLContext != nullptr) {
                primitives += MG_State::pGLContext->GetTransformFeedbackPausedPrimitiveCounter() -
                              query->pausedPrimitiveSnapshot;
            }
            *outNanoseconds = primitives;
            return true;
        }
        // With wait, mirrors ClientWaitSync: a query ended this frame cannot
        // complete until Present submits the commands, so the wait refuses to
        // block on the current unsubmitted serial. Returning false keeps the
        // handle alive in the frontend; the query stays readable once a later
        // Present submits the frame.
        const auto ensureReady = [&](VkTimerQueryManager::TimestampRecord& record) {
            return wait ? pVulkanRenderer->WaitForTimerQueryResult(record)
                        : pVulkanRenderer->IsTimerQueryResultReady(record);
        };
        if (query->begin && query->end) {
            if (!ensureReady(*query->begin) || !ensureReady(*query->end)) {
                return false;
            }
            *outNanoseconds = pVulkanRenderer->GetTimerQueryElapsedNs(*query->begin, *query->end);
            return true;
        }
        if (query->end) {
            if (!ensureReady(*query->end)) {
                return false;
            }
            *outNanoseconds = pVulkanRenderer->GetTimerQueryTimestampNs(*query->end);
            return true;
        }
        // TIME_ELAPSED span that never got its end timestamp (pool
        // exhaustion): nothing further can arrive, resolve with a final 0.
        return true;
    }

    void DeleteBackendQuery(BackendQueryHandle handle) {
        delete static_cast<VulkanTimerQuery*>(handle);
    }

    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginXfbPrimitivesQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->StartXfbQueryCapture(generated ? 1u : 0u)) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->kind = generated ? VulkanTimerQuery::Kind::XfbGenerated : VulkanTimerQuery::Kind::XfbWritten;
        query->rendererGeneration = GetRendererGeneration();
        query->pausedPrimitiveSnapshot =
            MG_State::pGLContext ? MG_State::pGLContext->GetTransformFeedbackPausedPrimitiveCounter() : 0;
        return query;
    }

    void EndXfbPrimitivesQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndXfbPrimitivesQuery called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return;
        }
        pVulkanRenderer->StopXfbQueryCapture(
            query->kind == VulkanTimerQuery::Kind::XfbGenerated ? 1u : 0u, query->occlusionSlots);
    }

    BackendQueryHandle BeginOcclusionQuery() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginOcclusionQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->StartOcclusionQueryCapture()) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->kind = VulkanTimerQuery::Kind::Occlusion;
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    void EndOcclusionQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndOcclusionQuery called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return;
        }
        pVulkanRenderer->StopOcclusionQueryCapture(query->occlusionSlots);
    }

    Int64 GetGpuTimestampNs() {
        // Vulkan cannot synchronously sample the GPU clock: timestamps only
        // exist as vkCmdWriteTimestamp results read back later, and
        // VK_EXT_calibrated_timestamps is not wired up. Returning 0 tells the
        // frontend GL_TIMESTAMP getter to fall back.
        return 0;
    }

    void Present() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::Present called with null VulkanRenderer");
        pVulkanRenderer->Present();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
