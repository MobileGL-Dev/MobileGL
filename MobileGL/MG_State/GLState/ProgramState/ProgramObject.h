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
        ProgramObject(Uint externalIndex) : m_externalIndex(externalIndex), m_lifetimeId(AllocateLifetimeId()) {}
        bool ShaderIsAttached(const SharedPtr<ShaderObject>& shader);
        // GL-visible attachment: in the attach list and not pending detach (glDetachShader
        // defers the actual removal to the next link).
        Bool ShaderIsAttachedGLVisible(const SharedPtr<ShaderObject>& shader) const {
            const auto matches = [&shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); };
            if (std::none_of(m_shaders.begin(), m_shaders.end(), matches)) return false;
            return std::none_of(m_detachedShaders.begin(), m_detachedShaders.end(), matches);
        }
        bool AttachShader(const SharedPtr<ShaderObject>& shader);
        SizeT DetachShader(const SharedPtr<ShaderObject>& shader);
        SizeT RemoveShader(const SharedPtr<ShaderObject>& shader);
        void Link(Bool addDefaultFSIfMissingForRenderingPipelineProgram = false);
        void MarkAsDeleted();

        void SetExplicitVertexInLocation(Uint index, const char* name);
        void SetExplicitFragmentOutLocation(Uint index, const char* name);
        // Dual-source blend color index (glBindFragDataLocationIndexed). Takes effect on next link.
        void SetExplicitFragmentOutIndex(Uint colorIndex, const char* name);
        void SetMaxFragmentOutputColorNumber(Int maxDrawBuffers) {
            m_maxFragmentOutputColorNumber = maxDrawBuffers;
        }
        Int GetFragmentDataLocation(const char* name);
        // Bound color index for an active fragment output (0 by default), or -1 if name is not one.
        Int GetFragmentDataIndex(const char* name);

        Vector<SharedPtr<ShaderObject>>& GetAttachedShaders();
        const Vector<SharedPtr<ShaderObject>>& GetAttachedShaders() const;
        const String& GetInfoLog() const { return m_infoLog; }
        Int GetUniformMaxLength() const { return m_uniformNameMaxLength; }
        Uint GetUniformCount() const { return m_activeUniformCount; }
        Uint GetMaxUniformLocation() const { return m_maxUniformLocation; }
        Int GetUniformLocation(const String& name) const {
            const auto it = m_uniformLocations.find(name);
            if (it != m_uniformLocations.end()) return (Int)it->second;

            // Reflection stores GL-style names: an array uniform is keyed "arr[0]" (its base
            // location). A bare "arr" query resolves to that entry; an "arr[k]" query resolves
            // to base + k because DoReflection reserves one location per array element.
            if (name.empty()) return -1;
            if (name.back() != ']') {
                const auto suffixedIt = m_uniformLocations.find(name + "[0]");
                if (suffixedIt != m_uniformLocations.end()) return (Int)suffixedIt->second;
                return -1;
            }
            if (name.length() < 4) return -1;
            const SizeT bracket = name.rfind('[');
            // Require at least one digit between the brackets.
            if (bracket == String::npos || bracket + 1 >= name.length() - 1) return -1;
            Uint element = 0;
            for (SizeT i = bracket + 1; i < name.length() - 1; ++i) {
                if (name[i] < '0' || name[i] > '9') return -1;
                element = element * 10 + static_cast<Uint>(name[i] - '0');
                if (element > 0x0FFFFFFFu) return -1;
            }
            auto baseIt = m_uniformLocations.find(name.substr(0, bracket) + "[0]");
            if (baseIt == m_uniformLocations.end()) {
                // Legacy key without the "[0]" suffix (defensive; reflection normally
                // stores the suffixed form for arrays).
                baseIt = m_uniformLocations.find(name.substr(0, bracket));
                if (baseIt == m_uniformLocations.end()) return -1;
            }
            const Int base = (Int)baseIt->second;
            if (!IsValidUniformLocation(base)) return -1;
            const Int index = m_uniformIndexInTProgram[base];
            // "[k]" only addresses arrays ("scalar[0]" is not a uniform name), and only
            // in-range elements.
            const glslang::TType* type = m_program->getUniform(index).getType();
            if (type == nullptr || !type->isArray()) return -1;
            if (static_cast<GLint>(element) >= GetActiveUniformArraySize(index)) return -1;
            const Int location = base + (Int)element;
            if (!UniformLocationsAliasSameUniform(base, location)) return -1;
            return location;
        }

        // True when both locations are element slots of the same uniform variable.
        Bool UniformLocationsAliasSameUniform(Int a, Int b) const {
            if (!IsValidUniformLocation(a) || !IsValidUniformLocation(b)) return false;
            return m_uniformIndexInTProgram[a] == m_uniformIndexInTProgram[b];
        }

        Int GetActiveUniformIndex(const String& name) const {
            const Int uniformIndex = m_program->getUniformIndex(name.c_str());
            if (uniformIndex >= 0 && uniformIndex < m_activeUniformCount &&
                m_program->getUniform(uniformIndex).name == name) {
                return uniformIndex;
            }

            // Reflection stores an array uniform under "arr[0]"; accept the bare "arr"
            // spelling too. The reverse ("arr[0]" against a bare "arr" entry) is kept for
            // robustness against non-suffixed reflection entries.
            if (!name.empty() && name.back() != ']') {
                const String suffixedName = name + "[0]";
                const Int suffixedIndex = m_program->getUniformIndex(suffixedName.c_str());
                if (suffixedIndex >= 0 && suffixedIndex < m_activeUniformCount &&
                    m_program->getUniform(suffixedIndex).name == suffixedName) {
                    return suffixedIndex;
                }
                return -1;
            }

            if (name.length() <= 3 || name.compare(name.length() - 3, 3, "[0]") != 0) return -1;
            const String baseName = name.substr(0, name.length() - 3);
            const Int baseIndex = m_program->getUniformIndex(baseName.c_str());
            if (baseIndex < 0 || baseIndex >= m_activeUniformCount) return -1;
            return m_program->getUniform(baseIndex).name == baseName ? baseIndex : -1;
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

        // Number of active array elements (GL_UNIFORM_SIZE / GL_ARRAY_SIZE); 1 for a non-array.
        // glslang's TObjectReflection.size only carries the element count for a NON-block array; for
        // a block array member it reports 1, so take the count from the TType, which is authoritative
        // for both. GL 3.3 core uniforms are always sized.
        GLint GetActiveUniformArraySize(Uint index) const {
            const auto& uniform = m_program->getUniform(static_cast<Int>(index));
            const glslang::TType* type = uniform.getType();
            if (type != nullptr && type->isSizedArray()) {
                return type->getOuterArraySize();
            }
            return uniform.size < 1 ? 1 : uniform.size;
        }

        Int GetActiveUniformBlockIndex(Uint index) const {
            auto& uniform = m_program->getUniform(static_cast<Int>(index));
            return uniform.index;
        }

        // GL_UNIFORM_OFFSET: byte offset within the owning named block. glslang already reports -1
        // for a default-block uniform, which is exactly the spec value there.
        GLint GetActiveUniformOffset(Uint index) const {
            return m_program->getUniform(static_cast<Int>(index)).offset;
        }

        // GL_UNIFORM_ARRAY_STRIDE: byte stride of an array member in a named block; 0 for a non-array
        // block member; -1 for a default-block uniform (glslang yields arrayStride==0 there, so gate
        // on block membership for the spec-mandated -1). The stride itself is derived from the type
        // instead of glslang's reflected arrayStride: for an array nested inside a struct member,
        // glslang computes that field against the enclosing STRUCT's (unset) packing and reports a
        // tight std430-like stride (ivec2 a[7] -> 8), even though its own member offsets and the
        // generated SPIR-V lay the array out with std140 16-byte-rounded strides. MobileGL's UBO
        // layout is always std140, where every array element stride rounds up to a vec4.
        GLint GetActiveUniformArrayStride(Uint index) const {
            const auto& uniform = m_program->getUniform(static_cast<Int>(index));
            if (uniform.index < 0) return -1;
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isArray()) return 0;
            if (type->isMatrix()) {
                const bool rowMajor = GetActiveUniformIsRowMajor(index) != 0;
                const int vectors = rowMajor ? type->getMatrixRows() : type->getMatrixCols();
                return GetActiveUniformMatrixStride(index) * vectors;
            }
            return 16; // scalars and vectors: std140 rounds the element stride up to a vec4
        }

        // GL_UNIFORM_IS_ROW_MAJOR: 1 only for a row-major matrix in a named block, else 0. The
        // isMatrix() guard is required -- glslang stamps a block-level layout(row_major) onto
        // non-matrix members too, so a float/vec in a row_major block would otherwise report 1.
        // For the glslang build here a block-level layout(row_major) is also resolved onto each
        // matrix member's own qualifier (verified by GetActiveUniformsivRowMajorBlock), so the member
        // check suffices; the getUniformBlock() fallback is defensive for a config that instead leaves
        // an inheriting member's layoutMatrix == ElmNone.
        GLint GetActiveUniformIsRowMajor(Uint index) const {
            const auto& uniform = m_program->getUniform(static_cast<Int>(index));
            if (uniform.index < 0) return 0;
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isMatrix()) return 0;
            glslang::TLayoutMatrix layoutMatrix = type->getQualifier().layoutMatrix;
            if (layoutMatrix == glslang::ElmNone) {
                layoutMatrix = m_program->getUniformBlock(uniform.index).getType()->getQualifier().layoutMatrix;
            }
            return (layoutMatrix == glslang::ElmRowMajor) ? 1 : 0;
        }

        // GL_UNIFORM_MATRIX_STRIDE: byte stride between columns (col-major) / rows (row-major) of a
        // matrix in a named block; 0 for a non-matrix block member; -1 for a default-block uniform.
        // glslang exposes no matrix stride, so it is derived from the std140 rule -- each column/row
        // vector's base alignment rounded up to a vec4 (16 B). MobileGL's SPIR-V path lays every UBO
        // out as std140 (packed/shared are coerced), so this matches the offsets glslang reports. For
        // every GL 3.3 float matrix this evaluates to 16, independent of majorness.
        GLint GetActiveUniformMatrixStride(Uint index) const {
            const auto& uniform = m_program->getUniform(static_cast<Int>(index));
            if (uniform.index < 0) return -1;
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isMatrix()) return 0;
            glslang::TLayoutMatrix layoutMatrix = type->getQualifier().layoutMatrix;
            if (layoutMatrix == glslang::ElmNone) {
                layoutMatrix = m_program->getUniformBlock(uniform.index).getType()->getQualifier().layoutMatrix;
            }
            const bool rowMajor = (layoutMatrix == glslang::ElmRowMajor);
            const int strideVectorComponents = rowMajor ? type->getMatrixCols() : type->getMatrixRows();
            constexpr int scalarSize = 4; // GL 3.3 core uniform matrices are float
            const int vectorAlignment = (strideVectorComponents <= 1)   ? scalarSize
                                        : (strideVectorComponents == 2) ? 2 * scalarSize
                                                                        : 4 * scalarSize;
            return (vectorAlignment + 15) & ~15; // std140 round-up to a vec4
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
        // Sentinel for a uniform location without global-UBO backing storage (should not
        // survive linking: GenerateBinary falls back to tail-allocated scratch storage).
        static constexpr Uint kInvalidUniformOffset = ~0u;
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
        // Content version of the CPU-side global-UBO shadow: writers bump it so backends
        // can skip re-uploading an unchanged UBO on every draw. ~0u is reserved as the
        // backends' "never uploaded" sentinel, so skip over it on wrap.
        Uint32 GetUBOContentVersion() const { return m_uboContentVersion; }
        void MarkUBOContentDirty() {
            if (++m_uboContentVersion == ~0u) m_uboContentVersion = 0;
        }
        Uint32 GetBackendStateVersion() const { return m_backendStateVersion; }
        // Bumped only by (re)linking — lets backends detect that every piece of
        // link-derived reflection (locations, block order, UBO layout) is stale.
        Uint32 GetLinkVersion() const { return m_linkVersion; }

        // Content-hash memo for backends: avoids re-hashing the generated SPIR-V on every
        // draw. The memo is keyed by (backendStateVersion, flags); ResetLinkArtifacts and
        // the binding setters below invalidate it by bumping m_backendStateVersion.
        Bool GetBackendHashMemo(Uint flags, Uint64& outHash) const {
            if (m_backendHashMemoVersion != m_backendStateVersion) return false;
            for (const auto& slot : m_backendHashMemoSlots) {
                if (slot.valid && slot.flags == flags) {
                    outHash = slot.hash;
                    return true;
                }
            }
            return false;
        }
        void SetBackendHashMemo(Uint flags, Uint64 hash) const {
            if (m_backendHashMemoVersion != m_backendStateVersion) {
                for (auto& slot : m_backendHashMemoSlots) slot.valid = false;
                m_backendHashMemoVersion = m_backendStateVersion;
                m_backendHashMemoNextSlot = 0;
            }
            for (auto& slot : m_backendHashMemoSlots) {
                if (slot.valid && slot.flags == flags) {
                    slot.hash = hash;
                    return;
                }
            }
            auto& slot = m_backendHashMemoSlots[m_backendHashMemoNextSlot];
            slot.flags = flags;
            slot.hash = hash;
            slot.valid = true;
            m_backendHashMemoNextSlot = (m_backendHashMemoNextSlot + 1) % kBackendHashMemoSlotCount;
        }

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
            // Instances of an arrayed block are reflected as "Block[0]".."Block[N-1]";
            // a bare "Block" query resolves to the first instance per GL semantics.
            const String suffixedName = String(name) + "[0]";
            it = m_uniformBlockIndexByName.find(suffixedName);
            if (it != m_uniformBlockIndexByName.end()) return it->second;
            return 0xFFFFFFFFu; // GL_INVALID_INDEX
        }
        Bool IsActiveUniformBlock(Uint index) const {
            if (index >= GetActiveUniformBlocksCount()) return false;
            return true;
        }
        Uint GetUBOSizeAt(Uint index) const {
            if (!IsActiveUniformBlock(index)) return 0;
            // glslang reports the unpadded end offset of the last member, but a std140 block
            // (like a std140 struct) occupies a vec4-rounded size, and that is what the
            // backend compiles: ES drivers reject draws whose bound UBO range is smaller
            // than the block (a block ending in ivec3 reported 12 while the driver needs 16).
            return (m_program->getUniformBlock((Int)index).size + 15u) & ~15u;
        }

        const String& GetUniformBlockName(Uint index) const {
            auto& ubo = m_program->getUniformBlock((Int)index);
            return ubo.name;
        }

        // Uniform entries that belong to an arrayed uniform block are reflected once, against
        // the first instance ("Block[0]"); per GL semantics every other instance shares that
        // member set. Maps any instance's block index to the index owning the member entries.
        Uint GetUniformBlockMemberOwnerIndex(Uint index) const {
            const String& name = GetUniformBlockName(index);
            if (name.empty() || name.back() != ']') return index;
            const SizeT bracket = name.rfind('[');
            if (bracket == String::npos) return index;
            const auto it = m_uniformBlockIndexByName.find(name.substr(0, bracket) + "[0]");
            if (it != m_uniformBlockIndexByName.end()) return it->second;
            return index;
        }

        // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: derived from the same active-uniform scan that
        // fills GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, so the two queries always agree
        // (glslang's numMembers counts declared members, which diverges from the reflected
        // entry list for struct arrays and arrayed block instances).
        Int GetUniformBlockActiveUniformCount(Uint index) const {
            const Int ownerIndex = static_cast<Int>(GetUniformBlockMemberOwnerIndex(index));
            Int count = 0;
            for (Uint uniformIndex = 0; uniformIndex < m_activeUniformCount; ++uniformIndex) {
                if (GetActiveUniformBlockIndex(uniformIndex) == ownerIndex) ++count;
            }
            return count;
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

        // Transform feedback (GL 3.0 core: glTransformFeedbackVaryings applies on
        // the NEXT link; the linked snapshot below is what draws and queries see).
        struct XfbVarying {
            String name;
            GLenum type = GL_FLOAT;
            GLint size = 1;           // array element count
            Uint32 bufferIndex = 0;   // capture buffer slot
            Uint32 offsetBytes = 0;   // offset within the capture buffer
            Uint32 byteSize = 0;      // bytes captured per vertex for this varying
        };
        void SetTransformFeedbackVaryings(Vector<String>&& names, GLenum bufferMode) {
            m_requestedXfbVaryings = Move(names);
            m_requestedXfbBufferMode = bufferMode;
        }
        GLenum GetTransformFeedbackBufferMode() const { return m_xfbBufferMode; }
        SizeT GetTransformFeedbackVaryingCount() const { return m_xfbVaryings.size(); }
        const XfbVarying* GetTransformFeedbackVarying(SizeT index) const {
            return index < m_xfbVaryings.size() ? &m_xfbVaryings[index] : nullptr;
        }
        const Vector<XfbVarying>& GetTransformFeedbackVaryings() const { return m_xfbVaryings; }
        // Stride of one captured vertex in the given capture buffer slot.
        Uint32 GetTransformFeedbackStride(Uint32 bufferIndex) const {
            return bufferIndex < m_xfbStrides.size() ? m_xfbStrides[bufferIndex] : 0;
        }
        SizeT GetTransformFeedbackBufferCount() const { return m_xfbStrides.size(); }
        Int GetTransformFeedbackVaryingMaxLength() const { return m_xfbVaryingNameMaxLength; }

        Uint GetExternalIndex() const { return m_externalIndex; }
        // Globally-unique, never-reused id for this program object's lifetime. Unlike the GL
        // name (external index), which is freed to a LIFO list and immediately handed back by
        // the next glCreateProgram, this distinguishes a deleted-and-recreated program from the
        // original, so an identity cache can't false-hit on name recycling.
        Uint64 GetLifetimeId() const { return m_lifetimeId; }

    private:
        void ResetLinkArtifacts();
        void DoReflection();
        // Resolves the requested transform feedback varyings against the linked
        // vertex stage; fails the link (GL semantics) on unknown or duplicate
        // names or exceeded capture limits.
        Bool ResolveTransformFeedbackVaryings();
        void GenerateBinary();
        void WaitUntilGenerationCompleted() const;
        void AddDefaultFragmentShaderIfMissing();
        Bool ValidateFragmentOutputLocations();

        static Uint64 AllocateLifetimeId();

        const Uint m_externalIndex = 0;
        const Uint64 m_lifetimeId = 0;
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
        // Dual-source blend color index per output name (glBindFragDataLocationIndexed); snapshotted
        // into the linked map at link time, like the location maps above.
        UnorderedMap<String, Uint> m_explicitFragDataIndex;
        UnorderedMap<String, Uint> m_linkedFragDataIndex;
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

        // Backend-owned content-hash memo (see GetBackendHashMemo): valid only while
        // m_backendStateVersion matches. Several slots, not one: a backend may resolve the same
        // program under more than one compile-flag set within a frame (surface rotation, and the
        // explicit-LOD sampling variant), and a single slot would then miss on every lookup and
        // re-hash the program's whole SPIR-V once per draw.
        static constexpr SizeT kBackendHashMemoSlotCount = 4;
        struct BackendHashMemoSlot {
            Uint64 hash = 0;
            Uint flags = 0;
            Bool valid = false;
        };
        mutable Array<BackendHashMemoSlot, kBackendHashMemoSlotCount> m_backendHashMemoSlots{};
        mutable SizeT m_backendHashMemoNextSlot = 0;
        mutable Uint32 m_backendHashMemoVersion = ~0u;
        Uint32 m_uboContentVersion = 0;
        Uint32 m_linkVersion = 0;

        // Transform feedback: request (applies at next link) and linked snapshot.
        Vector<String> m_requestedXfbVaryings;
        GLenum m_requestedXfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        Vector<XfbVarying> m_xfbVaryings;
        Vector<Uint32> m_xfbStrides;
        GLenum m_xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        Int m_xfbVaryingNameMaxLength = 0;
    };
} // namespace MobileGL::MG_State::GLState
