// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "ShaderObject.h"

#include <MG_Util/Metrics/BufferMetrics.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>

namespace MobileGL::MG_State::GLState {
    class ProgramObject {
    public:
        ProgramObject(Uint externalIndex) : m_externalIndex(externalIndex) {}
        bool ShaderIsAttached(const SharedPtr<ShaderObject>& shader);
        bool AttachShader(const SharedPtr<ShaderObject>& shader);
        SizeT DetachShader(const SharedPtr<ShaderObject>& shader);
        SizeT RemoveShader(const SharedPtr<ShaderObject>& shader);
        void Link(Bool addDefaultFSIfMissingForRenderingPipelineProgram = false);
        void MarkAsDeleted();

        void SetExplicitVertexInLocation(Uint index, const char* name);
        void SetExplicitFragmentOutLocation(Uint index, const char* name);
        void SetMaxFragmentOutputColorNumber(Int maxDrawBuffers) {
            m_maxFragmentOutputColorNumber = maxDrawBuffers;
        }
        Int GetFragmentDataLocation(const char* name);

        Vector<SharedPtr<ShaderObject>>& GetAttachedShaders();
        const Vector<SharedPtr<ShaderObject>>& GetAttachedShaders() const;
        const String& GetInfoLog() const { return m_infoLog; }
        Int GetUniformMaxLength() const { return m_uniformNameMaxLength; }
        Uint GetUniformCount() const { return m_activeUniformCount; }
        Uint GetMaxUniformLocation() const { return m_maxUniformLocation; }
        Int GetUniformLocation(const String& name) const {
            const auto it = m_uniformLocations.find(name);
            if (it == m_uniformLocations.end()) return -1;
            return (Int)it->second;
        }

        Int GetActiveUniformIndex(const String& name) const {
            const Int uniformIndex = m_program->getUniformIndex(name.c_str());
            if (uniformIndex < 0 || uniformIndex >= m_activeUniformCount) return -1;
            return m_program->getUniform(uniformIndex).name == name ? uniformIndex : -1;
        }

        Bool IsValidUniformLocation(Int location) const {
            if (location < 0 || location > static_cast<Int>(m_maxUniformLocation)) return false;
            if (static_cast<SizeT>(location) >= m_uniformIndexInTProgram.size()) return false;
            const Int uniformIndexInProgram = m_uniformIndexInTProgram[location];
            return uniformIndexInProgram != glslang::TQualifier::layoutLocationEnd &&
                   uniformIndexInProgram >= 0 && uniformIndexInProgram < m_activeUniformCount;
        }

        GLenum GetUniformType(Uint location) const {
            auto& uniform = m_program->getUniform(m_uniformIndexInTProgram[location]);
            return uniform.glDefineType;
        }

        GLenum GetActiveUniformType(Uint index) const {
            auto& uniform = m_program->getUniform(static_cast<Int>(index));
            return uniform.glDefineType;
        }

        GLint GetActiveUniformArraySize(Uint index) const {
            auto& uniform = m_program->getUniform(static_cast<Int>(index));
            return uniform.size;
        }

        Int GetActiveUniformBlockIndex(Uint index) const {
            auto& uniform = m_program->getUniform(static_cast<Int>(index));
            return uniform.index;
        }

        const glslang::TType* GetUniformTType(Uint location) const {
            auto& uniform = m_program->getUniform(m_uniformIndexInTProgram[location]);
            return uniform.getType();
        }

        Bool IsUniformOpaqueAtLocation(Uint location) const { return GetUniformTType(location)->isOpaque(); }

        const String& GetUniformName(Uint location) const {
            auto& uniform = m_program->getUniform(m_uniformIndexInTProgram[location]);
            return uniform.name;
        }

        const String& GetActiveUniformName(Uint index) const {
            auto& uniform = m_program->getUniform(static_cast<Int>(index));
            return uniform.name;
        }
        Uint GetUniformOffset(Uint location) const { return m_uniformOffsets[location]; }
        Uint GetUniformSizesInBytes(Uint location) const { return MG_Util::GetGLTypeSize(GetUniformType(location)); }

        Int GetAttributeLocation(const String& name) {
            const auto it = std::find(m_attribs.begin(), m_attribs.end(), name);
            return (it == m_attribs.end()) ? -1 : (Int)std::distance(m_attribs.begin(), it);
        }
        Uint32 GetActiveAttributeLocationMask() const {
            Uint32 mask = 0;
            const SizeT count = std::min<SizeT>(m_attribs.size(), 32);
            for (SizeT index = 0; index < count; ++index) {
                if (!m_attribs[index].empty()) {
                    mask |= (1u << index);
                }
            }
            return mask;
        }
        Uint32 GetActiveFragmentOutputLocationMask() const {
            if (!m_program) {
                return 0;
            }

            Uint32 mask = 0;
            const Int outputCount = m_program->getNumPipeOutputs();
            for (Int index = 0; index < outputCount; ++index) {
                const Int location = static_cast<Int>(m_program->getPipeOutput(index).layoutLocation());
                if (location >= 0 && location < 32) {
                    mask |= (1u << location);
                }
            }
            return mask;
        }
        Int GetActiveFragmentOutputCount() const {
            return m_program ? m_program->getNumPipeOutputs() : 0;
        }
        const String& GetActiveFragmentOutputName(Uint index) const {
            MOBILEGL_ASSERT(m_program != nullptr, "ProgramObject::GetActiveFragmentOutputName: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(m_program->getNumPipeOutputs()),
                            "ProgramObject::GetActiveFragmentOutputName: index=%u out of range", index);
            return m_program->getPipeOutput(static_cast<Int>(index)).name;
        }
        Int GetFragmentOutputLocation(Uint index) const {
            MOBILEGL_ASSERT(m_program != nullptr, "ProgramObject::GetFragmentOutputLocation: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(m_program->getNumPipeOutputs()),
                            "ProgramObject::GetFragmentOutputLocation: index=%u out of range",
                            index);
            return static_cast<Int>(m_program->getPipeOutput(static_cast<Int>(index)).layoutLocation());
        }
        GLint GetActiveFragmentOutputArraySize(Uint index) const {
            MOBILEGL_ASSERT(m_program != nullptr, "ProgramObject::GetActiveFragmentOutputArraySize: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(m_program->getNumPipeOutputs()),
                            "ProgramObject::GetActiveFragmentOutputArraySize: index=%u out of range", index);
            return m_program->getPipeOutput(static_cast<Int>(index)).size;
        }
        GLenum GetFragmentOutputType(Uint index) const {
            MOBILEGL_ASSERT(m_program != nullptr, "ProgramObject::GetFragmentOutputType: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(m_program->getNumPipeOutputs()),
                            "ProgramObject::GetFragmentOutputType: index=%u out of range",
                            index);
            return m_program->getPipeOutput(static_cast<Int>(index)).glDefineType;
        }
        GLenum GetAttribType(Uint index) const { return m_attribTypes[index]; }
        const String& GetAttribName(Uint index) const { return m_attribs[index]; }
        GLenum GetActiveAttribType(Uint index) const { return m_program->getPipeInput(static_cast<Int>(index)).glDefineType; }
        GLint GetActiveAttribArraySize(Uint index) const { return m_program->getPipeInput(static_cast<Int>(index)).size; }
        const String& GetActiveAttribName(Uint index) const { return m_program->getPipeInput(static_cast<Int>(index)).name; }
        void* MapUBO() { return m_globalUboScratch.data(); }
        const void* GetUBOData() const { return m_globalUboScratch.data(); }
        Uint GetUBOSize() const { return static_cast<Uint>(m_globalUboScratch.size()); }
        Uint32 GetBackendStateVersion() const { return m_backendStateVersion; }

        void SetUniformSamplerOrImageUnitIndex(Uint location, Int unit) {
            if (location >= m_uniformSamplerOrImageUnitIndex.size() ||
                m_uniformSamplerOrImageUnitIndex[location] == unit) {
                return;
            }
            m_uniformSamplerOrImageUnitIndex[location] = unit;
            ++m_backendStateVersion;
        }

        Int GetUniformSamplerOrImageUnitIndex(Uint location) const {
            return m_uniformSamplerOrImageUnitIndex[location];
        }

        Bool GetDeleteStatus() const { return m_deleteStatus; }
        Bool GetLinkStatus() const { return m_linkStatus; }
        Bool GetValidateStatus() const { return m_validateStatus; }
        Int GetActiveAtomicCounterCount() const { return m_program->getNumAtomicCounters(); }
        Int GetActiveAttributesCount() const { return m_program->getNumPipeInputs(); }
        Int GetActiveUniformBlocksCount() const { return m_program->getNumUniformBlocks(); }
        GLuint GetComputeLocalSize(Uint dim) const { return m_program->getLocalSize(static_cast<Int>(dim)); }
        Int GetActiveAttributesMaxLength() const { return m_attribInNameMaxLength; }
        Int GetActiveUniformBlocksMaxNameLength() const { return m_uniformBlockNameMaxLength; }
        Uint GetUniformBlockIndex(const char* name) const {
            auto it = m_uniformBlockIndexByName.find(name);
            if (it != m_uniformBlockIndexByName.end()) return it->second;
            return 0xFFFFFFFFu; // GL_INVALID_INDEX
        }
        Bool IsActiveUniformBlock(Uint index) const {
            if (index >= GetActiveUniformBlocksCount()) return false;
            return true;
        }
        Uint GetUBOSizeAt(Uint index) const {
            if (!IsActiveUniformBlock(index)) return 0;
            return m_program->getUniformBlock((Int)index).size;
        }

        const String& GetUniformBlockName(Uint index) const {
            auto& ubo = m_program->getUniformBlock((Int)index);
            return ubo.name;
        }

        Int GetUniformBlockActiveUniformCount(Uint index) const {
            return m_program->getUniformBlock((Int)index).numMembers;
        }

        Bool IsUniformBlockReferencedByStage(Uint index, EShLanguage stage) const {
            const auto& ubo = m_program->getUniformBlock((Int)index);
            const auto stageMask = static_cast<EShLanguageMask>(1 << stage);
            return (ubo.stages & stageMask) != 0;
        }

        // Set by glUniformBlockBinding
        void SetUniformBlockBinding(Uint index, Uint binding) {
            if (index >= m_uniformBlockBinding.size() || m_uniformBlockBinding[index] == static_cast<Int>(binding)) {
                return;
            }
            m_uniformBlockBinding[index] = static_cast<Int>(binding);
            ++m_backendStateVersion;
        }

        Uint GetUniformBlockBinding(Uint index) const { return m_uniformBlockBinding[index]; }

        Vector<Vector<unsigned>>& GetGeneratedSpirv() { return m_generatedSpirv; }
        const Vector<Vector<unsigned>>& GetGeneratedSpirv() const { return m_generatedSpirv; }

        Int GetShaderIndexByStage(ShaderStage stage) const {
            auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [stage](const SharedPtr<ShaderObject>& shader) {
                return shader->GetShaderStage() == stage;
            });
            return it == m_shaders.end() ? -1 : (Int)std::distance(m_shaders.begin(), it);
        }

        Uint GetExternalIndex() const { return m_externalIndex; }

    private:
        void ResetLinkArtifacts();
        void DoReflection();
        void GenerateBinary();
        void WaitUntilGenerationCompleted() const;
        void AddDefaultFragmentShaderIfMissing();
        Bool ValidateFragmentOutputLocations();

        const Uint m_externalIndex = 0;
        Vector<SharedPtr<ShaderObject>> m_shaders;
        Vector<SharedPtr<ShaderObject>> m_detachedShaders; // Store detached shaders and remove on next link

        SharedPtr<glslang::TProgram> m_program;

        Vector<Vector<unsigned>> m_generatedSpirv;

        // Attributes (Vertex in)
        UnorderedMap<String, Uint> m_explicitAttribLocations;
        Vector<String> m_attribs;
        Vector<GLenum> m_attribTypes;

        // FragData (Frag out)
        UnorderedMap<String, Uint> m_explicitFragDataLocation;
        UnorderedMap<String, Uint> m_linkedFragDataLocation;
        Int m_maxFragmentOutputColorNumber = 8;

        // Uniforms
        UnorderedMap<String, Uint> m_uniformLocations;
        // Ordered by location,
        // aka. m_uniformIndexInTProgram[loc] == "uniform index of TProgram at location `loc`"
        Vector<Int> m_uniformIndexInTProgram;
        // ditto. Will be set at glUniform1i
        Vector<Int> m_uniformSamplerOrImageUnitIndex;
        UnorderedMap<String, Uint> m_explicitOpaqueUniformBindings;

        // Ordered by uniform block index
        // index is DIFFERENT from binding!!!
        //
        // Let's define UniformBlockIndex == the order at glslang getUniformBlock()
        // aka `i = glGetUniformBlockIndex(prog, "BlockName")` implies:
        // `prog->getUniformBlock(i) == "BlockName"`
        // These stuff are present for GL semantics, not for backend inspection
        // These may change after-link (because GL spec decided to have `glUniformBlockBinding`)
        UnorderedMap<String, Uint> m_uniformBlockIndexByName;
        Vector<Int> m_uniformBlockBinding;

        // Need to be reflected after linking of SPIR-V binary
        Vector<Uint> m_uniformOffsets;
        Vector<Uint> m_uniformSizesInBytes;
        Vector<Uint8> m_globalUboScratch;

        Uint m_activeUniformCount = 0;
        Uint m_maxUniformLocation = 0;
        Int m_uniformNameMaxLength = 0;
        Int m_attribInNameMaxLength = 0;
        Int m_uniformBlockNameMaxLength = 0;

        String m_infoLog;
        Bool m_deleteStatus = false;
        Bool m_linkStatus = false;
        Bool m_validateStatus = true;
        Uint32 m_backendStateVersion = 0;
    };
} // namespace MobileGL::MG_State::GLState
