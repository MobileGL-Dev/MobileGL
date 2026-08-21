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
        // GL_MAX_UNIFORM_LOCATIONS: locations 0 .. MAX_UNIFORM_LOCATIONS-1 are the whole legal
        // range (GL 4.6 core 7.6.1 / ARB_explicit_uniform_location). Shared with GL_Getter rather
        // than spelled twice, because the link and the query must agree exactly - the CTS declares
        // a uniform at the advertised value minus one and expects it to link
        // (KHR-GL43.explicit_uniform_location.uniform-loc-max).
        //
        // Tied to glslang's own ceiling and NOT raisable past it: ParseHelper rejects
        // `layout(location = N)` for N >= TQualifier::layoutLocationEnd at COMPILE time, so
        // layoutLocationEnd - 1 is the largest location any shader in this stack can declare -
        // which makes exactly layoutLocationEnd locations, 0 .. layoutLocationEnd - 1, the pool.
        // Advertising more would promise a location no shader could name. Comfortably above the
        // 1024 GL 4.3 requires.
        static constexpr Int MAX_UNIFORM_LOCATIONS = static_cast<Int>(glslang::TQualifier::layoutLocationEnd);

        // Everything the query surface ever asked a glslang::TType, flattened. Twenty
        // predicates, no recursion: nothing post-link ever walks a struct, a type name or the
        // AST, so a POD covers the whole surface exactly.
        struct TypeFacts {
            Bool isArray = false;
            // A runtime-sized array (a storage block's unsized trailing member) is an array
            // that is NOT sized; GL_ARRAY_SIZE reports 0 for it.
            Bool isSizedArray = false;
            Bool isMatrix = false;
            Bool isVector = false;
            Bool isOpaque = false;
            Bool isTexture = false;
            Bool isImage = false;
            Bool isDouble = false;   // getBasicType() == EbtDouble
            Bool isVoid = false;     // getBasicType() == EbtVoid (hidden block members)
            Bool isBuffer = false;   // getQualifier().storage == EvqBuffer
            Bool isPatch = false;    // getQualifier().patch
            Bool hasIndex = false;   // getQualifier().hasIndex()
            Bool hasFormat = false;  // getQualifier().hasFormat()
            Int vectorSize = 0;
            Int matrixCols = 0;
            Int matrixRows = 0;
            Int layoutIndex = 0;     // getQualifier().layoutIndex
            Uint layoutFormat = 0;   // getQualifier().getFormat()
            // glslang::TLayoutMatrix, widened. For a uniform this is already RESOLVED against
            // the owning block's qualifier, so the getUniformBlock() fallback the old
            // accessors carried is gone.
            Int layoutMatrix = 0;
            // glslang::TBasicType, widened - ApplyUniformInitialValues and the typed
            // glGetUniform* paths compare against a handful of enumerators.
            Int basicType = 0;
        };

        // One glslang::TObjectReflection, flattened. Used for uniforms, blocks, pipe inputs
        // and pipe outputs alike, because glslang reflects all four as TObjectReflection.
        struct ResourceReflection {
            String name;
            GLenum glDefineType = 0;
            Int offset = -1;
            // TObjectReflection::size, RAW. For a uniform prefer `arraySize` below, which is
            // the resolved GL_UNIFORM_SIZE answer.
            Int size = 0;
            // TObjectReflection::index - for a uniform, the TPROGRAM block index owning it
            // (-1 for a default-block one; translate with GlBlockIndexFromTProgram).
            Int index = -1;
            Int counterIndex = -1;
            Int arrayStride = 0;
            Int topLevelArraySize = 0;
            Int topLevelArrayStride = 0;
            Int binding = -1;
            Int location = -1; // layoutLocation()
            // EShLanguageMask of the stages that reference it; 0 means "declared but read by
            // nobody", which is what the dead-default-block-uniform filter tests.
            Uint32 stages = 0;
            // GL_UNIFORM_SIZE / GL_ARRAY_SIZE, already resolved through the
            // isSizedArray()/getOuterArraySize()/size fallback.
            GLint arraySize = 1;
            TypeFacts type;
        };

        using UniformReflection = ResourceReflection;
        using BlockReflection = ResourceReflection;
        using PipeInputReflection = ResourceReflection;
        using PipeOutputReflection = ResourceReflection;

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

        // One shader exactly as this program's last Link() consumed it: the object, the
        // source snapshot, and the compile node taken at that link's enqueue. GL 4.6 7.3/7.4
        // makes this triple - not the live attach list, not the shader's current compile -
        // what a program pipeline stage executes ("as last linked"): glAttachShader and
        // glCompileShader take effect only at the program's next link, yet neither moves
        // m_linkVersion, so anything keyed on the link generation must consume this
        // snapshot rather than re-read the live state.
        struct LinkedShaderRef {
            SharedPtr<ShaderObject> shader;
            SharedPtr<const String> source;
            SharedPtr<ShaderCompileTask> node;
        };
        // The last link's full input set; empty when this program has never linked (or its
        // last link had no shaders attached). GL-thread-owned, rebuilt in Link()'s prologue.
        const Vector<LinkedShaderRef>& GetLinkedShaderSnapshot() const { return m_linkedShaderSnapshot; }
        // "Does this program's EXECUTABLE have this stage" - the only form of the question a
        // draw may ask. GetShaderIndexByStage answers it of the live attach list, which by the
        // rule above is a different set: glAttachShader adds to that list immediately while
        // leaving the executable (and LINK_STATUS) alone, and glDetachShader defers the removal
        // to the next Link(), so between an attach and the relink the two disagree in both
        // directions. A draw-time stage test that reads the live list therefore starts rejecting
        // draws GL requires to execute, against an executable that does not carry the stage at
        // all - and stays wrong until the application happens to relink.
        Bool HasLinkedShaderStage(ShaderStage stage) const {
            return std::any_of(m_linkedShaderSnapshot.begin(), m_linkedShaderSnapshot.end(),
                               [stage](const LinkedShaderRef& ref) {
                                   return ref.shader && ref.shader->GetShaderStage() == stage;
                               });
        }
        // The stage of each module of GetGeneratedSpirv(), at the SAME index and with the same
        // size: phase B emits exactly one module per entry of the snapshot above, in that order
        // (Link() fills ProgramLinkTask::in.shaders from the snapshot loop, phase A copies the
        // stages straight across into SpirvHandoff::shaderTypes, and GetSpirvBinaryFromProgram
        // walks that list). This - never GetAttachedShaders() - is what a consumer of the
        // generated SPIR-V must size its loop by and index alongside.
        //
        // The two lists are NOT interchangeable and cannot be made so: the attach list is live
        // and the SPIR-V is a link artifact, so a glAttachShader after a link grows one and not
        // the other, with no link in between at which they could be reconciled. A loop that runs
        // over the attach list and indexes the SPIR-V therefore reads off the end of it - which
        // is a plain out-of-bounds Vector read, not a wrong answer.
        //
        // Deliberately a Vector<ShaderStage> and not the shader objects: every consumer wants
        // only the stage, and a distinct type is what makes handing it the attach list by
        // mistake a compile error rather than a segfault. Built on demand because these callers
        // are program-BUILD paths (a backend rebuild, a pipeline cache miss), each of which then
        // spends milliseconds compiling the very modules this indexes.
        Vector<ShaderStage> GetLinkedShaderStages() const {
            Vector<ShaderStage> stages;
            stages.reserve(m_linkedShaderSnapshot.size());
            for (const LinkedShaderRef& ref : m_linkedShaderSnapshot) {
                stages.push_back(ref.shader ? ref.shader->GetShaderStage() : ShaderStage::Unknown);
            }
            return stages;
        }
        // Pipeline-composite attach: AttachShader plus a pin that makes THIS program's
        // Link() consume ref's (source, node) instead of the shader's current ones, so a
        // post-link recompile of the stage program's shader cannot leak into the composite.
        bool AttachShaderWithPinnedLinkInput(const LinkedShaderRef& ref);
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
            if (!UniformAt(index).type.isArray) return -1;
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
        // Block index -> glslang TProgram block index (the inverse of
        // GlBlockIndexFromTProgram). The interface-query layer needs it to reach block
        // properties glslang exposes but no typed getter here does.
        Int TProgramBlockIndex(Uint blockIndex) const {
            return blockIndex < Artifacts().glBlockIndexToTProgram.size()
                ? Artifacts().glBlockIndexToTProgram[blockIndex]
                : -1;
        }
        Int GlBlockIndexFromTProgram(Int tBlockIndex) const {
            if (tBlockIndex < 0 || tBlockIndex >= static_cast<Int>(Artifacts().tProgramBlockIndexToGl.size())) return -1;
            return Artifacts().tProgramBlockIndexToGl[tBlockIndex];
        }

        // ---- GL_UNIFORM_BLOCK index <-> block index translation ----
        // The block index space above carries the storage blocks and the synthesized atomic
        // counter blocks as well; GL_ACTIVE_UNIFORM_BLOCKS counts only actual uniform blocks
        // (GL 4.6 core 7.6). Every glGetActiveUniformBlock* / glGetUniformBlockIndex /
        // glUniformBlockBinding entry point speaks THIS space and translates into the block
        // space before touching any of the block-keyed tables; the backends keep speaking the
        // block space directly. See LinkArtifacts::glUniformBlockIndexToBlock.
        Int GetGlUniformBlockCount() const {
            return static_cast<Int>(Artifacts().glUniformBlockIndexToBlock.size());
        }
        Bool IsActiveGlUniformBlock(Uint glUniformBlockIndex) const {
            return glUniformBlockIndex < Artifacts().glUniformBlockIndexToBlock.size();
        }
        Int BlockIndexFromGlUniformBlock(Uint glUniformBlockIndex) const {
            return glUniformBlockIndex < Artifacts().glUniformBlockIndexToBlock.size()
                ? Artifacts().glUniformBlockIndexToBlock[glUniformBlockIndex]
                : -1;
        }
        Int GlUniformBlockIndexFromBlock(Int blockIndex) const {
            if (blockIndex < 0 || blockIndex >= static_cast<Int>(Artifacts().blockIndexToGlUniformBlock.size())) {
                return -1;
            }
            return Artifacts().blockIndexToGlUniformBlock[blockIndex];
        }
        // glGetUniformBlockIndex: GL_INVALID_INDEX for a name that is not an active UNIFORM
        // block, which includes every storage block and every atomic counter block even though
        // GetUniformBlockIndex() below resolves them (it answers in the block space, which the
        // backends need to keep reaching them by name).
        Uint GetGlUniformBlockIndex(const char* name) const {
            const Uint blockIndex = GetUniformBlockIndex(name);
            if (blockIndex == 0xFFFFFFFFu) return 0xFFFFFFFFu;
            const Int glIndex = GlUniformBlockIndexFromBlock(static_cast<Int>(blockIndex));
            return glIndex < 0 ? 0xFFFFFFFFu : static_cast<Uint>(glIndex);
        }

        Int GetActiveUniformIndex(const String& name) const {
            // uniformIndexByName is keyed by the REFLECTED name, so a lookup that hits is
            // already the exact-match the old code re-verified with a string compare after
            // glslang's getUniformIndex(); a lookup that misses needs no bounds check.
            const auto& byName = Artifacts().uniformIndexByName;
            if (const auto direct = byName.find(name); direct != byName.end()) {
                return GlUniformIndexFromTProgram(direct->second);
            }

            // Reflection stores an array uniform under "arr[0]"; accept the bare "arr"
            // spelling too. The reverse ("arr[0]" against a bare "arr" entry) is kept for
            // robustness against non-suffixed reflection entries.
            if (!name.empty() && name.back() != ']') {
                const auto suffixed = byName.find(name + "[0]");
                return suffixed != byName.end() ? GlUniformIndexFromTProgram(suffixed->second) : -1;
            }

            if (name.length() <= 3 || name.compare(name.length() - 3, 3, "[0]") != 0) return -1;
            const auto base = byName.find(name.substr(0, name.length() - 3));
            return base != byName.end() ? GlUniformIndexFromTProgram(base->second) : -1;
        }

        Bool IsValidUniformLocation(Int location) const { return IsValidUniformLocation(Artifacts(), location); }

        GLenum GetUniformType(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).glDefineType;
        }

        GLenum GetActiveUniformType(Uint index) const {
            // The lowered counter is a plain uint inside a synthesized block; what the GL
            // client declared - and what glGetActiveUniform must report - is an atomic_uint.
            if (IsActiveUniformAtomicCounter(index)) return GL_UNSIGNED_INT_ATOMIC_COUNTER;
            return UniformAt(TProgramUniformIndex(index)).glDefineType;
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

        // The BLOCK index of the block owning this active uniform, or -1 when it owns none as
        // far as GL is concerned. Internal: pair it with another block-space index, never with
        // a GL_UNIFORM_BLOCK one (GetActiveUniformBlockIndex below is that one).
        Int GetActiveUniformOwnerBlockIndex(Uint index) const {
            // An atomic counter is a DEFAULT-BLOCK uniform to GL, whatever block the
            // transpiler lowered it onto (GL 4.6 core 7.6, table 7.6): -1.
            if (IsActiveUniformAtomicCounter(index)) return -1;
            // Members of the synthesized global UBO are default-block uniforms to GL: -1.
            return GlBlockIndexFromTProgram(UniformAt(TProgramUniformIndex(index)).index);
        }

        // GL_UNIFORM_BLOCK_INDEX: an index into the GL_ACTIVE_UNIFORM_BLOCKS list, or -1. A
        // buffer variable owns a storage block, which is not in that list, so it answers -1 too
        // (and after the enumeration filter it is not an active uniform in the first place).
        Int GetActiveUniformBlockIndex(Uint index) const {
            return GlUniformBlockIndexFromBlock(GetActiveUniformOwnerBlockIndex(index));
        }

        // The transpiler lowers every atomic_uint onto a synthesized gl_AtomicCounterBlock_N
        // block, but GL keeps seeing an atomic counter as a default-block uniform of type
        // GL_UNSIGNED_INT_ATOMIC_COUNTER that points at an atomic-counter BUFFER. These two
        // answer for that GL-level declaration; without them the query surface reports the
        // lowering instead (GL_UNSIGNED_INT, block index 0) and
        // KHR-GL43.shader_atomic_counters.basic-program-query fails on both.
        //
        // The returned value is an index into the GL_ACTIVE_ATOMIC_COUNTER_BUFFERS list, i.e.
        // the RANK of the owning counter block among the counter blocks in glslang's block
        // order - exactly how ProgramInterface numbers the GL_ATOMIC_COUNTER_BUFFER
        // resources glGetActiveAtomicCounterBufferiv answers from. -1 when this uniform is
        // not an atomic counter.
        // Answered from the OWNED reflection snapshot, never from Artifacts().program. This
        // arrived reading the live TProgram, which is null for every program served from the
        // translation cache's L1 - and unlike the other query-surface accessors that made the
        // same mistake, this one DEREFERENCES it, so the second program built from a given set
        // of sources would have taken the process down rather than answered wrongly. The
        // snapshot carries the same three facts in the same TPROGRAM index space:
        // getUniform(i).index -> UniformAt(i).index, getNumUniformBlocks() ->
        // blockReflection.size(), getUniformBlock(i).name -> BlockAt(i).name.
        Int GetActiveUniformAtomicCounterBufferIndex(Uint index) const {
            const Int tIndex = TProgramUniformIndex(index);
            if (tIndex < 0) return -1;
            const Int owner = UniformAt(tIndex).index;
            if (owner < 0) return -1;
            const Int blockCount = static_cast<Int>(Artifacts().blockReflection.size());
            if (owner >= blockCount) return -1;
            const SizeT prefixLength = StringView(MG_Util::ShaderTranspiler::ATOMIC_COUNTER_BLOCK_PREFIX).size();
            Int counterBufferIndex = 0;
            for (Int i = 0; i < blockCount; ++i) {
                const auto& blockName = BlockAt(i).name;
                if (blockName.compare(0, prefixLength, MG_Util::ShaderTranspiler::ATOMIC_COUNTER_BLOCK_PREFIX) != 0) {
                    continue;
                }
                if (i == owner) return counterBufferIndex;
                ++counterBufferIndex;
            }
            return -1;
        }

        Bool IsActiveUniformAtomicCounter(Uint index) const {
            return GetActiveUniformAtomicCounterBufferIndex(index) >= 0;
        }

        // GL_UNIFORM_OFFSET: byte offset within the owning named block; -1 for a default-block
        // uniform. The relaxed parse gives global-UBO members real byte offsets, but GL must keep
        // seeing them as default-block uniforms, so gate on the GL-visible block index.
        GLint GetActiveUniformOffset(Uint index) const {
            const auto& uniform = UniformAt(TProgramUniformIndex(index));
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
            const auto& uniform = UniformAt(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return -1;
            if (!uniform.type.isArray) return 0;
            // An atomic counter reaches the std140 branch below only because the transpiler
            // lowered it onto a synthesized block; the buffer it actually addresses is an
            // ATOMIC COUNTER buffer, whose elements are tightly packed uints (GL 4.6 core 7.6:
            // "each counter is a single 4-byte value"). Its array stride is therefore 4, not the
            // vec4 round-up std140 would apply
            // (KHR-GL43.shader_atomic_counters.basic-program-query wants 4 for ac_counter67[0]).
            if (IsActiveUniformAtomicCounter(index)) return 4;
            if (uniform.type.isMatrix) {
                const bool rowMajor = GetActiveUniformIsRowMajor(index) != 0;
                const int vectors = rowMajor ? uniform.type.matrixRows : uniform.type.matrixCols;
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
            const auto& uniform = UniformAt(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return 0;
            if (!uniform.type.isMatrix) return 0;
            // layoutMatrix is already resolved against the owning block's qualifier at
            // snapshot time, so the getUniformBlock() fallback this used to carry is gone.
            return (uniform.type.layoutMatrix == static_cast<Int>(glslang::ElmRowMajor)) ? 1 : 0;
        }

        // GL_UNIFORM_MATRIX_STRIDE: byte stride between columns (col-major) / rows (row-major) of a
        // matrix in a named block; 0 for a non-matrix block member; -1 for a default-block uniform.
        // glslang exposes no matrix stride, so it is derived from the std140 rule -- each column/row
        // vector's base alignment rounded up to a vec4 (16 B). MobileGL's SPIR-V path lays every UBO
        // out as std140 (packed/shared are coerced), so this matches the offsets glslang reports. For
        // every GL 3.3 float matrix this evaluates to 16, independent of majorness.
        GLint GetActiveUniformMatrixStride(Uint index) const {
            const auto& uniform = UniformAt(TProgramUniformIndex(index));
            if (GlBlockIndexFromTProgram(uniform.index) < 0) return -1;
            if (!uniform.type.isMatrix) return 0;
            const bool rowMajor = (uniform.type.layoutMatrix == static_cast<Int>(glslang::ElmRowMajor));
            const int strideVectorComponents = rowMajor ? uniform.type.matrixCols : uniform.type.matrixRows;
            constexpr int scalarSize = 4; // GL 3.3 core uniform matrices are float
            const int vectorAlignment = (strideVectorComponents <= 1)   ? scalarSize
                                        : (strideVectorComponents == 2) ? 2 * scalarSize
                                                                        : 4 * scalarSize;
            return (vectorAlignment + 15) & ~15; // std140 round-up to a vec4
        }

        // The flattened type of the uniform at `location`. This is what replaced
        // GetUniformTType(): the same information, owned by the program instead of by a
        // glslang pool, so it stays valid for a link served from the L1 translation memo.
        const TypeFacts& GetUniformTypeFacts(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).type;
        }

        // Replaces GetUniformTType(), which used to hand a raw glslang::TType* - into a
        // pool the program no longer necessarily owns - out to the DirectGLES image-format
        // bake. These are the only three things any caller ever read off it.
        Bool UniformHasDeclaredImageFormat(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).type.hasFormat;
        }
        Uint GetUniformDeclaredImageFormat(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).type.layoutFormat;
        }
        // Matrix column count, 0 for a non-matrix. The global-UBO fallback allocator sizes a
        // matrix slot from it.
        Int GetUniformMatrixColumns(Uint location) const {
            const auto& uniform = UniformAt(Artifacts().uniformIndexInTProgram[location]);
            return uniform.type.isMatrix ? uniform.type.matrixCols : 0;
        }

        Bool IsUniformOpaqueAtLocation(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).type.isOpaque;
        }

        const String& GetUniformName(Uint location) const {
            return UniformAt(Artifacts().uniformIndexInTProgram[location]).name;
        }

        const String& GetActiveUniformName(Uint index) const {
            return UniformAt(TProgramUniformIndex(index)).name;
        }
        // Sentinel for a uniform location without global-UBO backing storage (should not
        // survive linking: GenerateBinary falls back to tail-allocated scratch storage).
        static constexpr Uint kInvalidUniformOffset = ~0u;
        // PHASE B (joins the SPIR-V job; see EnsureSpirvJoined).
        //
        // BOUNDS-CHECKED, and that is not defensive padding - it is the load-bearing half of
        // the "linked but not drawable" contract. A phase B that settles CANCELLED rather than
        // Complete (its body threw, the pool failed to enqueue it, or teardown cancelled it
        // while phase A had already published) publishes nothing, so the shadow is a
        // default-constructed SpirvArtifacts with an EMPTY uniformOffsets - while LINK_STATUS
        // stays GL_TRUE, because GL gives no way to retract one, and IsValidUniformLocation()
        // keeps answering true out of phase-A reflection. Every glUniform*/glGetUniform* call
        // site reaches this getter BEFORE its own kInvalidUniformOffset / null-scratch guard,
        // so an unchecked operator[] here would be a null dereference on the query surface
        // this design promises stays answerable. Reporting kInvalidUniformOffset instead hands
        // each of those sites exactly the value their existing guard already handles - the
        // same value the routing pass itself uses for a uniform the optimizer deleted.
        Uint GetUniformOffset(Uint location) const {
            const SpirvArtifacts& spirv = Spirv();
            return location < spirv.uniformOffsets.size() ? spirv.uniformOffsets[location]
                                                          : kInvalidUniformOffset;
        }
        Uint GetUniformSizesInBytes(Uint location) const { return MG_Util::GetGLTypeSize(GetUniformType(location)); }
        // Bytes a uniform actually occupies in the global UBO, which is not its GL type size,
        // for two reasons. std140 pads each column of a matrix out to a vec4, so a mat3 spans
        // 48 bytes even though only 36 of them carry components. And every 64-bit float in a
        // shader is narrowed to 32 bits before the module reaches a backend
        // (ShaderTranspiler::DemoteFloat64Pass) - the global UBO is laid out by reflecting that
        // demoted module - so a `double` uniform occupies exactly what its float-typed twin
        // would, half its GL type size, and a `dmat4` is padded like any other matrix. Anything
        // reading or writing a whole uniform's storage - a bounds check, a copy between two
        // programs' shadows - wants this rather than GetUniformSizesInBytes.
        static SizeT UniformStorageSpanInBytes(const TypeFacts& type, SizeT tightSize) {
            if (type.isMatrix) {
                return static_cast<SizeT>(type.matrixCols) * 4 * sizeof(Float);
            }
            if (type.isDouble) {
                return tightSize / 2;
            }
            return tightSize;
        }
        SizeT GetUniformStorageSpanInBytes(Uint location) const {
            return UniformStorageSpanInBytes(GetUniformTypeFacts(location), GetUniformSizesInBytes(location));
        }

        // ---- "written since link": the per-location dirty set the pipeline composite mirrors from ----
        //
        // A pipeline's stage programs each own their uniform storage, but the composite the draw
        // goes through has ONE slot per name. Mirroring every active uniform of every stage
        // therefore lets the last stage that merely DECLARES a name overwrite the value an
        // earlier stage was actually written with - the shared-header idiom (the same
        // `uniform mat4 u_mvp` in the VS and the FS) rendered nothing because of it. Recording
        // which locations an application has written is what lets the mirror carry only those.
        //
        // WHO PAYS: only a program that could ever be a pipeline stage, decided by the latch
        // below. glUseProgram's uniform path - thousands of calls per frame in Minecraft - pays
        // one predictable bool branch and nothing else.
        //
        // GRANULARITY is per LOCATION, not per name: glUniform*v writes array elements at
        // element locations, and a program that wrote `arr[3]` and nothing else must mirror
        // exactly that element. The compact index list beside it is what keeps the mirror
        // O(uniforms actually written) instead of O(active uniforms) - it is the set of GL
        // active-uniform indices owning at least one written location, so the mirror does its
        // two name lookups once per written uniform rather than once per uniform in the program.
        //
        // NOT counted as a write: the declared initializers ProgramLinkTask seeds at link
        // (ApplyUniformInitialValues). They are a property of the SHADERS, and the composite
        // links the very same shader objects, so it seeds itself with the identical values -
        // there is nothing to carry. Counting them would also re-introduce the bug this set
        // exists to fix, by letting a stage that only declares `uniform float f = 0.0;` clobber
        // the value the application wrote for `f` in another stage.
        Bool TracksUniformWrites() const { return m_tracksUniformWrites; }

        // Generation of the write SET itself, as distinct from the values in it. The refresh
        // gate (ProgramPipelineObject::ComputeUniformMirrorVersions) is otherwise built out of
        // counters that only move when BYTES move - and a write can enlarge the set without
        // moving a byte, because both write funnels drop a value-identical write before
        // bumping anything. glProgramUniform1f(fs, f, 0.0f) on an `f` that already reads 0.0
        // is exactly that: it makes the FRAGMENT stage the last written-to stage for `f`, so
        // the composite must be re-mirrored to hand it the slot, and nothing else in the gate
        // would have noticed.
        Uint32 GetUniformWriteSetVersion() const { return m_uniformWriteSetVersion; }

        // Records that `location` has been written since the last link. Cheap and idempotent;
        // a no-op on a program that can never be a pipeline stage.
        void MarkUniformWrittenAtLocation(Uint location) {
            if (!m_tracksUniformWrites) return;
            LinkArtifacts& artifacts = Artifacts();
            if (!IsValidUniformLocation(artifacts, static_cast<Int>(location))) return;

            // Sized to cover this location AND the whole location space, so a program whose
            // highest location is written first does not reallocate on every later write, and
            // so the subscript below needs no second guard: the vector provably contains it.
            const SizeT locationWord = location / 64u;
            if (locationWord >= artifacts.writtenUniformLocationBits.size()) {
                artifacts.writtenUniformLocationBits.resize(
                    std::max<SizeT>(locationWord + 1u, static_cast<SizeT>(artifacts.maxUniformLocation) / 64u + 1u),
                    0u);
            }
            const Uint64 locationBit = Uint64{1} << (location % 64u);
            if ((artifacts.writtenUniformLocationBits[locationWord] & locationBit) == 0) {
                artifacts.writtenUniformLocationBits[locationWord] |= locationBit;
                // Only on the 0 -> 1 transition: a re-write of a location already in the set
                // changes nothing the mirror would do differently, and moving the version for
                // it would re-walk the set on every repeated glUniform* call.
                ++m_uniformWriteSetVersion;
            }

            // Add the owning GL active-uniform index to the compact list, once.
            const Int tIndex = artifacts.uniformIndexInTProgram[location];
            if (tIndex < 0 || static_cast<SizeT>(tIndex) >= artifacts.tProgramUniformIndexToGl.size()) return;
            const Int glIndex = artifacts.tProgramUniformIndexToGl[tIndex];
            // -1 is a uniform the relaxed parse swept out of the GL-visible index space; the
            // mirror enumerates GL indices, so there is nothing it could look such a one up by.
            if (glIndex < 0) return;
            const SizeT indexWord = static_cast<SizeT>(glIndex) / 64u;
            if (indexWord >= artifacts.writtenUniformIndexBits.size()) {
                artifacts.writtenUniformIndexBits.resize(
                    std::max<SizeT>(indexWord + 1u, static_cast<SizeT>(artifacts.activeUniformCount) / 64u + 1u), 0u);
            }
            const Uint64 indexBit = Uint64{1} << (static_cast<SizeT>(glIndex) % 64u);
            if ((artifacts.writtenUniformIndexBits[indexWord] & indexBit) != 0) return;
            artifacts.writtenUniformIndexBits[indexWord] |= indexBit;
            artifacts.writtenUniformIndices.push_back(static_cast<Uint>(glIndex));
        }

        Bool IsUniformWrittenAtLocation(Uint location) const {
            const auto& bits = Artifacts().writtenUniformLocationBits;
            const SizeT locationWord = location / 64u;
            return locationWord < bits.size() &&
                   (bits[locationWord] & (Uint64{1} << (location % 64u))) != 0;
        }

        // GL active-uniform indices owning at least one written location. Empty for every
        // program that has not been written to since its last link - and for every program
        // that never asked to be separable, which is what makes the mirror free for them.
        const Vector<Uint>& GetWrittenUniformIndices() const { return Artifacts().writtenUniformIndices; }

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
            if (Artifacts().pipeOutputReflection.empty()) {
                return 0;
            }

            Uint32 mask = 0;
            const Int outputCount = static_cast<Int>(Artifacts().pipeOutputReflection.size());
            for (Int index = 0; index < outputCount; ++index) {
                const Int location = Artifacts().pipeOutputReflection[index].location;
                if (location >= 0 && location < 32) {
                    mask |= (1u << location);
                }
            }
            return mask;
        }
        Int GetActiveFragmentOutputCount() const {
            return static_cast<Int>(Artifacts().pipeOutputReflection.size());
        }
        const String& GetActiveFragmentOutputName(Uint index) const {
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().pipeOutputReflection.size()),
                            "ProgramObject::GetActiveFragmentOutputName: index=%u out of range", index);
            return Artifacts().pipeOutputReflection[index].name;
        }
        Int GetFragmentOutputLocation(Uint index) const {
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().pipeOutputReflection.size()),
                            "ProgramObject::GetFragmentOutputLocation: index=%u out of range",
                            index);
            return Artifacts().pipeOutputReflection[index].location;
        }
        GLint GetActiveFragmentOutputArraySize(Uint index) const {
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().pipeOutputReflection.size()),
                            "ProgramObject::GetActiveFragmentOutputArraySize: index=%u out of range", index);
            return Artifacts().pipeOutputReflection[index].size;
        }
        GLenum GetFragmentOutputType(Uint index) const {
            MOBILEGL_ASSERT(index < static_cast<Uint>(Artifacts().pipeOutputReflection.size()),
                            "ProgramObject::GetFragmentOutputType: index=%u out of range",
                            index);
            return Artifacts().pipeOutputReflection[index].glDefineType;
        }
        GLenum GetAttribType(Uint index) const { return Artifacts().attribTypes[index]; }
        const String& GetAttribName(Uint index) const { return Artifacts().attribs[index]; }
        GLenum GetActiveAttribType(Uint index) const { return Artifacts().pipeInputReflection[index].glDefineType; }
        GLint GetActiveAttribArraySize(Uint index) const { return Artifacts().pipeInputReflection[index].size; }
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
            return NormalizeBuiltinPipeInputName(Artifacts().pipeInputReflection[index].name);
        }
        // PHASE B, all three (see EnsureSpirvJoined): the shadow buffer's layout is decided
        // by the OPTIMIZED SPIR-V, so it does not exist until the SPIR-V job has settled - and
        // never exists at all for a program whose SPIR-V job settled cancelled. These three
        // degrade to nullptr/nullptr/0 in that case, which is exactly the "no backing storage"
        // shape every caller already tests for (see GetUniformOffset's note).
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
            if (location >= Artifacts().uniformSamplerOrImageUnitIndex.size()) return;
            // BEFORE the equality bail-out, not after: "written" is about the application
            // having addressed the uniform, not about the bytes changing. glUniform1i(s, 0) on
            // a sampler that already reads 0 still has to beat another stage's untouched
            // declaration of the same name in the composite - which is only possible if the
            // write is recorded. (The mirror is the only reader, and it runs this same setter
            // on the composite, where the latch is off.)
            MarkUniformWrittenAtLocation(location);
            if (Artifacts().uniformSamplerOrImageUnitIndex[location] == unit) return;
            Artifacts().uniformSamplerOrImageUnitIndex[location] = unit;
            ++m_backendStateVersion;
            // IMAGE units get their own generation, and it is not redundant with the one
            // above. A sampler unit is re-issued to the driver per draw as a plain
            // glUniform1i, so a backend can honour a change without rebuilding anything; an
            // image unit cannot be, because ES forbids glUniform1i on image uniforms - Espryt
            // has to BAKE it into the ESSL it generates (RebindImageUniformsToFrontendUnits),
            // which means the change is only honoured by regenerating the program. That
            // regeneration is gated on link-shaped versions, so without a counter that moves
            // here the new unit would never reach the driver.
            if (GetUniformTypeFacts(location).isImage) {
                ++m_imageUnitVersion;
            }
        }

        // Generation of the image-uniform unit assignment; see SetUniformSamplerOrImageUnitIndex.
        // A backend that compiles the unit into its program source compares this to decide
        // whether what it built is still describing the right binding.
        Uint32 GetImageUnitVersion() const { return m_imageUnitVersion; }

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
        void SetSeparable(Bool separable) {
            m_separable = separable;
            // ---- arming the uniform-write tracking latch ----
            //
            // The predicate wanted is "this program can ever be a pipeline stage", and
            // GetSeparable() is NOT it in either direction. GL_PROGRAM_SEPARABLE takes effect
            // at the NEXT link, so it can read true on a program glUseProgramStages would
            // still reject; that direction is merely wasteful. The other direction is a
            // correctness hole: glProgramParameteri may clear the flag AFTER a separable link,
            // and glUseProgramStages tests the state the program was LINKED with, so such a
            // program is still a legal stage while GetSeparable() reads false. Tracking driven
            // by the live flag would stop recording writes on a program the composite is still
            // mirroring from, and those uniforms would silently stop reaching the draw.
            //
            // "Attached to a pipeline" is not usable either, and for a more basic reason:
            // glProgramUniform* legitimately runs before glUseProgramStages, so the marks have
            // to already exist by the time the program becomes a stage.
            //
            // So: a MONOTONE latch, armed the first time GL_PROGRAM_SEPARABLE is requested
            // true and never cleared. It over-approximates - a program that was separable once
            // keeps paying the bookkeeping - and over-approximating only ever costs a bitset,
            // never a wrong value. glCreateShaderProgramv arms it through this same setter.
            // A program that never asks (every monolithic glUseProgram program, which is the
            // hot uniform path) never arms it and pays one bool branch per glUniform*.
            if (separable) m_tracksUniformWrites = true;
        }
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
        // Artifacts().program is null until a link produces reflection, and glGetProgramiv is
        // perfectly legal on a program that never linked (GL 4.6 sec. 7.3: the queried state is
        // simply its initial value, zero). Dereferencing it there took the process down with a
        // SIGSEGV inside glslang::TProgram::getNumPipeInputs - KHR-GL30.api.coverage does exactly
        // this after a failed glGetAttribLocation, and reached it as soon as the CopyTexImage2D
        // throw ahead of it stopped killing the run first.
        Int GetActiveAttributesCount() const {
            return static_cast<Int>(Artifacts().pipeInputReflection.size());
        }
        // Size of the BLOCK index space - every block the relaxed parse produced except the
        // synthesized MGL_GLOBAL_UBO, which DoReflection filters out. NOT the answer to
        // glGetProgramiv(GL_ACTIVE_UNIFORM_BLOCKS): storage blocks and atomic counter blocks
        // live in here too, and GetGlUniformBlockCount() is the one that excludes them.
        Int GetActiveUniformBlocksCount() const { return static_cast<Int>(Artifacts().glBlockIndexToTProgram.size()); }
        GLuint GetComputeLocalSize(Uint dim) const {
            return dim < 3u ? Artifacts().computeLocalSize[dim] : 0u;
        }
        Int GetActiveAttributesMaxLength() const { return Artifacts().attribInNameMaxLength; }
        Int GetActiveUniformBlocksMaxNameLength() const { return Artifacts().uniformBlockNameMaxLength; }
        // Answers in the BLOCK space, so it resolves storage and atomic counter blocks too -
        // the backends reach those by name. glGetUniformBlockIndex must NOT: use
        // GetGlUniformBlockIndex() for the GL entry point.
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
        // Takes a BLOCK index. The GL entry points validate their argument against the
        // GL_UNIFORM_BLOCK space with IsActiveGlUniformBlock() first and translate; the bound
        // test here is only the range of the space this index actually lives in.
        Uint GetUBOSizeAt(Uint index) const {
            if (index >= Artifacts().glBlockIndexToTProgram.size()) return 0;
            // glslang reports the unpadded end offset of the last member, but a std140 block
            // (like a std140 struct) occupies a vec4-rounded size, and that is what the
            // backend compiles: ES drivers reject draws whose bound UBO range is smaller
            // than the block (a block ending in ivec3 reported 12 while the driver needs 16).
            return (static_cast<Uint>(BlockAt(Artifacts().glBlockIndexToTProgram[index]).size) + 15u) & ~15u;
        }

        const String& GetUniformBlockName(Uint index) const {
            const auto& ubo = BlockAt(Artifacts().glBlockIndexToTProgram[index]);
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
        // Takes a BLOCK index, and scans in the block space: GetUniformBlockMemberOwnerIndex
        // answers there, so pairing it with the GL_UNIFORM_BLOCK-space
        // GetActiveUniformBlockIndex would compare two different numberings.
        Int GetUniformBlockActiveUniformCount(Uint index) const {
            const Int ownerIndex = static_cast<Int>(GetUniformBlockMemberOwnerIndex(index));
            Int count = 0;
            for (Uint uniformIndex = 0; uniformIndex < Artifacts().activeUniformCount; ++uniformIndex) {
                if (GetActiveUniformOwnerBlockIndex(uniformIndex) == ownerIndex) ++count;
            }
            return count;
        }

        Bool IsUniformBlockReferencedByStage(Uint index, EShLanguage stage) const {
            const auto& ubo = BlockAt(Artifacts().glBlockIndexToTProgram[index]);
            const auto stageMask = static_cast<EShLanguageMask>(1 << stage);
            return (ubo.stages & stageMask) != 0;
        }

        // Bumped by both block-binding setters below. A program pipeline's flattened composite
        // is a different program object from the stage programs the application rebinds blocks
        // on, so it has to be told - and this is what tells it something is worth re-reading.
        // Separate from m_backendStateVersion because the storage-block setter deliberately
        // does not disturb that one (see SetShaderStorageBlockBinding).
        Uint32 GetBlockBindingVersion() const { return m_blockBindingVersion; }

        // Set by glUniformBlockBinding. The vector is seeded at link with each block's DECLARED
        // binding (layout(binding=N), else -1), so an untouched program already reports what its
        // shaders asked for.
        void SetUniformBlockBinding(Uint index, Uint binding) {
            if (index >= Artifacts().uniformBlockBinding.size() || Artifacts().uniformBlockBinding[index] == static_cast<Int>(binding)) {
                return;
            }
            Artifacts().uniformBlockBinding[index] = static_cast<Int>(binding);
            ++m_backendStateVersion;
            ++m_blockBindingVersion;
        }

        Uint GetUniformBlockBinding(Uint index) const { return Artifacts().uniformBlockBinding[index]; }

        // Set by glShaderStorageBlockBinding, keyed by the block's GL name rather than by any
        // index. A shader storage block has THREE index spaces - the frontend interface-query
        // enumeration, DirectVulkan's SPIR-V descriptor order and DirectGLES's real-driver
        // order - and the name is the only coordinate all three agree on. Absent from the map
        // means "never rebound", and the shader's declared binding still stands.
        void SetShaderStorageBlockBinding(const String& blockName, Uint binding) {
            // Equality bail-out like SetUniformBlockBinding's: the pipeline composite
            // mirror replays every override each draw, and without this every replay
            // would churn m_blockBindingVersion and rebuild whatever keys on it.
            const auto it = Artifacts().shaderStorageBlockBinding.find(blockName);
            if (it != Artifacts().shaderStorageBlockBinding.end() && it->second == static_cast<Int>(binding)) {
                return;
            }
            Artifacts().shaderStorageBlockBinding[blockName] = static_cast<Int>(binding);
            // Deliberately NOT m_backendStateVersion: Espryt's entry point never forces a
            // program build off this, and bumping that version would start doing so. The
            // dedicated counter carries the news to the pipeline composite instead.
            ++m_blockBindingVersion;
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
        // Copied from the link task that generated this program's SPIR-V. Backends use it for
        // their final transforms, which must honor the same diagnostic setting as phase B.
        Bool GetSpirvValidationEnabled() const { return Spirv().enableSpirvValidation; }

        // The linked glslang reflection itself, for the ONE consumer that needs resource
        // lists no typed getter above exposes: the GL program-interface query layer
        // (MG_Impl/GLImpl/Program/ProgramInterface.cpp), which has to enumerate buffer
        // blocks, buffer variables, atomic counters and per-stage reference masks. Null
        // until a link has succeeded. Read through the join gate like everything else.
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

            // GL 4.6 core 11.1.2.1 / 7.3.1.1: a member of an output interface block is
            // captured under "<block name>.<member>". `name` keeps that GL spelling (it is
            // what the interface queries and the ESSL backend's driver-side capture list
            // need, since SPIRV-Cross re-emits the block under its own type name), while
            // the three fields below carry what a SPIR-V backend needs instead: the
            // decoration target is the block's *instance* variable and the member index
            // inside it. blockMemberIndex < 0 means "not a block member".
            String blockInstanceName;
            String blockName;
            Int blockMemberIndex = -1;
            // Which element of an arrayed block member this capture names, -1 for "the
            // member as a whole". SPIR-V cannot decorate a single array element, so a
            // backend needs the element index to tell a full run from a partial one.
            Int blockMemberElement = -1;
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
        // ---- the owned mirror of glslang's reflection ----
        //
        // WHY THIS EXISTS. Every GL query about a linked program used to be answered by
        // asking the live glslang::TProgram - program->getUniform(i).getType()->isMatrix()
        // and friends. That made the TProgram part of the program's PERMANENT state, which
        // in turn made the whole front end (parse + link) unskippable: the L1 shader
        // translation memo could hand back the SPIR-V but the reflection still had to be
        // rebuilt from a freshly parsed AST.
        //
        // These three tables are a snapshot of everything the query surface ever reads off
        // the TProgram, in PLAIN OWNED VALUES - no TType*, no TString, nothing pointing into
        // a glslang pool. Taken once at the tail of DoReflection (SnapshotGlslangReflection),
        // they are copyable, immutable after the link, and safe to memoize and share between
        // ProgramObjects and threads. Once they are filled, `program` is dead weight to
        // everything except DoReflection itself.
        //
        // INDEXED BY TPROGRAM INDEX, deliberately: that is the space uniformIndexInTProgram,
        // glUniformIndexToTProgram and tProgramUniformIndexToGl already speak, so every
        // accessor that used to call program->getUniform(i) indexes uniformReflection[i]
        // instead, unchanged in every other respect.

        struct LinkArtifacts {
            // Live only between LinkProgram() and the end of DoReflection. Everything after
            // that reads the owned mirror below; a link served from the L1 memo never
            // constructs one at all, so this is null for such a program and MUST NOT be
            // dereferenced outside DoReflection.
            SharedPtr<glslang::TProgram> program;

            // The owned reflection snapshot. Indexed by TProgram index; see the structs above.
            Vector<UniformReflection> uniformReflection;
            Vector<BlockReflection> blockReflection;
            Vector<PipeInputReflection> pipeInputReflection;
            Vector<PipeOutputReflection> pipeOutputReflection;
            // Program-level scalars glslang answers off the linked intermediates.
            // Whether the program's LAST stage is the fragment stage. A color number - and so a
            // color index - exists only there; a separable tess/geometry/vertex program's
            // outputs are varyings and must report -1 (KHR-GL43.program_interface_query.
            // separate-programs-tess-control).
            Bool lastStageIsFragment = false;
            Array<GLuint, 3> computeLocalSize{};
            // Replaces program->getUniformIndex(name). Maps the reflected name to its
            // TProgram uniform index.
            UnorderedMap<String, Int> uniformIndexByName;

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
            // GL_UNIFORM_BLOCK index space: ACTUAL uniform blocks only, a strict subsequence of
            // glBlockIndexToTProgram above.
            //
            // That list is the BLOCK space - everything the relaxed parse produced except
            // MGL_GLOBAL_UBO - and it is what the backends walk and what every block-keyed table
            // here (uniformBlockBinding, uniformBlockIndexByName, blockReflection ordering) is
            // indexed by. It is NOT the GL uniform-block list: MobileGL does not pass
            // EShReflectionSeparateBuffers to buildReflection, so glslang routes BUFFER blocks
            // through indexToUniformBlock too, and the list therefore also carries every shader
            // storage block and every synthesized gl_AtomicCounterBlock_N. GL 4.6 core 7.6 gives
            // those their own enumerations (GL_SHADER_STORAGE_BLOCK and
            // GL_ACTIVE_ATOMIC_COUNTER_BUFFERS respectively), and GL_ACTIVE_UNIFORM_BLOCKS /
            // glGetActiveUniformBlock*/glGetUniformBlockIndex must not see either.
            //
            // Kept as a SECOND space rather than filtering the first in place: DirectGLES assigns
            // one ESSL uniform-buffer binding point per entry of the block list as it walks it
            // (Managers.cpp CacheResourceLocations and the matching per-draw loop in
            // DirectGLES.cpp), so compacting that list would renumber every backend binding
            // point, and tProgramBlockIndexToGl[i] < 0 is what DoReflection and
            // BuildGlobalUboRouting read as "member of the synthesized global UBO".
            Vector<Int> glUniformBlockIndexToBlock; // GL uniform-block index -> block index
            Vector<Int> blockIndexToGlUniformBlock; // block index -> GL uniform-block index (-1)
            // Per-link merged snapshot of the attached shaders' lexically extracted
            // layout(location = N) default-block uniform qualifiers (the relaxed parse drops
            // them from reflection; the DoReflection assigner restores them from here).
            UnorderedMap<String, Int> linkedExplicitUniformLocations;
            // Per-link snapshot of the default-block uniform INITIALIZERS the attached shaders
            // declared ("uniform int i = 1;"). Desktop GLSL says that value is what the uniform
            // reads until the application overwrites it, and relinking restores it - but the
            // relaxed parse turns those uniforms into members of MGL_GLOBAL_UBO, where SPIR-V
            // cannot carry an initializer, so the value only survives as this side-channel.
            // Applied into the uniform shadow at the phase-B publish (ApplyUniformInitialValues).
            Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;
            UnorderedMap<String, Uint> uniformLocations;
            // ---- "written since link" (see MarkUniformWrittenAtLocation) ----
            // In LinkArtifacts deliberately: a link is exactly the event that retracts every
            // write (GL resets uniforms to their initial values), so living here means the set
            // is cleared by the same three paths that clear the rest of a link's output -
            // Link()'s whole-struct reset, ResetLinkArtifacts, and the publish's move - and no
            // fourth reset site can be forgotten. Empty (and never allocated) for a program
            // that never asked to be separable.
            Vector<Uint64> writtenUniformLocationBits;
            Vector<Uint64> writtenUniformIndexBits;
            Vector<Uint> writtenUniformIndices;
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
            //
            // ALSO SEEDED AT LINK, by ProgramLinkTask::SeedDefaultStorageBlockBindings, with the
            // GL-mandated binding 0 for every storage block whose shader declared no
            // layout(binding = N). Those blocks have no other way to be told apart from a block
            // that declared one: glslang's IO mapper invents a binding and writes it into the
            // qualifier, so the reflection reports the invention. A seed is therefore "GL's
            // default binding for this block", and a later glShaderStorageBlockBinding simply
            // overwrites it - default and rebind travel one path.
            UnorderedMap<String, Int> shaderStorageBlockBinding;
            // Block type names of the storage blocks the program's shaders declared with NO
            // layout(binding = N), merged across stages by MergeShaderSideChannels. Input to the
            // seeding above; see ExtractStorageBlocksWithoutExplicitBinding for why it has to be
            // captured lexically.
            std::set<String> storageBlocksWithoutBinding;

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
            Bool enableSpirvValidation = false;
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

        // The owned reflection snapshot, for the program-interface query layer. Replaces
        // GetReflection(), which handed out the live glslang::TProgram - the last thing that
        // forced a linked program to keep its parse alive.
        const LinkArtifacts& GetLinkReflection() const {
            EnsureLinkJoined();
            return Artifacts();
        }

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
            return UniformAtIn(artifacts, tIndex).arraySize;
        }

        // Bounds-checked mirror lookup. Out of range yields a default-constructed entry
        // rather than UB, which is the same shape the phase-B getters use: a program whose
        // reflection is missing must stay answerable, not crash the query surface.
        static const UniformReflection& UniformAtIn(const LinkArtifacts& artifacts, Int tIndex) {
            static const UniformReflection kEmpty;
            if (tIndex < 0 || static_cast<SizeT>(tIndex) >= artifacts.uniformReflection.size()) return kEmpty;
            return artifacts.uniformReflection[tIndex];
        }
        const UniformReflection& UniformAt(Int tIndex) const { return UniformAtIn(Artifacts(), tIndex); }
        const BlockReflection& BlockAt(Int tBlockIndex) const {
            static const BlockReflection kEmpty;
            if (tBlockIndex < 0 || static_cast<SizeT>(tBlockIndex) >= Artifacts().blockReflection.size()) {
                return kEmpty;
            }
            return Artifacts().blockReflection[tBlockIndex];
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
        // Seeds the freshly published uniform shadow with the declared initializers. Runs at
        // the phase-B publish, BEFORE ReplayBufferedUniformWrites, so an application write
        // made during the A->B window still wins - which is the GL ordering.
        void ApplyUniformInitialValues() const;
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
        // See GetLinkedShaderSnapshot. Holding the SharedPtrs here is deliberate: the
        // "as last linked" set must survive detach-and-delete of its shaders (the
        // glCreateShaderProgramv shape) until the next link replaces it.
        Vector<LinkedShaderRef> m_linkedShaderSnapshot;
        // See AttachShaderWithPinnedLinkInput. Populated only on pipeline composites,
        // which never detach, so entries need no removal path. GL-thread-owned.
        UnorderedMap<const ShaderObject*, LinkedShaderRef> m_pinnedLinkInputs;

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
        // Monotone "this program may ever be a pipeline stage" latch; see SetSeparable for why
        // it is a latch and not just m_separable. Outside LinkArtifacts on purpose: a relink
        // clears the write SET, but a program that was separable is still separable after it.
        Bool m_tracksUniformWrites = false;
        // Generation counters that must NOT be reset by a link, for the same reason the memo
        // versions above are not: a reader compares them for INEQUALITY, so a reset could make
        // a stale cache compare equal to a fresh program. See their getters.
        Uint32 m_uniformWriteSetVersion = 0;
        Uint32 m_imageUnitVersion = 0;
        Bool m_validateStatus = true;
        // Mutable, like m_artifacts and for the same reason: publishing a pending link is a
        // READ-side operation (the first gated getter is what pulls the result in), and the
        // publish has to bump these. Still GL-thread-only - a worker never touches them.
        mutable Uint32 m_backendStateVersion = 0;
        // Interface-block binding generation; see GetBlockBindingVersion.
        Uint32 m_blockBindingVersion = 0;

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
