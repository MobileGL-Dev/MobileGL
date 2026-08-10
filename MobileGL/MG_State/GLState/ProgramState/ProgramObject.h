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
    // The link job. Only ever held by SharedPtr here, so a forward declaration is enough -
    // ProgramLinkTask.h includes THIS header (it outputs a LinkArtifacts), so including it
    // back would be circular. The destructor is therefore out of line.
    class ProgramLinkTask;
    // Phase B of the same link: SPIR-V generation, spirv-opt and the global-UBO routing
    // tables. Chained behind the ProgramLinkTask, forward-declared for the same reason.
    class ProgramSpirvTask;

    class ProgramObject {
    public:
        ProgramObject(Uint externalIndex) : m_externalIndex(externalIndex), m_lifetimeId(AllocateLifetimeId()) {}
        // Cancel-not-join, exactly like ~ShaderObject: the link job owns its inputs, so an
        // in-flight link whose program just went away is safe to abandon where it stands.
        // Nothing can observe its result any more - this object was the only route to it.
        // Out of line because ProgramLinkTask is incomplete here.
        ~ProgramObject();
        ProgramObject(const ProgramObject&) = delete;
        ProgramObject& operator=(const ProgramObject&) = delete;

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
        const String& GetInfoLog() const { return Artifacts().infoLog; }
        // glCreateShaderProgramv folds the shader's compile log into the program's log, which
        // is the only place a caller can read it from once the shader name is gone.
        void AppendInfoLog(const String& text) {
            if (text.empty()) return;
            if (!Artifacts().infoLog.empty() && Artifacts().infoLog.back() != '\n') Artifacts().infoLog += '\n';
            Artifacts().infoLog += text;
        }
        Int GetUniformMaxLength() const { return Artifacts().uniformNameMaxLength; }
        Uint GetUniformCount() const { return Artifacts().activeUniformCount; }
        Uint GetMaxUniformLocation() const { return Artifacts().maxUniformLocation; }
        Int GetUniformLocation(const String& name) const {
            const auto it = Artifacts().uniformLocations.find(name);
            if (it != Artifacts().uniformLocations.end()) return (Int)it->second;

            // Reflection stores GL-style names: an array uniform is keyed "arr[0]" (its base
            // location). A bare "arr" query resolves to that entry; an "arr[k]" query resolves
            // to base + k because DoReflection reserves one location per array element.
            if (name.empty()) return -1;
            if (name.back() != ']') {
                const auto suffixedIt = Artifacts().uniformLocations.find(name + "[0]");
                if (suffixedIt != Artifacts().uniformLocations.end()) return (Int)suffixedIt->second;
                return -1;
            }
            if (name.length() < 4) return -1;
            // An array of arrays is keyed by its full "[0]"-terminated spelling
            // ("a[2][1][0]"), so a query that already ends in a subscript may still be the
            // NAME of an array rather than an element of one. Try that first; only then
            // treat the trailing subscript as an element index.
            {
                const auto arrayOfArraysIt = Artifacts().uniformLocations.find(name + "[0]");
                if (arrayOfArraysIt != Artifacts().uniformLocations.end()) return (Int)arrayOfArraysIt->second;
            }
            const SizeT bracket = name.rfind('[');
            // Require at least one digit between the brackets.
            if (bracket == String::npos || bracket + 1 >= name.length() - 1) return -1;
            Uint element = 0;
            for (SizeT i = bracket + 1; i < name.length() - 1; ++i) {
                if (name[i] < '0' || name[i] > '9') return -1;
                element = element * 10 + static_cast<Uint>(name[i] - '0');
                if (element > 0x0FFFFFFFu) return -1;
            }
            auto baseIt = Artifacts().uniformLocations.find(name.substr(0, bracket) + "[0]");
            if (baseIt == Artifacts().uniformLocations.end()) {
                // Legacy key without the "[0]" suffix (defensive; reflection normally
                // stores the suffixed form for arrays).
                baseIt = Artifacts().uniformLocations.find(name.substr(0, bracket));
                if (baseIt == Artifacts().uniformLocations.end()) return -1;
            }
            const Int base = (Int)baseIt->second;
            if (!IsValidUniformLocation(base)) return -1;
            const Int index = Artifacts().uniformIndexInTProgram[base];
            // "[k]" only addresses arrays ("scalar[0]" is not a uniform name), and only
            // in-range elements.
            const glslang::TType* type = Artifacts().program->getUniform(index).getType();
            if (type == nullptr || !type->isArray()) return -1;
            if (static_cast<GLint>(element) >= GetUniformArraySizeByTIndex(index)) return -1;
            const Int location = base + (Int)element;
            if (!UniformLocationsAliasSameUniform(base, location)) return -1;
            return location;
        }

        // True when both locations are element slots of the same uniform variable.
        Bool UniformLocationsAliasSameUniform(Int a, Int b) const {
            if (!IsValidUniformLocation(a) || !IsValidUniformLocation(b)) return false;
            return Artifacts().uniformIndexInTProgram[a] == Artifacts().uniformIndexInTProgram[b];
        }

        // ---- GL index <-> glslang TProgram index translation ----
        // The single relaxed parse enumerates artifacts GL must not see: every declared
        // default-block uniform (even dead ones) as a member of the synthesized
        // MGL_GLOBAL_UBO, and that block itself. DoReflection builds filtered GL-facing
        // index spaces; every public "index"-taking getter translates through them, so
        // GL and backend consumers keep seeing exactly the pre-P0a surface.
        Int TProgramUniformIndex(Uint glIndex) const {
            return Artifacts().glUniformIndexToTProgram[glIndex];
        }
        Int GlUniformIndexFromTProgram(Int tIndex) const {
            if (tIndex < 0 || tIndex >= static_cast<Int>(Artifacts().tProgramUniformIndexToGl.size())) return -1;
            return Artifacts().tProgramUniformIndexToGl[tIndex];
        }
        // GL uniform-block index -> glslang TProgram block index (the inverse of
        // GlBlockIndexFromTProgram). The interface-query layer needs it to reach block
        // properties glslang exposes but no typed getter here does.
        Int TProgramBlockIndex(Uint glBlockIndex) const {
            return glBlockIndex < Artifacts().glBlockIndexToTProgram.size()
                ? Artifacts().glBlockIndexToTProgram[glBlockIndex]
                : -1;
        }
        Int GlBlockIndexFromTProgram(Int tBlockIndex) const {
            if (tBlockIndex < 0 || tBlockIndex >= static_cast<Int>(Artifacts().tProgramBlockIndexToGl.size())) return -1;
            return Artifacts().tProgramBlockIndexToGl[tBlockIndex];
        }

        Int GetActiveUniformIndex(const String& name) const {
            const Int tProgramCount = static_cast<Int>(Artifacts().tProgramUniformIndexToGl.size());
            const Int uniformIndex = Artifacts().program->getUniformIndex(name.c_str());
            if (uniformIndex >= 0 && uniformIndex < tProgramCount &&
                Artifacts().program->getUniform(uniformIndex).name == name) {
                return GlUniformIndexFromTProgram(uniformIndex);
            }

            // Reflection stores an array uniform under "arr[0]"; accept the bare "arr"
            // spelling too. The reverse ("arr[0]" against a bare "arr" entry) is kept for
            // robustness against non-suffixed reflection entries.
            if (!name.empty() && name.back() != ']') {
                const String suffixedName = name + "[0]";
                const Int suffixedIndex = Artifacts().program->getUniformIndex(suffixedName.c_str());
                if (suffixedIndex >= 0 && suffixedIndex < tProgramCount &&
                    Artifacts().program->getUniform(suffixedIndex).name == suffixedName) {
                    return GlUniformIndexFromTProgram(suffixedIndex);
                }
                return -1;
            }

            if (name.length() <= 3 || name.compare(name.length() - 3, 3, "[0]") != 0) return -1;
            const String baseName = name.substr(0, name.length() - 3);
            const Int baseIndex = Artifacts().program->getUniformIndex(baseName.c_str());
            if (baseIndex < 0 || baseIndex >= tProgramCount) return -1;
            return Artifacts().program->getUniform(baseIndex).name == baseName ? GlUniformIndexFromTProgram(baseIndex)
                                                                     : -1;
        }

        Bool IsValidUniformLocation(Int location) const { return IsValidUniformLocation(Artifacts(), location); }

        GLenum GetUniformType(Uint location) const {
            auto& uniform = Artifacts().program->getUniform(Artifacts().uniformIndexInTProgram[location]);
            return uniform.glDefineType;
        }

        GLenum GetActiveUniformType(Uint index) const {
            auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            return uniform.glDefineType;
        }

        // Number of active array elements (GL_UNIFORM_SIZE / GL_ARRAY_SIZE); 1 for a non-array.
        // glslang's TObjectReflection.size only carries the element count for a NON-block array; for
        // a block array member it reports 1, so take the count from the TType, which is authoritative
        // for both. GL 3.3 core uniforms are always sized. Takes a TProgram uniform index (the space
        // the artifacts' uniformIndexInTProgram stores).
        GLint GetUniformArraySizeByTIndex(Int tIndex) const {
            return GetUniformArraySizeByTIndex(Artifacts(), tIndex);
        }

        GLint GetActiveUniformArraySize(Uint index) const {
            return GetUniformArraySizeByTIndex(TProgramUniformIndex(index));
        }

        Int GetActiveUniformBlockIndex(Uint index) const {
            auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            // Members of the synthesized global UBO are default-block uniforms to GL: -1.
            return GlBlockIndexFromTProgram(uniform.index);
        }

        // GL_UNIFORM_OFFSET: byte offset within the owning named block; -1 for a default-block
        // uniform. The relaxed parse gives global-UBO members real byte offsets, but GL must keep
        // seeing them as default-block uniforms, so gate on the GL-visible block index.
        GLint GetActiveUniformOffset(Uint index) const {
            const auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return -1;
            return uniform.offset;
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
            const auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return -1;
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
            const auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return 0;
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isMatrix()) return 0;
            glslang::TLayoutMatrix layoutMatrix = type->getQualifier().layoutMatrix;
            if (layoutMatrix == glslang::ElmNone) {
                layoutMatrix = Artifacts().program->getUniformBlock(uniform.index).getType()->getQualifier().layoutMatrix;
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
            const auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return -1;
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isMatrix()) return 0;
            glslang::TLayoutMatrix layoutMatrix = type->getQualifier().layoutMatrix;
            if (layoutMatrix == glslang::ElmNone) {
                layoutMatrix = Artifacts().program->getUniformBlock(uniform.index).getType()->getQualifier().layoutMatrix;
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
            auto& uniform = Artifacts().program->getUniform(Artifacts().uniformIndexInTProgram[location]);
            return uniform.getType();
        }

        Bool IsUniformOpaqueAtLocation(Uint location) const { return GetUniformTType(location)->isOpaque(); }

        const String& GetUniformName(Uint location) const {
            auto& uniform = Artifacts().program->getUniform(Artifacts().uniformIndexInTProgram[location]);
            return uniform.name;
        }

        const String& GetActiveUniformName(Uint index) const {
            auto& uniform = Artifacts().program->getUniform(TProgramUniformIndex(index));
            return uniform.name;
        }
        // Sentinel for a uniform location without global-UBO backing storage (should not
        // survive linking: GenerateBinary falls back to tail-allocated scratch storage).
        static constexpr Uint kInvalidUniformOffset = ~0u;
        // PHASE B (joins the SPIR-V job; see EnsureSpirvJoined).
        Uint GetUniformOffset(Uint location) const { return Spirv().uniformOffsets[location]; }
        Uint GetUniformSizesInBytes(Uint location) const { return MG_Util::GetGLTypeSize(GetUniformType(location)); }

        Int GetAttributeLocation(const String& name) {
            const auto it = std::find(Artifacts().attribs.begin(), Artifacts().attribs.end(), name);
            return (it == Artifacts().attribs.end()) ? -1 : (Int)std::distance(Artifacts().attribs.begin(), it);
        }
        Uint32 GetActiveAttributeLocationMask() const {
            Uint32 mask = 0;
            const SizeT count = std::min<SizeT>(Artifacts().attribs.size(), 32);
            for (SizeT index = 0; index < count; ++index) {
                if (!Artifacts().attribs[index].empty()) {
                    mask |= (1u << index);
                }
            }
            return mask;
        }
        Uint32 GetActiveFragmentOutputLocationMask() const {
            if (!Artifacts().program) {
                return 0;
            }

            Uint32 mask = 0;
            const Int outputCount = Artifacts().program->getNumPipeOutputs();
            for (Int index = 0; index < outputCount; ++index) {
                const Int location = static_cast<Int>(Artifacts().program->getPipeOutput(index).layoutLocation());
                if (location >= 0 && location < 32) {
                    mask |= (1u << location);
                }
            }
            return mask;
        }
        Int GetActiveFragmentOutputCount() const {
            return Artifacts().program ? Artifacts().program->getNumPipeOutputs() : 0;
        }
        const String& GetActiveFragmentOutputName(Uint index) const {
            MOBILEGL_ASSERT(Artifacts().program != nullptr, "ProgramObject::GetActiveFragmentOutputName: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().program->getNumPipeOutputs()),
                            "ProgramObject::GetActiveFragmentOutputName: index=%u out of range", index);
            return Artifacts().program->getPipeOutput(static_cast<Int>(index)).name;
        }
        Int GetFragmentOutputLocation(Uint index) const {
            MOBILEGL_ASSERT(Artifacts().program != nullptr, "ProgramObject::GetFragmentOutputLocation: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().program->getNumPipeOutputs()),
                            "ProgramObject::GetFragmentOutputLocation: index=%u out of range",
                            index);
            return static_cast<Int>(Artifacts().program->getPipeOutput(static_cast<Int>(index)).layoutLocation());
        }
        GLint GetActiveFragmentOutputArraySize(Uint index) const {
            MOBILEGL_ASSERT(Artifacts().program != nullptr, "ProgramObject::GetActiveFragmentOutputArraySize: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().program->getNumPipeOutputs()),
                            "ProgramObject::GetActiveFragmentOutputArraySize: index=%u out of range", index);
            return Artifacts().program->getPipeOutput(static_cast<Int>(index)).size;
        }
        GLenum GetFragmentOutputType(Uint index) const {
            MOBILEGL_ASSERT(Artifacts().program != nullptr, "ProgramObject::GetFragmentOutputType: program is null");
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().program->getNumPipeOutputs()),
                            "ProgramObject::GetFragmentOutputType: index=%u out of range",
                            index);
            return Artifacts().program->getPipeOutput(static_cast<Int>(index)).glDefineType;
        }
        GLenum GetAttribType(Uint index) const { return Artifacts().attribTypes[index]; }
        const String& GetAttribName(Uint index) const { return Artifacts().attribs[index]; }
        GLenum GetActiveAttribType(Uint index) const { return Artifacts().program->getPipeInput(static_cast<Int>(index)).glDefineType; }
        GLint GetActiveAttribArraySize(Uint index) const { return Artifacts().program->getPipeInput(static_cast<Int>(index)).size; }
        // The Vulkan-semantics parse reflects the vertex builtins under their SPIR-V names;
        // GL must keep reporting the GL spellings (glGetActiveAttrib and the program-input
        // resource queries enumerate builtins).
        static const String& NormalizeBuiltinPipeInputName(const String& name) {
            static const String kGlVertexId = "gl_VertexID";
            static const String kGlInstanceId = "gl_InstanceID";
            if (name == "gl_VertexIndex") return kGlVertexId;
            if (name == "gl_InstanceIndex") return kGlInstanceId;
            return name;
        }
        const String& GetActiveAttribName(Uint index) const {
            return NormalizeBuiltinPipeInputName(Artifacts().program->getPipeInput(static_cast<Int>(index)).name);
        }
        // PHASE B, all three (see EnsureSpirvJoined): the shadow buffer's layout is decided
        // by the OPTIMIZED SPIR-V, so it does not exist until the SPIR-V job has settled.
        void* MapUBO() { return Spirv().globalUboScratch.data(); }
        const void* GetUBOData() const { return Spirv().globalUboScratch.data(); }
        Uint GetUBOSize() const { return static_cast<Uint>(Spirv().globalUboScratch.size()); }
        // Content version of the CPU-side global-UBO shadow: writers bump it so backends
        // can skip re-uploading an unchanged UBO on every draw. ~0u is reserved as the
        // backends' "never uploaded" sentinel, so skip over it on wrap.
        Uint32 GetUBOContentVersion() const { return m_uboContentVersion; }
        void MarkUBOContentDirty() const {
            if (++m_uboContentVersion == ~0u) m_uboContentVersion = 0;
        }
        // ---- glUniform* inside the phase-A -> phase-B window ----
        //
        // True while the program is fully linked and fully queryable but its uniform shadow's
        // LAYOUT (which the optimized SPIR-V decides) does not exist yet. A non-opaque
        // glUniform* write in that window is RECORDED rather than joined, and replayed into
        // the shadow at the phase-B publish - so a pack that sets its uniforms immediately
        // after glLinkProgram never waits for SPIR-V.
        //
        // Nothing can observe the difference: the only route to those bytes is glGetUniform*
        // (and a draw), and both of those go through the phase-B gate, which replays first.
        // The OPAQUE branch of glUniform* is deliberately not buffered - a sampler unit is
        // phase-A state (uniformSamplerOrImageUnitIndex), so glUniform1i(samplerLoc, unit)
        // right after a link stays a zero-join operation, which is exactly what Iris does.
        Bool IsSpirvPending() const { return m_pendingSpirv != nullptr; }
        // Records one write. Returns false if it declined to buffer - the caller must then
        // perform the write directly (which joins). Declining is the pressure valve for an
        // application that writes megabytes of uniforms into a single pending window.
        Bool BufferUniformWrite(Uint location, SizeT byteOffsetInUniform, const void* source, SizeT byteSize);

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
            if (location >= Artifacts().uniformSamplerOrImageUnitIndex.size() ||
                Artifacts().uniformSamplerOrImageUnitIndex[location] == unit) {
                return;
            }
            Artifacts().uniformSamplerOrImageUnitIndex[location] = unit;
            ++m_backendStateVersion;
        }

        Int GetUniformSamplerOrImageUnitIndex(Uint location) const {
            return Artifacts().uniformSamplerOrImageUnitIndex[location];
        }

        Bool GetDeleteStatus() const { return m_deleteStatus; }
        Bool GetLinkStatus() const { return Artifacts().linkStatus; }
        // GL_PROGRAM_BINARY_RETRIEVABLE_HINT. MobileGL exposes no program binary format
        // (GL_NUM_PROGRAM_BINARY_FORMATS is 0), so the hint is pure state - which is all
        // ARB_get_program_binary requires of it.
        Bool GetBinaryRetrievableHint() const { return m_binaryRetrievableHint; }
        void SetBinaryRetrievableHint(Bool hint) { m_binaryRetrievableHint = hint; }
        // GL_PROGRAM_SEPARABLE (GL_ARB_separate_shader_objects): the program may supply a
        // subset of the stages of a program pipeline. Only takes effect on the next link,
        // which is why it is plain state here rather than something Link() consults.
        Bool GetSeparable() const { return m_separable; }
        void SetSeparable(Bool separable) { m_separable = separable; }
        // glProgramBinary always fails here (there is no format it could accept) and the
        // spec then requires the program's LINK_STATUS to read FALSE.
        void MarkLinkFailedByProgramBinary() {
            // Before anything reads m_artifacts: a pending link would otherwise publish its
            // (possibly successful) result over the failure this call is required to install
            // - and Artifacts() below would be the thing that let it. Cancel-not-join: GL
            // gives glProgramBinary no reason to wait for a link it is about to invalidate.
            CancelLink();
            BumpLinkObservableVersions();
            ResetLinkArtifacts(Artifacts());
            // ResetLinkArtifacts is a LinkArtifacts-only operation (the link body calls it on
            // its own block, where no phase-B output exists yet), so the phase-B half is
            // cleared here. CancelLink() above already dropped the pending SPIR-V job, so
            // this cannot be racing a publish.
            m_spirv = {};
            Artifacts().infoLog = "No program binary format is supported.";
        }
        Bool GetValidateStatus() const { return m_validateStatus; }
        Int GetActiveAtomicCounterCount() const { return Artifacts().program->getNumAtomicCounters(); }
        Int GetActiveAttributesCount() const { return Artifacts().program->getNumPipeInputs(); }
        // GL-visible uniform blocks only: the synthesized MGL_GLOBAL_UBO the relaxed parse
        // materializes for default-block uniforms is filtered out by DoReflection.
        Int GetActiveUniformBlocksCount() const { return static_cast<Int>(Artifacts().glBlockIndexToTProgram.size()); }
        GLuint GetComputeLocalSize(Uint dim) const { return Artifacts().program->getLocalSize(static_cast<Int>(dim)); }
        Int GetActiveAttributesMaxLength() const { return Artifacts().attribInNameMaxLength; }
        Int GetActiveUniformBlocksMaxNameLength() const { return Artifacts().uniformBlockNameMaxLength; }
        Uint GetUniformBlockIndex(const char* name) const {
            auto it = Artifacts().uniformBlockIndexByName.find(name);
            if (it != Artifacts().uniformBlockIndexByName.end()) return it->second;
            // Instances of an arrayed block are reflected as "Block[0]".."Block[N-1]";
            // a bare "Block" query resolves to the first instance per GL semantics.
            const String suffixedName = String(name) + "[0]";
            it = Artifacts().uniformBlockIndexByName.find(suffixedName);
            if (it != Artifacts().uniformBlockIndexByName.end()) return it->second;
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
            return (Artifacts().program->getUniformBlock(Artifacts().glBlockIndexToTProgram[index]).size + 15u) & ~15u;
        }

        const String& GetUniformBlockName(Uint index) const {
            auto& ubo = Artifacts().program->getUniformBlock(Artifacts().glBlockIndexToTProgram[index]);
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
            const auto it = Artifacts().uniformBlockIndexByName.find(name.substr(0, bracket) + "[0]");
            if (it != Artifacts().uniformBlockIndexByName.end()) return it->second;
            return index;
        }

        // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: derived from the same active-uniform scan that
        // fills GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, so the two queries always agree
        // (glslang's numMembers counts declared members, which diverges from the reflected
        // entry list for struct arrays and arrayed block instances).
        Int GetUniformBlockActiveUniformCount(Uint index) const {
            const Int ownerIndex = static_cast<Int>(GetUniformBlockMemberOwnerIndex(index));
            Int count = 0;
            for (Uint uniformIndex = 0; uniformIndex < Artifacts().activeUniformCount; ++uniformIndex) {
                if (GetActiveUniformBlockIndex(uniformIndex) == ownerIndex) ++count;
            }
            return count;
        }

        Bool IsUniformBlockReferencedByStage(Uint index, EShLanguage stage) const {
            const auto& ubo = Artifacts().program->getUniformBlock(Artifacts().glBlockIndexToTProgram[index]);
            const auto stageMask = static_cast<EShLanguageMask>(1 << stage);
            return (ubo.stages & stageMask) != 0;
        }

        // Set by glUniformBlockBinding
        void SetUniformBlockBinding(Uint index, Uint binding) {
            if (index >= Artifacts().uniformBlockBinding.size() || Artifacts().uniformBlockBinding[index] == static_cast<Int>(binding)) {
                return;
            }
            Artifacts().uniformBlockBinding[index] = static_cast<Int>(binding);
            ++m_backendStateVersion;
        }

        Uint GetUniformBlockBinding(Uint index) const { return Artifacts().uniformBlockBinding[index]; }

        // Set by glShaderStorageBlockBinding, keyed by the block's GL name rather than by any
        // index. A shader storage block has THREE index spaces - the frontend interface-query
        // enumeration, DirectVulkan's SPIR-V descriptor order and DirectGLES's real-driver
        // order - and the name is the only coordinate all three agree on. Absent from the map
        // means "never rebound", and the shader's declared binding still stands.
        void SetShaderStorageBlockBinding(const String& blockName, Uint binding) {
            Artifacts().shaderStorageBlockBinding[blockName] = static_cast<Int>(binding);
        }
        // -1 when the block has never been rebound. `blockName` is the interface-query
        // spelling; an arrayed block's elements ("B[0]", "B[1]") are separate GL resources
        // with separate bindings, so they are separate keys.
        Int GetShaderStorageBlockBindingOverride(const String& blockName) const {
            const auto it = Artifacts().shaderStorageBlockBinding.find(blockName);
            if (it != Artifacts().shaderStorageBlockBinding.end()) return it->second;
            // A backend that collapses an arrayed block down to one resource knows it only by
            // the bare block name; answer that with element zero's binding.
            const auto zeroth = Artifacts().shaderStorageBlockBinding.find(blockName + "[0]");
            return zeroth != Artifacts().shaderStorageBlockBinding.end() ? zeroth->second : -1;
        }
        // Every rebinding recorded so far, for a backend that has to REPLAY them onto a
        // driver program it just (re)built. Empty for the overwhelming majority of programs -
        // check .empty() before doing any per-block work.
        const UnorderedMap<String, Int>& GetShaderStorageBlockBindingOverrides() const {
            return Artifacts().shaderStorageBlockBinding;
        }

        // PHASE B (see EnsureSpirvJoined). Empty for a program whose SPIR-V job was
        // cancelled; GetSpirvStatus() below is how a backend tells that apart from a program
        // that never linked.
        Vector<Vector<unsigned>>& GetGeneratedSpirv() { return Spirv().generatedSpirv; }
        const Vector<Vector<unsigned>>& GetGeneratedSpirv() const { return Spirv().generatedSpirv; }
        // Whether phase B produced usable SPIR-V. Joins, like the four getters above: a
        // backend asks this exactly where it used to ask GetLinkStatus(), i.e. right before
        // it builds or draws with the program.
        Bool GetSpirvStatus() const { return Spirv().spirvStatus; }

        // The linked glslang reflection itself, for the ONE consumer that needs resource
        // lists no typed getter above exposes: the GL program-interface query layer
        // (MG_Impl/GLImpl/Program/ProgramInterface.cpp), which has to enumerate buffer
        // blocks, buffer variables, atomic counters and per-stage reference masks. Null
        // until a link has succeeded. Read through the join gate like everything else.
        const glslang::TProgram* GetReflection() const { return Artifacts().program.get(); }

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
            // Offset within the gap-free record a backend that cannot express the GL
            // layout captures into; see NeedsScatteredTransformFeedbackCapture.
            Uint32 packedOffsetBytes = 0;
        };

        // ---- P1: everything a link PRODUCES, in one movable block ----
        //
        // The membership rule is mechanical, not editorial: this is exactly the field list
        // ResetLinkArtifacts() clears (plus the four it forgot to - infoLog,
        // linkedFragDataLocation/Index and the geometry strip-capture pair - which are just
        // as much link output). Nothing else belongs here.
        //
        // Why a struct: once glLinkProgram runs on a worker (P1 stage 4) the worker writes
        // its OWN LinkArtifacts and the GL thread publishes it with a single move, instead
        // of thirty cross-thread field assignments. Until then this is a pure refactor.
        //
        // Access rule (invariant I5): the member below is private and reachable ONLY
        // through ProgramObject::Artifacts(), which calls EnsureLinkJoined() first. That is
        // what makes "every read of link output joins the pending link" a property the
        // compiler checks rather than a review item - a new reader cannot spell the field
        // without going through the gate.
        struct LinkArtifacts {
            SharedPtr<glslang::TProgram> program;

            // Attributes (Vertex in)
            Vector<String> attribs;
            Vector<GLenum> attribTypes;

            // FragData (Frag out): the per-link snapshot of the explicit request maps.
            UnorderedMap<String, Uint> linkedFragDataLocation;
            UnorderedMap<String, Uint> linkedFragDataIndex;

            // GL-facing index spaces (see the translation helpers above): GL active-uniform
            // index <-> glslang TProgram uniform index, GL uniform-block index <-> TProgram
            // block index. -1 marks a TProgram entry GL does not expose (dead default-block
            // uniforms swept into MGL_GLOBAL_UBO by the relaxed parse, and that block itself).
            Vector<Int> glUniformIndexToTProgram;
            Vector<Int> tProgramUniformIndexToGl;
            Vector<Int> glBlockIndexToTProgram;
            Vector<Int> tProgramBlockIndexToGl;
            // Per-link merged snapshot of the attached shaders' lexically extracted
            // layout(location = N) default-block uniform qualifiers (the relaxed parse drops
            // them from reflection; the DoReflection assigner restores them from here).
            UnorderedMap<String, Int> linkedExplicitUniformLocations;
            UnorderedMap<String, Uint> uniformLocations;
            // Ordered by location,
            // aka. uniformIndexInTProgram[loc] == "uniform index of TProgram at location `loc`"
            Vector<Int> uniformIndexInTProgram;
            // ditto. Will be set at glUniform1i
            Vector<Int> uniformSamplerOrImageUnitIndex;
            UnorderedMap<String, Uint> explicitOpaqueUniformBindings;

            // Ordered by uniform block index
            // index is DIFFERENT from binding!!!
            //
            // Let's define UniformBlockIndex == the order at glslang getUniformBlock()
            // aka `i = glGetUniformBlockIndex(prog, "BlockName")` implies:
            // `prog->getUniformBlock(i) == "BlockName"`
            // These stuff are present for GL semantics, not for backend inspection
            // These may change after-link (because GL spec decided to have `glUniformBlockBinding`)
            UnorderedMap<String, Uint> uniformBlockIndexByName;
            Vector<Int> uniformBlockBinding;
            // glShaderStorageBlockBinding overrides, keyed by GL block name. See
            // SetShaderStorageBlockBinding for why this one is by name and not by index.
            UnorderedMap<String, Int> shaderStorageBlockBinding;

            Uint activeUniformCount = 0;
            Uint maxUniformLocation = 0;
            Int uniformNameMaxLength = 0;
            Int attribInNameMaxLength = 0;
            Int uniformBlockNameMaxLength = 0;

            String infoLog;
            Bool linkStatus = false;

            // Transform feedback: the linked snapshot (the request lives outside, on the
            // GL-thread-owned side).
            Vector<XfbVarying> xfbVaryings;
            // The glTransformFeedbackVaryings request list exactly as this link consumed it,
            // INCLUDING the gl_NextBuffer / gl_SkipComponentsN pseudo-varyings that
            // xfbVaryings deliberately drops (they steer the capture layout and must never
            // reach a backend's varying list). GL_TRANSFORM_FEEDBACK_VARYING enumerates the
            // full request, pseudo-varyings and all, so the interface query needs its own copy.
            Vector<String> xfbInterfaceNames;
            Vector<Uint32> xfbStrides;
            Vector<Uint32> gsStripTriangles;
            Bool gsStripCaptureFixup = false;
            GLenum gsInputPrimitive = GL_NONE;
            GLenum xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
            Int xfbVaryingNameMaxLength = 0;
            Bool xfbNeedsScatteredCapture = false;
            Uint32 xfbPackedStride = 0;
        };

        // ---- everything phase B of a link produces, in one movable block ----
        //
        // The membership rule is the same mechanical one LinkArtifacts uses: this is exactly
        // what ProgramSpirvTask writes, which is what makes moving it THE publish. It is
        // deliberately NOT part of LinkArtifacts, and that separation is what routes the five
        // readers of SPIR-V-derived data through their own join gate by compiler rather than
        // by review - m_spirv is private and Spirv() is the only spelling that reaches it.
        //
        // Why these three and nothing else: `generatedSpirv` has no GL-thread reader at all
        // (every consumer is a backend draw/prepare path), and `uniformOffsets` +
        // `globalUboScratch` are the ONLY things glUniform*/glGetUniform* need that are
        // derived from the OPTIMIZED SPIR-V rather than from glslang reflection - spirv-opt
        // runs in place and can delete a uniform, or the whole global UBO, so the offsets
        // cannot be lifted out of glslang's reflection instead.
        struct SpirvArtifacts {
            Vector<Vector<unsigned>> generatedSpirv;
            // Byte offset of each uniform location inside globalUboScratch, or
            // kInvalidUniformOffset. Sized maxUniformLocation + 1 by the routing pass.
            Vector<Uint> uniformOffsets;
            Vector<Uint8> globalUboScratch;
            // False for a program whose SPIR-V was never produced (phase B cancelled at
            // teardown or by a relink) or whose optimizer run failed. GL has no way to
            // retract a LINK_STATUS it already reported true, so such a program stays
            // "linked" and every reflection answer it has given stays correct - it is simply
            // not drawable, which the backends already express through their link-status
            // gates.
            Bool spirvStatus = false;
        };

        // ---- artifacts-only helpers, shared with ProgramLinkTask ----
        // Static and taking the block explicitly, because from stage 4 the link BODY needs
        // them while its artifacts still live on the job node, not on any ProgramObject. The
        // member overloads above are the same functions read through the join gate.

        // Clears every field one link produces, EXCEPT infoLog, linkedFragDataLocation/Index
        // and the geometry strip-capture pair. That exception is load-bearing: the callers
        // that survive (glProgramBinary's mandated failure, and the link body's own mid-link
        // aborts) write infoLog immediately AFTER calling here. Link()'s prologue does not
        // use this at all - it assigns a whole default-constructed LinkArtifacts, where the
        // ordering is explicit and nothing is exempt.
        static void ResetLinkArtifacts(LinkArtifacts& artifacts);

        static Bool IsValidUniformLocation(const LinkArtifacts& artifacts, Int location) {
            if (location < 0 || location > static_cast<Int>(artifacts.maxUniformLocation)) return false;
            if (static_cast<SizeT>(location) >= artifacts.uniformIndexInTProgram.size()) return false;
            const Int uniformIndexInProgram = artifacts.uniformIndexInTProgram[location];
            return uniformIndexInProgram != glslang::TQualifier::layoutLocationEnd &&
                   uniformIndexInProgram >= 0 &&
                   uniformIndexInProgram < static_cast<Int>(artifacts.tProgramUniformIndexToGl.size());
        }

        // Number of active array elements (GL_UNIFORM_SIZE / GL_ARRAY_SIZE); 1 for a non-array.
        // glslang's TObjectReflection.size only carries the element count for a NON-block array; for
        // a block array member it reports 1, so take the count from the TType, which is authoritative
        // for both. GL 3.3 core uniforms are always sized. Takes a TProgram uniform index (the space
        // the artifacts' uniformIndexInTProgram stores).
        static GLint GetUniformArraySizeByTIndex(const LinkArtifacts& artifacts, Int tIndex) {
            const auto& uniform = artifacts.program->getUniform(tIndex);
            const glslang::TType* type = uniform.getType();
            if (type != nullptr && type->isSizedArray()) {
                return type->getOuterArraySize();
            }
            return uniform.size < 1 ? 1 : uniform.size;
        }

        // Blocks until a pending link has published its artifacts. Public because a few call
        // sites have to join without reading anything - see the explicit-join list (J1-J8) in
        // the P1 design. GL thread only.
        //
        // PHASE A ONLY. After this returns, LINK_STATUS and the whole GL query surface are
        // final and truthful, but the SPIR-V and the uniform shadow may still be in flight.
        void JoinLink() const { EnsureLinkJoined(); }

        // Both phases. The draw path uses this, and must: the backends sample lifetimeId /
        // backendStateVersion / the UBO content version OUTSIDE the gate, so a draw that
        // joined only phase A would sample a version, join phase B later inside the same draw
        // (through GetGeneratedSpirv), and memoize under a version the phase-B publish had
        // already superseded - the exact lost-invalidation hazard J1 exists to prevent.
        void JoinLinkAndSpirv() const { EnsureSpirvJoined(); }

        // Drops BOTH phases of a link that is still in flight, without waiting for either.
        // Called at the points
        // where the pending link's result stops being the answer to "what did this program
        // link to": a re-link supersedes it, glProgramBinary must force LINK_STATUS false,
        // and a destroyed program has no observers left.
        //
        // Deliberately NOT called by the "takes effect at the next link" setters
        // (glBindAttribLocation, glBindFragDataLocation(Indexed), glTransformFeedbackVaryings,
        // glProgramParameteri) NOR by glAttachShader/glDetachShader. Every one of those is
        // defined by GL to leave the CURRENT link result alone, and the pending link already
        // snapshotted its own inputs at enqueue, so it is computing exactly the answer GL
        // requires. Cancelling on any of them would make
        //   glLinkProgram(p); <setter>; glGetProgramiv(p, GL_LINK_STATUS)
        // report FALSE for a link that succeeded - and for the attach/detach pair it would
        // additionally break glCreateShaderProgramv, which detaches immediately after linking.
        void CancelLink();

        // MUST NOT JOIN - this is what GL_COMPLETION_STATUS_KHR reads when the extension
        // surface lands. "No job at all" counts as complete: there is nothing outstanding to
        // wait for.
        //
        // BOTH phases, deliberately: an application that polls GL_COMPLETION_STATUS_KHR and
        // then draws must not be told "done" while the SPIR-V is still being generated, or
        // the draw it was cleared for is the thing that blocks.
        Bool IsLinkComplete() const { return IsPhaseALinkComplete() && IsSpirvComplete(); }
        // Phase A alone, for the callers that only care about the query surface (and for the
        // tests that pin the two phases apart).
        Bool IsPhaseALinkComplete() const { return m_pendingLink == nullptr || IsPendingLinkTerminal(); }
        Bool IsSpirvComplete() const { return m_pendingSpirv == nullptr || IsPendingSpirvTerminal(); }

        void SetTransformFeedbackVaryings(Vector<String>&& names, GLenum bufferMode) {
            m_requestedXfbVaryings = Move(names);
            m_requestedXfbBufferMode = bufferMode;
        }
        GLenum GetTransformFeedbackBufferMode() const { return Artifacts().xfbBufferMode; }
        SizeT GetTransformFeedbackVaryingCount() const { return Artifacts().xfbVaryings.size(); }
        const XfbVarying* GetTransformFeedbackVarying(SizeT index) const {
            return index < Artifacts().xfbVaryings.size() ? &Artifacts().xfbVaryings[index] : nullptr;
        }
        const Vector<XfbVarying>& GetTransformFeedbackVaryings() const { return Artifacts().xfbVaryings; }
        // The GL_TRANSFORM_FEEDBACK_VARYING resource list: every name the last successful
        // link was asked to capture, in request order, pseudo-varyings included.
        const Vector<String>& GetTransformFeedbackInterfaceNames() const { return Artifacts().xfbInterfaceNames; }
        // Stride of one captured vertex in the given capture buffer slot.
        Uint32 GetTransformFeedbackStride(Uint32 bufferIndex) const {
            return bufferIndex < Artifacts().xfbStrides.size() ? Artifacts().xfbStrides[bufferIndex] : 0;
        }
        SizeT GetTransformFeedbackBufferCount() const { return Artifacts().xfbStrides.size(); }
        Int GetTransformFeedbackVaryingMaxLength() const { return Artifacts().xfbVaryingNameMaxLength; }
        // True when the capture layout uses gl_SkipComponents / gl_NextBuffer
        // (ARB_transform_feedback3), which no ES driver can express: it can only pack every
        // captured varying into one record with no gaps. A backend that captures through
        // such a driver has to capture into scratch storage and scatter the records into the
        // application's buffers itself, using packedOffsetBytes as the source offset and
        // (bufferIndex, offsetBytes, stride) as the destination.
        Bool NeedsScatteredTransformFeedbackCapture() const { return Artifacts().xfbNeedsScatteredCapture; }
        // Bytes one gap-free captured record occupies.
        Uint32 GetTransformFeedbackPackedStride() const { return Artifacts().xfbPackedStride; }
        // True when the capture stage is a triangle-strip geometry shader with a
        // statically-known emit sequence: the Vulkan capture order then needs the GL
        // odd-triangle vertex swap after EndTransformFeedback.
        Bool HasGsTriangleStripCaptureFixup() const { return Artifacts().gsStripCaptureFixup; }
        // Triangles per strip, in emission order, for ONE geometry invocation.
        const Vector<Uint32>& GetGsStripTriangles() const { return Artifacts().gsStripTriangles; }
        // GL_GEOMETRY_INPUT_TYPE of the linked geometry stage (GL_POINTS, GL_LINES,
        // GL_LINES_ADJACENCY, GL_TRIANGLES or GL_TRIANGLES_ADJACENCY), or GL_NONE when the
        // program has no geometry stage. Draws must present a compatible primitive type.
        GLenum GetGeometryInputType() const { return Artifacts().gsInputPrimitive; }

        Uint GetExternalIndex() const { return m_externalIndex; }
        // Globally-unique, never-reused id for this program object's lifetime. Unlike the GL
        // name (external index), which is freed to a LIFO list and immediately handed back by
        // the next glCreateProgram, this distinguishes a deleted-and-recreated program from the
        // original, so an identity cache can't false-hit on name recycling.
        Uint64 GetLifetimeId() const { return m_lifetimeId; }

    private:
        // ---- The one and only join gate for link output (P1 invariant I5) ----
        // Blocks until a pending link has finished and its LinkArtifacts have been
        // published into m_artifacts. It exists so that the ~120 readers of link output are
        // routed through it by the compiler rather than by review: m_artifacts is private
        // and Artifacts() is the only spelling that reaches it.
        //
        // The fast path - no pending link - is one predictable branch and stays inline: it
        // runs on every Artifacts() read (~1200 call sites project-wide) and the project
        // never builds with LTO (MOBILEGL_ENABLE_LTO=OFF), so an out-of-line body would be a
        // real cross-TU call at every one of them. The blocking half is out of line.
        void EnsureLinkJoined() const {
            if (m_pendingLink) JoinPendingLink();
        }
        void JoinPendingLink() const;
        // ProgramLinkTask is incomplete here, so IsLinkComplete()'s non-joining peek at the
        // node's state goes through this out-of-line helper.
        Bool IsPendingLinkTerminal() const;

        // ---- the second join gate: phase-B (SPIR-V) output only ----
        // Phase A FIRST, always. Two reasons: the phase-B publish replays the uniform writes
        // that were buffered during its window, and those need the phase-A reflection to
        // validate against; and a caller that reaches a phase-B getter without having settled
        // phase A would otherwise leave the link half-published.
        //
        // Same inline/out-of-line split as the phase-A gate, for the same reason: the five
        // getters behind this one include the per-draw uniform upload path.
        void EnsureSpirvJoined() const {
            if (m_pendingLink) JoinPendingLink();
            if (m_pendingSpirv) JoinPendingSpirv();
        }
        void JoinPendingSpirv() const;
        Bool IsPendingSpirvTerminal() const;

        // One buffered non-opaque glUniform* write. `dataOffset` indexes m_pendingUniformBytes,
        // which is one append-only blob rather than a per-record allocation.
        struct PendingUniformWrite {
            Uint location = 0;
            Uint byteOffsetInUniform = 0;
            Uint byteSize = 0;
            Uint dataOffset = 0;
        };
        // Replays the buffer into the freshly published shadow, in write order, and drains it.
        // Each record re-does the bounds check and the bytes-equal dedupe the live write path
        // performs, so "an identical write does not move the content version" survives the
        // detour exactly - and a record that really does change bytes moves the version, which
        // is what makes a backend re-upload the UBO it cached during the window.
        void ReplayBufferedUniformWrites() const;
        // Past this, BufferUniformWrite declines and the write joins instead. Sized so an
        // ordinary pack load never reaches it (a pending window is one program's worth of
        // uniforms) while a pathological writer cannot grow the heap without bound.
        static constexpr SizeT kMaxBufferedUniformBytes = 4u << 20;

        LinkArtifacts& Artifacts() {
            EnsureLinkJoined();
            return m_artifacts;
        }
        const LinkArtifacts& Artifacts() const {
            EnsureLinkJoined();
            return m_artifacts;
        }
        SpirvArtifacts& Spirv() {
            EnsureSpirvJoined();
            return m_spirv;
        }
        const SpirvArtifacts& Spirv() const {
            EnsureSpirvJoined();
            return m_spirv;
        }

        // GL-thread-only companion to ResetLinkArtifacts (see its definition). Const because
        // the publish half of the join calls it; see the mutable counters below.
        void BumpLinkObservableVersions() const;
        void AddDefaultFragmentShaderIfMissing();

        static Uint64 AllocateLifetimeId();

        // ---- GL-thread-owned state: never joins ----
        // Most of this is never produced by a link at all. The three version counters
        // (m_backendStateVersion / m_uboContentVersion / m_linkVersion) ARE
        // link-observable, but they are bumped exclusively on the GL thread
        // (BumpLinkObservableVersions in Link()'s prologue and glProgramBinary's
        // failure path) - the link BODY, which stage 4 moves to a worker, never
        // writes them.
        const Uint m_externalIndex = 0;
        const Uint64 m_lifetimeId = 0;
        // The attach lists are mutated only in Link()'s GL-thread prologue, which is why
        // glGetAttachedShaders / GL_ATTACHED_SHADERS / the orphan-shader sweep need no join.
        Vector<SharedPtr<ShaderObject>> m_shaders;
        Vector<SharedPtr<ShaderObject>> m_detachedShaders; // Store detached shaders and remove on next link

        // Link INPUTS (all "take effect at the next link" per GL): glBindAttribLocation,
        // glBindFragDataLocation(Indexed), glTransformFeedbackVaryings, and the draw-buffer
        // count stamped in by the entry point. A pending link snapshots these at enqueue.
        UnorderedMap<String, Uint> m_explicitAttribLocations;
        UnorderedMap<String, Uint> m_explicitFragDataLocation;
        // Dual-source blend color index per output name (glBindFragDataLocationIndexed); snapshotted
        // into the linked map at link time, like the location maps above.
        UnorderedMap<String, Uint> m_explicitFragDataIndex;
        Int m_maxFragmentOutputColorNumber = 8;
        Vector<String> m_requestedXfbVaryings;
        GLenum m_requestedXfbBufferMode = GL_INTERLEAVED_ATTRIBS;

        Bool m_deleteStatus = false;
        Bool m_binaryRetrievableHint = false;
        Bool m_separable = false;
        Bool m_validateStatus = true;
        // Mutable, like m_artifacts and for the same reason: publishing a pending link is a
        // READ-side operation (the first gated getter is what pulls the result in), and the
        // publish has to bump these. Still GL-thread-only - a worker never touches them.
        mutable Uint32 m_backendStateVersion = 0;

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
        mutable Uint32 m_uboContentVersion = 0;
        mutable Uint32 m_linkVersion = 0;

        // ---- Link OUTPUT ----
        // Written by the link and by the post-link setters GL allows (glUniform1i's sampler
        // unit, glUniformBlockBinding). Reachable only through Artifacts(); see LinkArtifacts.
        //
        // Mutable because publishing is a READ-side operation: a const getter has to be able
        // to settle an outstanding link before answering it.
        mutable LinkArtifacts m_artifacts;
        // Phase-B output. Same mutability argument as m_artifacts, reached only through
        // Spirv().
        mutable SpirvArtifacts m_spirv;

        // The link job, from enqueue until the first observable read pulls its result. Null
        // means m_artifacts is already the answer - which is the state every reader outside
        // the pending window sees, and the whole reason the gate above is one branch.
        mutable SharedPtr<ProgramLinkTask> m_pendingLink;
        // The SPIR-V job, chained behind m_pendingLink. Null means m_spirv is already the
        // answer. A program can be in the window where m_pendingLink is already null (phase A
        // published, the query surface is live) while this is still set.
        mutable SharedPtr<ProgramSpirvTask> m_pendingSpirv;
        // glUniform* writes taken while m_pendingSpirv was set, in call order, plus their
        // bytes. Drained by the phase-B publish and cleared by every cancel site (a relink's
        // uniforms are not the previous link's uniforms).
        mutable Vector<PendingUniformWrite> m_pendingUniformWrites;
        mutable Vector<Uint8> m_pendingUniformBytes;
    };
} // namespace MobileGL::MG_State::GLState
