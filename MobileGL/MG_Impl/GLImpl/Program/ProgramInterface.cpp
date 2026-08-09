// MobileGL - MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramInterface.h"

#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <cstring>

namespace MobileGL::MG_Impl::GLImpl::ProgramInterface {
    namespace {
        // glslang folds atomic counters into synthesized blocks named
        // "<getAtomicCounterBlockName()>_<binding>" (ParseContextBase.cpp), one per GL
        // atomic-counter binding point. That block IS the GL_ATOMIC_COUNTER_BUFFER resource
        // and its trailing number IS GL_BUFFER_BINDING; its members stay GL_UNIFORMs.
        constexpr const char* kAtomicCounterBlockPrefix = "gl_AtomicCounterBlock";

        enum class BlockKind {
            Uniform,       // a real GL uniform block
            GlobalUbo,     // the synthesized MGL_GLOBAL_UBO: GL sees its members as default-block
            AtomicCounter, // gl_AtomicCounterBlock_<binding>
            Storage,       // a shader storage block
        };

        // One row of any interface. Fields a given interface does not have keep the
        // spec-mandated "not applicable" value, so a prop read never has to special-case
        // the interface a second time.
        struct Resource {
            String name;
            GLenum type = GL_NONE;
            GLint arraySize = 1;
            GLint location = -1;
            GLint locationIndex = -1;
            GLint blockIndex = -1;
            GLint offset = -1;
            GLint arrayStride = -1;
            GLint matrixStride = -1;
            GLint isRowMajor = 0;
            GLint atomicCounterBufferIndex = -1;
            GLint topLevelArraySize = 0;
            GLint topLevelArrayStride = 0;
            GLint bufferBinding = 0;
            GLint bufferDataSize = 0;
            GLint isPerPatch = 0;
            GLint xfbBufferIndex = 0;
            Uint32 stages = 0; // EShLanguageMask
            Vector<GLuint> activeVariables;
        };

        using ResourceList = Vector<Resource>;

        struct Model {
            ResourceList uniforms;
            ResourceList uniformBlocks;
            ResourceList atomicCounterBuffers;
            ResourceList bufferVariables;
            ResourceList storageBlocks;
            ResourceList programInputs;
            ResourceList programOutputs;
            ResourceList xfbVaryings;
            Bool valid = false;
        };

        const ResourceList& EmptyList() {
            static const ResourceList empty;
            return empty;
        }

        // ---- name spelling (cluster 6) -------------------------------------------------

        Bool EndsWithZeroSubscript(const String& name) {
            return name.length() >= 3 && name.compare(name.length() - 3, 3, "[0]") == 0;
        }

        // The enumerated spelling of an array resource is "name[0]". glslang already applies
        // that to uniforms and buffer variables (EShReflectionBasicArraySuffix), but never to
        // stage inputs/outputs, so those get it here.
        String WithArraySuffix(const String& name, const glslang::TType* type) {
            if (type == nullptr || !type->isArray() || EndsWithZeroSubscript(name)) return name;
            return name + "[0]";
        }

        // GL_ARRAY_SIZE: element count for a sized array, 0 for a runtime-sized one
        // (a shader storage block's unsized trailing member), 1 for a non-array.
        GLint ArraySizeOf(const glslang::TType* type, GLint reflectedSize) {
            if (type != nullptr && type->isArray()) {
                if (!type->isSizedArray()) return 0;
                return type->getOuterArraySize();
            }
            return reflectedSize < 1 ? 1 : reflectedSize;
        }

        // Two spellings name the same resource when they are equal, or differ only by the
        // "[0]" the enumeration appends to an array.
        Bool NamesMatch(const String& resourceName, const String& query) {
            if (resourceName == query) return true;
            if (EndsWithZeroSubscript(resourceName) &&
                resourceName.compare(0, resourceName.length() - 3, query) == 0) {
                return true;
            }
            return EndsWithZeroSubscript(query) && query.compare(0, query.length() - 3, resourceName) == 0;
        }

        // Splits "base[k]" into ("base", k). GL 4.6 §7.3.1.1 requires the subscript to be a
        // decimal integer with no white space and no leading zeros, which is exactly what
        // separates array-names' "a[1]" (resolves) from "a[01]", "a[0 + 0]" and "a[ 0]" (do
        // not). Returns false when there is no trailing subscript at all; sets `malformed`
        // when there is one but it is not a strict decimal.
        Bool SplitTrailingSubscript(const String& name, String& outBase, Uint& outElement, Bool& outMalformed) {
            outMalformed = false;
            if (name.empty() || name.back() != ']') return false;
            const SizeT bracket = name.rfind('[');
            if (bracket == String::npos) return false;
            const SizeT first = bracket + 1;
            const SizeT last = name.length() - 1; // one past the digits
            if (first >= last) {
                outMalformed = true;
                return false;
            }
            // No leading zeros: "0" is the only spelling that may start with '0'.
            if (name[first] == '0' && last - first > 1) {
                outMalformed = true;
                return false;
            }
            Uint element = 0;
            for (SizeT i = first; i < last; ++i) {
                if (name[i] < '0' || name[i] > '9') {
                    outMalformed = true;
                    return false;
                }
                element = element * 10 + static_cast<Uint>(name[i] - '0');
                if (element > 0x0FFFFFFFu) {
                    outMalformed = true;
                    return false;
                }
            }
            outBase = name.substr(0, bracket);
            outElement = element;
            return true;
        }

        // ---- block classification ------------------------------------------------------

        Bool IsAtomicCounterBlockName(const String& name) {
            return name.compare(0, std::strlen(kAtomicCounterBlockPrefix), kAtomicCounterBlockPrefix) == 0;
        }

        // "gl_AtomicCounterBlock_5" -> 5. The suffix is the GL binding the counters were
        // declared with, which glslang does NOT keep in the block's own layout qualifier
        // (that one is remapped to a plain buffer binding).
        GLint AtomicCounterBlockBinding(const String& name) {
            const SizeT underscore = name.rfind('_');
            if (underscore == String::npos || underscore + 1 >= name.length()) return 0;
            GLint binding = 0;
            for (SizeT i = underscore + 1; i < name.length(); ++i) {
                if (name[i] < '0' || name[i] > '9') return 0;
                binding = binding * 10 + (name[i] - '0');
            }
            return binding;
        }

        // Element index of an arrayed block instance ("TrickyBuffer[1]" -> 1).
        GLint BlockArrayElement(const String& name) {
            String base;
            Uint element = 0;
            Bool malformed = false;
            if (!SplitTrailingSubscript(name, base, element, malformed)) return 0;
            return static_cast<GLint>(element);
        }

        BlockKind ClassifyBlock(const glslang::TObjectReflection& block) {
            if (std::strstr(block.name.c_str(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != nullptr) {
                return BlockKind::GlobalUbo;
            }
            if (IsAtomicCounterBlockName(block.name)) return BlockKind::AtomicCounter;
            const glslang::TType* type = block.getType();
            if (type != nullptr && type->getQualifier().storage == glslang::EvqBuffer) return BlockKind::Storage;
            return BlockKind::Uniform;
        }

        // std140/std430 column stride, the same vec4-rounded rule ProgramObject applies to
        // uniform matrices. 0 for a non-matrix.
        GLint MatrixStrideOf(const glslang::TType* type) {
            if (type == nullptr || !type->isMatrix()) return 0;
            const bool rowMajor = type->getQualifier().layoutMatrix == glslang::ElmRowMajor;
            const int strideVectorComponents = rowMajor ? type->getMatrixCols() : type->getMatrixRows();
            constexpr int scalarSize = 4;
            const int vectorAlignment = (strideVectorComponents <= 1)    ? scalarSize
                                        : (strideVectorComponents == 2) ? 2 * scalarSize
                                                                        : 4 * scalarSize;
            return (vectorAlignment + 15) & ~15;
        }

        GLint IsRowMajorOf(const glslang::TType* type) {
            if (type == nullptr || !type->isMatrix()) return 0;
            return type->getQualifier().layoutMatrix == glslang::ElmRowMajor ? 1 : 0;
        }

        GLint MappedLocation(Int rawLocation) {
            // glslang parks "no location" at layoutLocationEnd; GL spells it -1.
            if (rawLocation < 0 || rawLocation >= static_cast<Int>(glslang::TQualifier::layoutLocationEnd)) return -1;
            return rawLocation;
        }

        // ---- model construction --------------------------------------------------------

        void BuildBlocks(ProgramObject& program, const glslang::TProgram& reflection, Model& model,
                         Vector<BlockKind>& blockKind, Vector<Int>& blockInterfaceIndex) {
            const Int blockCount = const_cast<glslang::TProgram&>(reflection).getNumUniformBlocks();
            blockKind.assign(blockCount, BlockKind::Uniform);
            blockInterfaceIndex.assign(blockCount, -1);

            for (Int tIndex = 0; tIndex < blockCount; ++tIndex) {
                const auto& block = const_cast<glslang::TProgram&>(reflection).getUniformBlock(tIndex);
                const BlockKind kind = ClassifyBlock(block);
                blockKind[tIndex] = kind;
                if (kind == BlockKind::AtomicCounter) {
                    Resource resource;
                    // GL_ATOMIC_COUNTER_BUFFER resources have no name (and GetProgramResource
                    // Index/Name reject the interface outright, which is why this stays empty).
                    resource.bufferBinding = AtomicCounterBlockBinding(block.name);
                    resource.bufferDataSize = block.size;
                    resource.stages = static_cast<Uint32>(block.stages);
                    blockInterfaceIndex[tIndex] = static_cast<Int>(model.atomicCounterBuffers.size());
                    model.atomicCounterBuffers.push_back(Move(resource));
                } else if (kind == BlockKind::Storage) {
                    Resource resource;
                    resource.name = block.name;
                    // glslang reports the DECLARED binding for every instance of an arrayed
                    // block; GL gives element k the binding base + k. That is only the initial
                    // value: GL_BUFFER_BINDING must report the CURRENT binding, so a later
                    // glShaderStorageBlockBinding wins over the declaration (GL 4.6 §7.6.2 -
                    // exactly the same rule GL_UNIFORM_BLOCK follows through
                    // GetUniformBlockBinding below).
                    const GLint declared = block.getBinding();
                    resource.bufferBinding = declared < 0 ? 0 : declared + BlockArrayElement(block.name);
                    const Int rebound = program.GetShaderStorageBlockBindingOverride(block.name);
                    if (rebound >= 0) resource.bufferBinding = static_cast<GLint>(rebound);
                    resource.bufferDataSize = block.size;
                    resource.stages = static_cast<Uint32>(block.stages);
                    blockInterfaceIndex[tIndex] = static_cast<Int>(model.storageBlocks.size());
                    model.storageBlocks.push_back(Move(resource));
                }
            }

            // GL_UNIFORM_BLOCK keeps the index space glUniformBlockBinding and
            // glGetActiveUniformBlockiv already use, so an index handed out here is usable
            // with them (which is exactly what the CTS does).
            const Int glBlockCount = program.GetActiveUniformBlocksCount();
            for (Int glIndex = 0; glIndex < glBlockCount; ++glIndex) {
                Resource resource;
                resource.name = program.GetUniformBlockName(glIndex);
                resource.bufferBinding = static_cast<GLint>(program.GetUniformBlockBinding(glIndex));
                resource.bufferDataSize = static_cast<GLint>(program.GetUBOSizeAt(glIndex));
                const Int tIndex = program.TProgramBlockIndex(static_cast<Uint>(glIndex));
                if (tIndex >= 0 && tIndex < blockCount) {
                    resource.stages =
                        static_cast<Uint32>(const_cast<glslang::TProgram&>(reflection).getUniformBlock(tIndex).stages);
                }
                model.uniformBlocks.push_back(Move(resource));
            }
        }

        void BuildUniformsAndBufferVariables(ProgramObject& program, const glslang::TProgram& reflection, Model& model,
                                             const Vector<BlockKind>& blockKind,
                                             const Vector<Int>& blockInterfaceIndex) {
            const Uint uniformCount = program.GetUniformCount();
            for (Uint glIndex = 0; glIndex < uniformCount; ++glIndex) {
                const Int tIndex = program.TProgramUniformIndex(glIndex);
                const auto& refl = const_cast<glslang::TProgram&>(reflection).getUniform(tIndex);
                const glslang::TType* type = refl.getType();
                const Int owner = refl.index;
                const BlockKind kind = (owner >= 0 && owner < static_cast<Int>(blockKind.size()))
                                           ? blockKind[owner]
                                           : BlockKind::GlobalUbo;

                Resource resource;
                resource.name = refl.name;
                resource.type = static_cast<GLenum>(refl.glDefineType);
                resource.arraySize = ArraySizeOf(type, refl.size);
                resource.stages = static_cast<Uint32>(refl.stages);

                if (kind == BlockKind::Storage) {
                    resource.blockIndex = blockInterfaceIndex[owner];
                    resource.offset = refl.offset;
                    resource.arrayStride = refl.arrayStride;
                    resource.matrixStride = MatrixStrideOf(type);
                    resource.isRowMajor = IsRowMajorOf(type);
                    // GL requires 1 for a member that is not inside a top-level array (and for
                    // the top-level array itself); glslang leaves 0/-1 there.
                    resource.topLevelArraySize = refl.topLevelArraySize > 0 ? refl.topLevelArraySize : 1;
                    resource.topLevelArrayStride = refl.topLevelArrayStride;
                    model.bufferVariables.push_back(Move(resource));
                    continue;
                }

                if (kind == BlockKind::AtomicCounter) {
                    // An atomic counter is a default-block uniform with no location and no
                    // owning uniform block; what it does have is a buffer to point at.
                    resource.type = GL_UNSIGNED_INT_ATOMIC_COUNTER;
                    resource.blockIndex = -1;
                    resource.offset = refl.offset;
                    resource.arrayStride = refl.arrayStride;
                    resource.matrixStride = 0;
                    resource.atomicCounterBufferIndex = blockInterfaceIndex[owner];
                    resource.location = -1;
                } else {
                    resource.blockIndex = program.GetActiveUniformBlockIndex(glIndex);
                    resource.offset = program.GetActiveUniformOffset(glIndex);
                    resource.arrayStride = program.GetActiveUniformArrayStride(glIndex);
                    resource.matrixStride = program.GetActiveUniformMatrixStride(glIndex);
                    resource.isRowMajor = program.GetActiveUniformIsRowMajor(glIndex);
                    // A member of a named uniform block has no location, whatever the
                    // frontend's own location table says (it hands one out to every uniform
                    // so glUniform* can address block members through the global UBO).
                    resource.location =
                        resource.blockIndex >= 0 ? -1 : program.GetUniformLocation(refl.name);
                }
                model.uniforms.push_back(Move(resource));
            }

            // GL_ACTIVE_VARIABLES, both directions.
            for (SizeT i = 0; i < model.uniforms.size(); ++i) {
                const Resource& uniform = model.uniforms[i];
                if (uniform.atomicCounterBufferIndex >= 0 &&
                    uniform.atomicCounterBufferIndex < static_cast<GLint>(model.atomicCounterBuffers.size())) {
                    model.atomicCounterBuffers[uniform.atomicCounterBufferIndex].activeVariables.push_back(
                        static_cast<GLuint>(i));
                }
            }
            for (SizeT blockIndex = 0; blockIndex < model.uniformBlocks.size(); ++blockIndex) {
                // Members of an arrayed block are reflected once, against instance [0].
                const Int owner = static_cast<Int>(program.GetUniformBlockMemberOwnerIndex(static_cast<Uint>(blockIndex)));
                for (SizeT i = 0; i < model.uniforms.size(); ++i) {
                    if (model.uniforms[i].blockIndex == owner) {
                        model.uniformBlocks[blockIndex].activeVariables.push_back(static_cast<GLuint>(i));
                    }
                }
            }
            for (SizeT blockIndex = 0; blockIndex < model.storageBlocks.size(); ++blockIndex) {
                for (SizeT i = 0; i < model.bufferVariables.size(); ++i) {
                    if (model.bufferVariables[i].blockIndex == static_cast<GLint>(blockIndex)) {
                        model.storageBlocks[blockIndex].activeVariables.push_back(static_cast<GLuint>(i));
                    }
                }
            }
        }

        void BuildStageIO(ProgramObject& program, const glslang::TProgram& reflection, Model& model) {
            auto& mutableReflection = const_cast<glslang::TProgram&>(reflection);

            const Int inputCount = mutableReflection.getNumPipeInputs();
            for (Int index = 0; index < inputCount; ++index) {
                const auto& refl = mutableReflection.getPipeInput(index);
                const glslang::TType* type = refl.getType();
                Resource resource;
                // The Vulkan-semantics parse reflects the vertex builtins under their SPIR-V
                // names; GL enumerates the GL spellings.
                const String& glName = ProgramObject::NormalizeBuiltinPipeInputName(refl.name);
                resource.name = WithArraySuffix(glName, type);
                resource.type = static_cast<GLenum>(refl.glDefineType);
                resource.arraySize = ArraySizeOf(type, refl.size);
                resource.location = program.GetAttributeLocation(refl.name);
                if (resource.location < 0) resource.location = MappedLocation(static_cast<Int>(refl.layoutLocation()));
                resource.isPerPatch = (type != nullptr && type->getQualifier().patch) ? 1 : 0;
                resource.stages = static_cast<Uint32>(refl.stages);
                model.programInputs.push_back(Move(resource));
            }

            const Int outputCount = mutableReflection.getNumPipeOutputs();
            for (Int index = 0; index < outputCount; ++index) {
                const auto& refl = mutableReflection.getPipeOutput(index);
                const glslang::TType* type = refl.getType();
                Resource resource;
                resource.name = WithArraySuffix(refl.name, type);
                resource.type = static_cast<GLenum>(refl.glDefineType);
                resource.arraySize = ArraySizeOf(type, refl.size);
                resource.location = MappedLocation(program.GetFragmentDataLocation(refl.name.c_str()));
                if (resource.location < 0) {
                    // A built-in output (gl_FragDepth, gl_SampleMask) and a non-fragment stage
                    // output both have no location, and therefore no color index either.
                    resource.locationIndex = -1;
                } else {
                    resource.locationIndex = program.GetFragmentDataIndex(refl.name.c_str());
                    // glBindFragDataLocationIndexed wins; otherwise the shader's
                    // layout(index = N), which the frag-data maps never saw.
                    if (resource.locationIndex == 0 && type != nullptr && type->getQualifier().hasIndex()) {
                        resource.locationIndex = static_cast<GLint>(type->getQualifier().layoutIndex);
                    }
                }
                resource.isPerPatch = (type != nullptr && type->getQualifier().patch) ? 1 : 0;
                resource.stages = static_cast<Uint32>(refl.stages);
                model.programOutputs.push_back(Move(resource));
            }
        }

        void BuildXfb(ProgramObject& program, Model& model) {
            const auto& requested = program.GetTransformFeedbackInterfaceNames();
            const auto& captured = program.GetTransformFeedbackVaryings();
            for (const String& name : requested) {
                Resource resource;
                resource.name = name;
                // ARB_transform_feedback3's layout controls are enumerated as resources of
                // type NONE: gl_NextBuffer with array size 0, gl_SkipComponentsN with N.
                if (name == "gl_NextBuffer") {
                    resource.type = GL_NONE;
                    resource.arraySize = 0;
                } else if (name.size() == 18 && name.compare(0, 17, "gl_SkipComponents") == 0 && name[17] >= '1' &&
                           name[17] <= '4') {
                    resource.type = GL_NONE;
                    resource.arraySize = name[17] - '0';
                } else {
                    resource.type = GL_NONE;
                    resource.arraySize = 1;
                    for (const auto& varying : captured) {
                        if (varying.name != name) continue;
                        resource.type = varying.type;
                        resource.arraySize = varying.size < 1 ? 1 : varying.size;
                        resource.offset = static_cast<GLint>(varying.offsetBytes);
                        resource.xfbBufferIndex = static_cast<GLint>(varying.bufferIndex);
                        break;
                    }
                }
                model.xfbVaryings.push_back(Move(resource));
            }
        }

        Model BuildModel(ProgramObject& program) {
            Model model;
            if (!program.GetLinkStatus()) return model;
            const glslang::TProgram* reflection = program.GetReflection();
            if (reflection == nullptr) return model;
            model.valid = true;

            Vector<BlockKind> blockKind;
            Vector<Int> blockInterfaceIndex;
            BuildBlocks(program, *reflection, model, blockKind, blockInterfaceIndex);
            BuildUniformsAndBufferVariables(program, *reflection, model, blockKind, blockInterfaceIndex);
            BuildStageIO(program, *reflection, model);
            BuildXfb(program, model);
            return model;
        }

        const ResourceList& Select(const Model& model, GLenum programInterface) {
            switch (programInterface) {
            case GL_UNIFORM:
                return model.uniforms;
            case GL_UNIFORM_BLOCK:
                return model.uniformBlocks;
            case GL_ATOMIC_COUNTER_BUFFER:
                return model.atomicCounterBuffers;
            case GL_BUFFER_VARIABLE:
                return model.bufferVariables;
            case GL_SHADER_STORAGE_BLOCK:
                return model.storageBlocks;
            case GL_PROGRAM_INPUT:
                return model.programInputs;
            case GL_PROGRAM_OUTPUT:
                return model.programOutputs;
            case GL_TRANSFORM_FEEDBACK_VARYING:
                return model.xfbVaryings;
            default:
                // The subroutine interfaces are accepted by the API but nothing can populate
                // them: glslang refuses `subroutine` when generating SPIR-V, so a program
                // using one never links. Zero active resources is the honest answer.
                return EmptyList();
            }
        }
    } // namespace

    Bool IsInterfaceEnum(GLenum programInterface) {
        switch (programInterface) {
        case GL_UNIFORM:
        case GL_UNIFORM_BLOCK:
        case GL_PROGRAM_INPUT:
        case GL_PROGRAM_OUTPUT:
        case GL_BUFFER_VARIABLE:
        case GL_SHADER_STORAGE_BLOCK:
        case GL_ATOMIC_COUNTER_BUFFER:
        case GL_TRANSFORM_FEEDBACK_VARYING:
        case GL_TRANSFORM_FEEDBACK_BUFFER:
        case GL_VERTEX_SUBROUTINE:
        case GL_TESS_CONTROL_SUBROUTINE:
        case GL_TESS_EVALUATION_SUBROUTINE:
        case GL_GEOMETRY_SUBROUTINE:
        case GL_FRAGMENT_SUBROUTINE:
        case GL_COMPUTE_SUBROUTINE:
        case GL_VERTEX_SUBROUTINE_UNIFORM:
        case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
        case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
        case GL_GEOMETRY_SUBROUTINE_UNIFORM:
        case GL_FRAGMENT_SUBROUTINE_UNIFORM:
        case GL_COMPUTE_SUBROUTINE_UNIFORM:
            return true;
        default:
            return false;
        }
    }

    Bool IsNamedInterface(GLenum programInterface) {
        // GL 4.6 §7.3.1.2: the two buffer interfaces have no resource names, and asking for
        // one is INVALID_ENUM (deliberately asymmetric with GetProgramInterfaceiv, which
        // does count them).
        return IsInterfaceEnum(programInterface) && programInterface != GL_ATOMIC_COUNTER_BUFFER &&
               programInterface != GL_TRANSFORM_FEEDBACK_BUFFER;
    }

    Bool InterfaceHasLocations(GLenum programInterface) {
        switch (programInterface) {
        case GL_UNIFORM:
        case GL_PROGRAM_INPUT:
        case GL_PROGRAM_OUTPUT:
        case GL_VERTEX_SUBROUTINE_UNIFORM:
        case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
        case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
        case GL_GEOMETRY_SUBROUTINE_UNIFORM:
        case GL_FRAGMENT_SUBROUTINE_UNIFORM:
        case GL_COMPUTE_SUBROUTINE_UNIFORM:
            return true;
        default:
            return false;
        }
    }

    Bool IsResourceProp(GLenum prop) {
        switch (prop) {
        case GL_NAME_LENGTH:
        case GL_TYPE:
        case GL_ARRAY_SIZE:
        case GL_OFFSET:
        case GL_BLOCK_INDEX:
        case GL_ARRAY_STRIDE:
        case GL_MATRIX_STRIDE:
        case GL_IS_ROW_MAJOR:
        case GL_ATOMIC_COUNTER_BUFFER_INDEX:
        case GL_BUFFER_BINDING:
        case GL_BUFFER_DATA_SIZE:
        case GL_NUM_ACTIVE_VARIABLES:
        case GL_ACTIVE_VARIABLES:
        case GL_REFERENCED_BY_VERTEX_SHADER:
        case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
        case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
        case GL_REFERENCED_BY_GEOMETRY_SHADER:
        case GL_REFERENCED_BY_FRAGMENT_SHADER:
        case GL_REFERENCED_BY_COMPUTE_SHADER:
        case GL_TOP_LEVEL_ARRAY_SIZE:
        case GL_TOP_LEVEL_ARRAY_STRIDE:
        case GL_LOCATION:
        case GL_LOCATION_INDEX:
        case GL_IS_PER_PATCH:
        case GL_LOCATION_COMPONENT:
        case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX:
        case GL_TRANSFORM_FEEDBACK_BUFFER_STRIDE:
        case GL_NUM_COMPATIBLE_SUBROUTINES:
        case GL_COMPATIBLE_SUBROUTINES:
            return true;
        default:
            return false;
        }
    }

    // GL 4.6 Table 7.2, transcribed row by row: which interfaces each property applies to.
    // Too tight a table turns a currently-answered prop into a fresh INVALID_OPERATION, so
    // the rows below are deliberately no narrower than the spec's.
    Bool InterfaceSupportsProp(GLenum programInterface, GLenum prop) {
        const Bool isSubroutine =
            programInterface == GL_VERTEX_SUBROUTINE || programInterface == GL_TESS_CONTROL_SUBROUTINE ||
            programInterface == GL_TESS_EVALUATION_SUBROUTINE || programInterface == GL_GEOMETRY_SUBROUTINE ||
            programInterface == GL_FRAGMENT_SUBROUTINE || programInterface == GL_COMPUTE_SUBROUTINE;
        const Bool isSubroutineUniform =
            programInterface == GL_VERTEX_SUBROUTINE_UNIFORM ||
            programInterface == GL_TESS_CONTROL_SUBROUTINE_UNIFORM ||
            programInterface == GL_TESS_EVALUATION_SUBROUTINE_UNIFORM ||
            programInterface == GL_GEOMETRY_SUBROUTINE_UNIFORM ||
            programInterface == GL_FRAGMENT_SUBROUTINE_UNIFORM || programInterface == GL_COMPUTE_SUBROUTINE_UNIFORM;

        switch (prop) {
        case GL_NAME_LENGTH:
            return programInterface != GL_ATOMIC_COUNTER_BUFFER && programInterface != GL_TRANSFORM_FEEDBACK_BUFFER;
        case GL_TYPE:
        case GL_ARRAY_SIZE:
            return programInterface == GL_UNIFORM || programInterface == GL_PROGRAM_INPUT ||
                   programInterface == GL_PROGRAM_OUTPUT || programInterface == GL_BUFFER_VARIABLE ||
                   programInterface == GL_TRANSFORM_FEEDBACK_VARYING ||
                   (prop == GL_ARRAY_SIZE && isSubroutineUniform);
        case GL_OFFSET:
            return programInterface == GL_UNIFORM || programInterface == GL_BUFFER_VARIABLE ||
                   programInterface == GL_TRANSFORM_FEEDBACK_VARYING;
        case GL_BLOCK_INDEX:
        case GL_ARRAY_STRIDE:
        case GL_MATRIX_STRIDE:
        case GL_IS_ROW_MAJOR:
            return programInterface == GL_UNIFORM || programInterface == GL_BUFFER_VARIABLE;
        case GL_ATOMIC_COUNTER_BUFFER_INDEX:
            return programInterface == GL_UNIFORM;
        case GL_BUFFER_BINDING:
        case GL_NUM_ACTIVE_VARIABLES:
        case GL_ACTIVE_VARIABLES:
            // Table 7.2 lists GL_TRANSFORM_FEEDBACK_BUFFER on these three rows too. This
            // implementation enumerates no resources on that interface, so the query still
            // ends in an error - but INVALID_VALUE for the out-of-range index, not the
            // INVALID_OPERATION a narrower table would invent.
            return programInterface == GL_UNIFORM_BLOCK || programInterface == GL_ATOMIC_COUNTER_BUFFER ||
                   programInterface == GL_SHADER_STORAGE_BLOCK ||
                   programInterface == GL_TRANSFORM_FEEDBACK_BUFFER;
        case GL_BUFFER_DATA_SIZE:
            return programInterface == GL_UNIFORM_BLOCK || programInterface == GL_ATOMIC_COUNTER_BUFFER ||
                   programInterface == GL_SHADER_STORAGE_BLOCK;
        case GL_REFERENCED_BY_VERTEX_SHADER:
        case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
        case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
        case GL_REFERENCED_BY_GEOMETRY_SHADER:
        case GL_REFERENCED_BY_FRAGMENT_SHADER:
        case GL_REFERENCED_BY_COMPUTE_SHADER:
            return programInterface == GL_UNIFORM || programInterface == GL_UNIFORM_BLOCK ||
                   programInterface == GL_ATOMIC_COUNTER_BUFFER || programInterface == GL_BUFFER_VARIABLE ||
                   programInterface == GL_SHADER_STORAGE_BLOCK || programInterface == GL_PROGRAM_INPUT ||
                   programInterface == GL_PROGRAM_OUTPUT || isSubroutineUniform;
        case GL_TOP_LEVEL_ARRAY_SIZE:
        case GL_TOP_LEVEL_ARRAY_STRIDE:
            return programInterface == GL_BUFFER_VARIABLE;
        case GL_LOCATION:
            return InterfaceHasLocations(programInterface);
        case GL_LOCATION_INDEX:
            return programInterface == GL_PROGRAM_OUTPUT;
        case GL_IS_PER_PATCH:
        case GL_LOCATION_COMPONENT:
            return programInterface == GL_PROGRAM_INPUT || programInterface == GL_PROGRAM_OUTPUT;
        case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX:
            return programInterface == GL_TRANSFORM_FEEDBACK_VARYING;
        case GL_TRANSFORM_FEEDBACK_BUFFER_STRIDE:
            return programInterface == GL_TRANSFORM_FEEDBACK_BUFFER;
        case GL_NUM_COMPATIBLE_SUBROUTINES:
        case GL_COMPATIBLE_SUBROUTINES:
            return isSubroutineUniform;
        default:
            (void)isSubroutine;
            return false;
        }
    }

    Int GetActiveResourceCount(ProgramObject& program, GLenum programInterface) {
        const Model model = BuildModel(program);
        return static_cast<Int>(Select(model, programInterface).size());
    }

    Int GetMaxNameLength(ProgramObject& program, GLenum programInterface) {
        if (!IsNamedInterface(programInterface)) return 0;
        const Model model = BuildModel(program);
        SizeT longest = 0;
        for (const Resource& resource : Select(model, programInterface)) {
            longest = std::max(longest, resource.name.length() + 1);
        }
        return static_cast<Int>(longest);
    }

    Int GetMaxNumActiveVariables(ProgramObject& program, GLenum programInterface) {
        const Model model = BuildModel(program);
        SizeT longest = 0;
        for (const Resource& resource : Select(model, programInterface)) {
            longest = std::max(longest, resource.activeVariables.size());
        }
        return static_cast<Int>(longest);
    }

    GLuint GetResourceIndex(ProgramObject& program, GLenum programInterface, const char* name) {
        if (name == nullptr || name[0] == '\0') return GL_INVALID_INDEX;
        const Model model = BuildModel(program);
        const ResourceList& resources = Select(model, programInterface);
        const String query = name;
        // The layout controls of an interleaved capture are enumerable but not addressable
        // by name (GL 4.6 §7.3.1.1).
        if (programInterface == GL_TRANSFORM_FEEDBACK_VARYING &&
            (query == "gl_NextBuffer" ||
             (query.size() == 18 && query.compare(0, 17, "gl_SkipComponents") == 0))) {
            return GL_INVALID_INDEX;
        }
        for (SizeT i = 0; i < resources.size(); ++i) {
            if (NamesMatch(resources[i].name, query)) return static_cast<GLuint>(i);
        }
        return GL_INVALID_INDEX;
    }

    Bool GetResourceName(ProgramObject& program, GLenum programInterface, GLuint index, String& outName) {
        const Model model = BuildModel(program);
        const ResourceList& resources = Select(model, programInterface);
        if (index >= resources.size()) return false;
        outName = resources[index].name;
        return true;
    }

    Bool GetResourceProp(ProgramObject& program, GLenum programInterface, GLuint index, GLenum prop,
                         Vector<GLint>& outValues) {
        const Model model = BuildModel(program);
        const ResourceList& resources = Select(model, programInterface);
        if (index >= resources.size()) return false;
        const Resource& resource = resources[index];

        const auto referencedBy = [&resource](EShLanguage stage) {
            return (resource.stages & static_cast<Uint32>(1u << stage)) != 0 ? GL_TRUE : GL_FALSE;
        };

        switch (prop) {
        case GL_NAME_LENGTH:
            outValues.push_back(static_cast<GLint>(resource.name.length() + 1));
            break;
        case GL_TYPE:
            outValues.push_back(static_cast<GLint>(resource.type));
            break;
        case GL_ARRAY_SIZE:
            outValues.push_back(resource.arraySize);
            break;
        case GL_OFFSET:
            outValues.push_back(resource.offset);
            break;
        case GL_BLOCK_INDEX:
            outValues.push_back(resource.blockIndex);
            break;
        case GL_ARRAY_STRIDE:
            outValues.push_back(resource.arrayStride);
            break;
        case GL_MATRIX_STRIDE:
            outValues.push_back(resource.matrixStride);
            break;
        case GL_IS_ROW_MAJOR:
            outValues.push_back(resource.isRowMajor);
            break;
        case GL_ATOMIC_COUNTER_BUFFER_INDEX:
            outValues.push_back(resource.atomicCounterBufferIndex);
            break;
        case GL_BUFFER_BINDING:
            outValues.push_back(resource.bufferBinding);
            break;
        case GL_BUFFER_DATA_SIZE:
            outValues.push_back(resource.bufferDataSize);
            break;
        case GL_NUM_ACTIVE_VARIABLES:
            outValues.push_back(static_cast<GLint>(resource.activeVariables.size()));
            break;
        case GL_ACTIVE_VARIABLES:
            for (const GLuint variable : resource.activeVariables) outValues.push_back(static_cast<GLint>(variable));
            break;
        case GL_REFERENCED_BY_VERTEX_SHADER:
            outValues.push_back(referencedBy(EShLangVertex));
            break;
        case GL_REFERENCED_BY_TESS_CONTROL_SHADER:
            outValues.push_back(referencedBy(EShLangTessControl));
            break;
        case GL_REFERENCED_BY_TESS_EVALUATION_SHADER:
            outValues.push_back(referencedBy(EShLangTessEvaluation));
            break;
        case GL_REFERENCED_BY_GEOMETRY_SHADER:
            outValues.push_back(referencedBy(EShLangGeometry));
            break;
        case GL_REFERENCED_BY_FRAGMENT_SHADER:
            outValues.push_back(referencedBy(EShLangFragment));
            break;
        case GL_REFERENCED_BY_COMPUTE_SHADER:
            outValues.push_back(referencedBy(EShLangCompute));
            break;
        case GL_TOP_LEVEL_ARRAY_SIZE:
            outValues.push_back(resource.topLevelArraySize);
            break;
        case GL_TOP_LEVEL_ARRAY_STRIDE:
            outValues.push_back(resource.topLevelArrayStride);
            break;
        case GL_LOCATION:
            outValues.push_back(resource.location);
            break;
        case GL_LOCATION_INDEX:
            outValues.push_back(resource.locationIndex);
            break;
        case GL_IS_PER_PATCH:
            outValues.push_back(resource.isPerPatch);
            break;
        case GL_LOCATION_COMPONENT:
            outValues.push_back(0);
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER_INDEX:
            outValues.push_back(resource.xfbBufferIndex);
            break;
        default:
            outValues.push_back(0);
            break;
        }
        return true;
    }

    GLint GetResourceLocation(ProgramObject& program, GLenum programInterface, const char* name) {
        if (name == nullptr || name[0] == '\0') return -1;
        const String query = name;

        String base;
        Uint element = 0;
        Bool malformed = false;
        const Bool subscripted = SplitTrailingSubscript(query, base, element, malformed);
        if (malformed) return -1;

        const Model model = BuildModel(program);
        const ResourceList& resources = Select(model, programInterface);
        for (const Resource& resource : resources) {
            if (NamesMatch(resource.name, query)) return resource.location;
        }
        if (!subscripted || element == 0) return -1;
        // "d[1]" addresses the second element of an array resource enumerated as "d[0]".
        for (const Resource& resource : resources) {
            if (!NamesMatch(resource.name, base)) continue;
            if (resource.location < 0 || static_cast<GLint>(element) >= resource.arraySize) return -1;
            return resource.location + static_cast<GLint>(element);
        }
        return -1;
    }

    GLint GetResourceLocationIndex(ProgramObject& program, GLenum programInterface, const char* name) {
        if (programInterface != GL_PROGRAM_OUTPUT || name == nullptr || name[0] == '\0') return -1;
        const String query = name;
        String base;
        Uint element = 0;
        Bool malformed = false;
        const Bool subscripted = SplitTrailingSubscript(query, base, element, malformed);
        if (malformed) return -1;

        const Model model = BuildModel(program);
        for (const Resource& resource : model.programOutputs) {
            if (NamesMatch(resource.name, query)) return resource.locationIndex;
        }
        if (!subscripted) return -1;
        for (const Resource& resource : model.programOutputs) {
            if (!NamesMatch(resource.name, base)) continue;
            if (resource.location < 0 || static_cast<GLint>(element) >= resource.arraySize) return -1;
            return resource.locationIndex;
        }
        return -1;
    }
} // namespace MobileGL::MG_Impl::GLImpl::ProgramInterface
