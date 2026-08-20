// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramLinkTask.h"

#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <cstring>

namespace {
    // How many vertex input locations reflection may record. Backends consume this through
    // GetActiveAttributeLocationMask()/GetAttribType(), so a value below the advertised
    // GL_MAX_VERTEX_ATTRIBS would make a legal attribute location invisible to them -- DirectGLES would
    // then never feed the shader that attribute's current value. Bounded by the state layer's storage
    // capacity, which is also the width of the Uint32 masks backends build from it.
    static MobileGL::Int GetReflectionVertexAttribLimit(
        const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env) {
        constexpr MobileGL::Int capacity =
            static_cast<MobileGL::Int>(MobileGL::MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
        if (!env.HasBackend()) return capacity;

        const MobileGL::Int backendLimit = env.params.MaxVertexAttribs;
        if (backendLimit <= 0) return capacity;
        return std::min(backendLimit, capacity);
    }

    static MobileGL::String StripArrayElementSuffix(const MobileGL::String& name) {
        const MobileGL::SizeT bracket = name.find('[');
        return bracket == MobileGL::String::npos ? name : name.substr(0, bracket);
    }

    // Element index of an arrayed interface-block instance: "GOKU[3]" -> 3, "GOKU" -> 0.
    // Reflection spells arrayed instances exactly this way (glslang expands the instance
    // array into one TObjectReflection per element), and the subscript it writes is a plain
    // decimal, so a strict-decimal parse is both sufficient and the same rule GL 4.6
    // 7.3.1.1 puts on the name a program-resource query may use.
    static MobileGL::Int BlockArrayElement(const MobileGL::String& name) {
        if (name.empty() || name.back() != ']') return 0;
        const MobileGL::SizeT bracket = name.rfind('[');
        if (bracket == MobileGL::String::npos) return 0;
        const MobileGL::SizeT first = bracket + 1;
        const MobileGL::SizeT last = name.length() - 1;
        if (first >= last) return 0;
        if (name[first] == '0' && last - first > 1) return 0; // no leading zeros
        MobileGL::Int element = 0;
        for (MobileGL::SizeT i = first; i < last; ++i) {
            if (name[i] < '0' || name[i] > '9') return 0;
            element = element * 10 + static_cast<MobileGL::Int>(name[i] - '0');
            if (element > 0x0FFFFFFF) return 0;
        }
        return element;
    }

    // GL 4.6 core 7.7 / ARB_shader_atomic_counters: within one binding no two atomic counters
    // may occupy the same bytes, every offset is a multiple of 4, and no counter may reach past
    // GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE. glslang enforces all three in fixOffset(), which the
    // Vulkan-relaxed parse never reaches - vkRelaxedRemapUniformVariable folds the atomic_uint
    // into a synthesized storage block and returns from declareVariable() before fixOffset()
    // runs, clearing explicitOffset on the way ("xxTODO: use logic from fixOffset()"). Two
    // counters declared at the same binding AND the same offset therefore linked cleanly.
    //
    // The offsets themselves survive that lowering (reflection and the SPIR-V generator both
    // honour layoutOffset), so the check belongs here, over the same model the GL queries answer
    // from. Returns the info-log line for an illegal layout, empty for a legal one.
    static MobileGL::String ValidateAtomicCounterLayout(glslang::TProgram& reflection) {
        using MobileGL::Bool;
        using MobileGL::Int;
        using MobileGL::SizeT;
        using MobileGL::String;
        using MobileGL::Vector;
        namespace Transpiler = MobileGL::MG_Util::ShaderTranspiler;

        const Int blockCount = reflection.getNumUniformBlocks();
        if (blockCount <= 0) return {};
        const SizeT prefixLength = std::strlen(Transpiler::ATOMIC_COUNTER_BLOCK_PREFIX);
        Vector<Bool> isCounterBlock(static_cast<SizeT>(blockCount), false);
        Bool anyCounterBlock = false;
        for (Int i = 0; i < blockCount; ++i) {
            const auto& block = reflection.getUniformBlock(i);
            isCounterBlock[static_cast<SizeT>(i)] =
                block.name.compare(0, prefixLength, Transpiler::ATOMIC_COUNTER_BLOCK_PREFIX) == 0;
            anyCounterBlock = anyCounterBlock || isCounterBlock[static_cast<SizeT>(i)];
        }
        if (!anyCounterBlock) return {}; // every program that declares no atomic counter

        struct CounterSpan {
            Int offset = 0;
            Int size = 0;
            String name;
        };
        Vector<Vector<CounterSpan>> spansByBlock(static_cast<SizeT>(blockCount));
        const Int uniformCount = reflection.getNumUniformVariables();
        for (Int i = 0; i < uniformCount; ++i) {
            const auto& uniform = reflection.getUniform(i);
            const Int owner = uniform.index;
            if (owner < 0 || owner >= blockCount || !isCounterBlock[static_cast<SizeT>(owner)]) continue;
            const Int offset = uniform.offset;
            if (offset < 0) continue; // no offset recorded; nothing to compare
            Int elements = uniform.size > 1 ? uniform.size : 1;
            if (const glslang::TType* type = uniform.getType(); type != nullptr && type->isArray()) {
                elements = type->isSizedArray() ? type->getCumulativeArraySize() : 1;
            }
            const Int size = elements * static_cast<Int>(sizeof(MobileGL::Uint32));
            if (offset % 4 != 0) {
                return std::format("Atomic counter '{}' is declared at offset {}, which is not a multiple of 4.",
                                   uniform.name, offset);
            }
            if (offset > Transpiler::MAX_ATOMIC_COUNTER_BUFFER_SIZE - size) {
                return std::format("Atomic counter '{}' ends at byte {}, past the {}-byte "
                                   "GL_MAX_ATOMIC_COUNTER_BUFFER_SIZE.",
                                   uniform.name, offset + size, Transpiler::MAX_ATOMIC_COUNTER_BUFFER_SIZE);
            }
            auto& spans = spansByBlock[static_cast<SizeT>(owner)];
            for (const CounterSpan& existing : spans) {
                if (offset < existing.offset + existing.size && existing.offset < offset + size) {
                    return std::format("Atomic counters '{}' and '{}' share a binding and overlap at byte offset {}.",
                                       existing.name, uniform.name, std::max(offset, existing.offset));
                }
            }
            spans.push_back({offset, size, uniform.name});
        }
        return {};
    }

    // GL 4.6 core 7.6: LinkProgram FAILS when a stage's count of active image uniforms exceeds
    // GL_MAX_{VERTEX,TESS_CONTROL,TESS_EVALUATION,GEOMETRY,FRAGMENT,COMPUTE}_IMAGE_UNIFORMS, or
    // when their sum exceeds GL_MAX_COMBINED_IMAGE_UNIFORMS. Nothing enforced it: glslang carries
    // those numbers in TBuiltInResource only so gl_Max*ImageUniforms can expand from them, and
    // its linker never counts uniforms against them - so a program declaring one image uniform
    // more than the limit linked cleanly and then rendered nothing.
    //
    // The limits are the ones glGetIntegerv answers (MG_Impl/GLImpl/Getter/GL_Getter.cpp), the
    // hardcoded tessellation zeros included: a program may not exceed a limit the implementation
    // advertises, whatever the driver underneath would have taken.
    //
    // Counts the APPLICATION's image uniforms. The DirectGLES read/write split emits a second
    // declaration for an image a stage both reads and writes (MG_Backend/DirectGLES/Utils.h), but
    // that happens in the backend after this link, and counting the expanded set here would
    // reject programs that are legal by the numbers GL advertises. Returns the info-log line for
    // a program over a limit, empty for one within them.
    static MobileGL::String ValidateImageUniformLimits(
        glslang::TProgram& reflection, const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env) {
        using MobileGL::Array;
        using MobileGL::Int;
        using MobileGL::SizeT;
        using MobileGL::UnorderedMap;

        static constexpr EShLanguage kStages[] = {EShLangVertex,   EShLangTessControl, EShLangTessEvaluation,
                                                  EShLangGeometry, EShLangFragment,    EShLangCompute};
        static constexpr const char* kLimitNames[] = {
            "GL_MAX_VERTEX_IMAGE_UNIFORMS",   "GL_MAX_TESS_CONTROL_IMAGE_UNIFORMS",
            "GL_MAX_TESS_EVALUATION_IMAGE_UNIFORMS", "GL_MAX_GEOMETRY_IMAGE_UNIFORMS",
            "GL_MAX_FRAGMENT_IMAGE_UNIFORMS", "GL_MAX_COMPUTE_IMAGE_UNIFORMS"};
        constexpr SizeT kStageCount = sizeof(kStages) / sizeof(kStages[0]);
        const Int limits[kStageCount] = {env.params.MaxVertexImageUniforms,
                                         0,
                                         0,
                                         env.params.MaxGeometryImageUniforms,
                                         env.params.MaxFragmentImageUniforms,
                                         env.params.MaxComputeImageUniforms};

        // Reflection spells an image ARRAY one of two ways, and which one it picks depends on how
        // the shader indexed it: a variable index makes glslang expand the array into one entry
        // per element ("u_image[0]".."u_image[8]", each carrying the ELEMENT type), while an
        // array never dereferenced at all stays a single entry carrying the array type. One
        // program can even produce both spellings for the same array. So neither counting entries
        // nor trusting the declared size is right on its own - they are reconciled per declared
        // name with a max, which is exact for either spelling and cannot double-count the mixture.
        struct ImageUse {
            Int entries = 0;  // reflection entries seen for this name in this stage
            Int declared = 0; // largest element count any of them declared
        };
        UnorderedMap<MobileGL::String, Array<ImageUse, kStageCount>> useByName;

        const Int uniformCount = reflection.getNumUniformVariables();
        for (Int i = 0; i < uniformCount; ++i) {
            const auto& uniform = reflection.getUniform(i);
            const glslang::TType* type = uniform.getType();
            if (type == nullptr || !type->isImage()) continue;
            // An array occupies one image unit per element; an unsized one (never indexed, so
            // never more than the single element glslang kept) counts as one.
            Int elements = uniform.size > 1 ? uniform.size : 1;
            if (type->isArray()) {
                elements = type->isSizedArray() ? type->getCumulativeArraySize() : 1;
            }
            // `stages` is the set of stages that REFERENCE the uniform, which is exactly what GL
            // counts: an image declared in two stages costs a unit in each, and one no stage
            // reads is not active at all and costs nothing.
            Array<ImageUse, kStageCount>* use = nullptr;
            for (SizeT stage = 0; stage < kStageCount; ++stage) {
                if ((static_cast<unsigned>(uniform.stages) & (1u << static_cast<unsigned>(kStages[stage]))) == 0) {
                    continue;
                }
                // The one insert this uniform performs, so the reference survives the rest of the
                // stage loop - a flat hash map relocates on insert, never on read.
                if (use == nullptr) {
                    use = &useByName[StripArrayElementSuffix(uniform.name)];
                }
                ++(*use)[stage].entries;
                (*use)[stage].declared = std::max((*use)[stage].declared, elements);
            }
        }

        Int counts[kStageCount] = {};
        for (const auto& entry : useByName) {
            for (SizeT stage = 0; stage < kStageCount; ++stage) {
                counts[stage] += std::max(entry.second[stage].entries, entry.second[stage].declared);
            }
        }

        Int combined = 0;
        for (SizeT stage = 0; stage < kStageCount; ++stage) {
            combined += counts[stage];
            if (counts[stage] > limits[stage]) {
                return std::format("This program uses {} active image uniforms in one stage, more than the {} "
                                   "{} allows.",
                                   counts[stage], limits[stage], kLimitNames[stage]);
            }
        }
        if (combined > env.params.MaxCombinedImageUniforms) {
            return std::format("This program uses {} active image uniforms across its stages, more than the {} "
                               "GL_MAX_COMBINED_IMAGE_UNIFORMS allows.",
                               combined, env.params.MaxCombinedImageUniforms);
        }
        return {};
    }

    static bool IsBuiltInPipelineOutput(const glslang::TObjectReflection& output) {
        const auto* type = output.getType();
        return type && type->getQualifier().builtIn != glslang::EbvNone;
    }

    // Locations one ELEMENT of a vertex input occupies (GL 4.6 core 11.1.1): a matrix
    // takes one per column, everything else this backend can feed takes one.
    static int GetVertexInputLocationSpan(GLenum glType) {
        switch (glType) {
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT2x4:
            return 2;
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT3x2:
        case GL_FLOAT_MAT3x4:
            return 3;
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT4x2:
        case GL_FLOAT_MAT4x3:
            return 4;
        default:
            return 1;
        }
    }

    // How many elements an ARRAY vertex input has. glslang reflects such an input as ONE
    // record spelled "name[0]" carrying the ELEMENT's glDefineType and the array length,
    // so the type alone cannot say how many locations the declaration covers: GL 4.6 core
    // 11.1.1 gives an array one location per element (times the element's own span), and
    // `in vec4 a[16]` at location 0 therefore occupies 0..15, not 0. Missing that left
    // every location above the base with no recorded name or type, which is what the
    // backends read to decide whether an attribute is active at all.
    static MobileGL::Int GetVertexInputArrayElements(const glslang::TObjectReflection& input) {
        const glslang::TType* type = input.getType();
        if (type == nullptr || !type->isArray()) return 1;
        // An unsized input array has no span to compute; treat it as one element rather
        // than guessing, so it can only ever under-claim locations.
        if (!type->isSizedArray()) return 1;
        return std::max(1, type->getCumulativeArraySize());
    }

    static MobileGL::Int GetVertexInputTotalLocationSpan(const glslang::TObjectReflection& input) {
        return GetVertexInputLocationSpan(input.glDefineType) * GetVertexInputArrayElements(input);
    }

    static GLenum GetVertexInputLocationType(GLenum glType) {
        switch (glType) {
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3x2:
        case GL_FLOAT_MAT4x2:
            return GL_FLOAT_VEC2;
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT2x3:
        case GL_FLOAT_MAT4x3:
            return GL_FLOAT_VEC3;
        case GL_FLOAT_MAT4:
        case GL_FLOAT_MAT2x4:
        case GL_FLOAT_MAT3x4:
            return GL_FLOAT_VEC4;
        default:
            return glType;
        }
    }

    // How many consecutive uniform locations a uniform occupies. Array uniforms (opaque
    // or not) span one location per element so glUniform*v(count > 1) and
    // glGetUniformLocation("arr[k]") can address elements individually; everything else
    // spans a single location. TObjectReflection.size only carries the element count for
    // non-block arrays, so prefer the TType, which is authoritative for both.
    static MobileGL::Int GetUniformLocationSpan(const glslang::TObjectReflection& uniform) {
        const glslang::TType* type = uniform.getType();
        if (type != nullptr && type->isSizedArray()) {
            return std::max(1, type->getOuterArraySize());
        }
        return std::max(1, uniform.size);
    }

} // namespace

namespace MobileGL::MG_State::GLState {
    namespace {
        // The artifacts of a compile that ran to completion, or the never-compiled defaults.
        // A node that was abandoned (cancelled at teardown, or whose body threw) published
        // nothing, so it reads exactly like "never compiled" - which is the same collapse
        // ShaderObject's join gate performs, and is what keeps the link's view of a shader
        // identical whether it went through the object or through the snapshot.
        const ShaderCompileArtifacts& CompiledArtifacts(const SharedPtr<const ShaderCompileTask>& node) {
            static const ShaderCompileArtifacts empty;
            return (node && node->IsComplete()) ? node->artifacts : empty;
        }

        // GL type enum for a vertex-stage output symbol captured by transform
        // feedback. Covers the scalar/vector/matrix float+integer types transform
        // feedback may legally capture in GL 3.3.
        Bool ResolveXfbSymbolType(const glslang::TType& type, GLenum& outType, GLint& outArraySize,
                                  Uint32& outBytesPerElement) {
            outArraySize = type.isArray() ? type.getOuterArraySize() : 1;
            const Int columns = type.isMatrix() ? type.getMatrixCols() : 1;
            const Int components = type.isMatrix() ? type.getMatrixRows()
                                                   : (type.isVector() ? type.getVectorSize() : 1);
            const glslang::TBasicType basic = type.getBasicType();
            static constexpr GLenum kFloatTypes[5] = {0, GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4};
            static constexpr GLenum kIntTypes[5] = {0, GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4};
            static constexpr GLenum kUintTypes[5] = {0, GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3,
                                                     GL_UNSIGNED_INT_VEC4};
            static constexpr GLenum kDoubleTypes[5] = {0, GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3,
                                                      GL_DOUBLE_VEC4};
            if (type.isMatrix()) {
                if (basic != glslang::EbtFloat && basic != glslang::EbtDouble) return false;
                static constexpr GLenum kMatTypes[5][5] = {
                    {}, {},
                    {0, 0, GL_FLOAT_MAT2, GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4},
                    {0, 0, GL_FLOAT_MAT3x2, GL_FLOAT_MAT3, GL_FLOAT_MAT3x4},
                    {0, 0, GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4},
                };
                static constexpr GLenum kDoubleMatTypes[5][5] = {
                    {}, {},
                    {0, 0, GL_DOUBLE_MAT2, GL_DOUBLE_MAT2x3, GL_DOUBLE_MAT2x4},
                    {0, 0, GL_DOUBLE_MAT3x2, GL_DOUBLE_MAT3, GL_DOUBLE_MAT3x4},
                    {0, 0, GL_DOUBLE_MAT4x2, GL_DOUBLE_MAT4x3, GL_DOUBLE_MAT4},
                };
                if (columns < 2 || columns > 4 || components < 2 || components > 4) return false;
                outType = basic == glslang::EbtDouble ? kDoubleMatTypes[columns][components]
                                                      : kMatTypes[columns][components];
            } else if (components >= 1 && components <= 4) {
                switch (basic) {
                case glslang::EbtFloat: outType = kFloatTypes[components]; break;
                case glslang::EbtInt: outType = kIntTypes[components]; break;
                case glslang::EbtUint: outType = kUintTypes[components]; break;
                // A double-typed varying is capturable like any other; rejecting it here reported
                // the varying as "not an output of the vertex stage", which it plainly was.
                case glslang::EbtDouble: outType = kDoubleTypes[components]; break;
                default: return false;
                }
            } else {
                return false;
            }
            // GL 4.6 core 11.1.2.1: a double component occupies eight basic machine units, and
            // counts as two components against the transform feedback limits.
            const Uint32 bytesPerComponent = basic == glslang::EbtDouble ? 8u : 4u;
            outBytesPerElement = static_cast<Uint32>(columns * components) * bytesPerComponent;
            return true;
        }

        // Extracts a geometry shader's per-invocation EmitVertex/EndPrimitive sequence
        // when it is statically knowable (no emit inside selection/loop/switch). Vulkan
        // transform feedback captures triangle strips in plain (i, i+1, i+2) order while
        // GL decomposes odd strip triangles as (i+1, i, i+2) (GL 4.6 table 10.1); with
        // the static strip lengths the capture buffer can be reordered after EndTF.
        class GsEmitSequenceTraverser final : public glslang::TIntermTraverser {
        public:
            bool visitAggregate(glslang::TVisit, glslang::TIntermAggregate* node) override {
                if (node->getOp() == glslang::EOpEmitVertex) {
                    ++emitCount;
                    hasEmit = true;
                } else if (node->getOp() == glslang::EOpEndPrimitive) {
                    FlushStrip();
                }
                return true;
            }
            bool visitSelection(glslang::TVisit, glslang::TIntermSelection*) override {
                inControlFlow = true;
                return true;
            }
            bool visitLoop(glslang::TVisit, glslang::TIntermLoop*) override {
                inControlFlow = true;
                return true;
            }
            bool visitSwitch(glslang::TVisit, glslang::TIntermSwitch*) override {
                inControlFlow = true;
                return true;
            }
            void FlushStrip() {
                if (emitCount >= 3) {
                    stripTriangles.push_back(static_cast<Uint32>(emitCount - 2));
                }
                emitCount = 0;
            }

            Vector<Uint32> stripTriangles;
            Uint32 emitCount = 0;
            Bool hasEmit = false;
            Bool inControlFlow = false;
        };
    } // namespace

    void ProgramLinkTask::DeferLog(String line) { diagnostics.logLines.push_back(Move(line)); }

    void ProgramLinkTask::SubmitAfter(const Vector<SharedPtr<ShaderCompileTask>>& deps) {
        // +1 for the guard this function releases itself. Without it, a dependency that
        // settles on a worker between two OnTerminal() calls below could drive the counter to
        // zero and post the job while the remaining edges are still being registered - the
        // job would then run against a dependency that has not finished writing its
        // artifacts. Store before any edge exists, so every decrement sees the final total.
        m_remainingDeps.store(static_cast<Int>(deps.size()) + 1, std::memory_order_release);

        auto self = std::static_pointer_cast<ProgramLinkTask>(shared_from_this());
        for (const auto& dep : deps) {
            // Runs inline, right here, for a dependency that is already terminal (which
            // Link()'s prologue tries not to hand us, but a compile can settle between the
            // IsTerminal() check there and this line).
            dep->OnTerminal([self] { self->OnDepSettled(); });
        }
        OnDepSettled(); // release the guard; posts here iff every dependency already settled
    }

    void ProgramLinkTask::OnDepSettled() {
        // fetch_sub returning 1 means this call took the counter to zero, so exactly one
        // caller ever posts. acq_rel so the posting thread sees every dependency's artifacts,
        // which were published by their own terminal transitions.
        if (m_remainingDeps.fetch_sub(1, std::memory_order_acq_rel) != 1) return;

        // Non-throwing by construction, and it has to be: this is a JobNode continuation, so
        // on the pool side it runs inside an Asio handler. Post() contains its own allocation
        // failures (it cancels the node rather than propagating), and shared_from_this() can
        // only throw for a node that was never owned by a SharedPtr - which SubmitAfter's
        // contract forbids. The catch is the backstop for both, and it CANCELS rather than
        // swallowing: a link that is never posted is a GL thread blocked forever in
        // EnsureLinkJoined(), which is a far worse failure than a link reported as not linked.
        try {
            MG_Util::Async::ShaderCompilePool::Get().Post(shared_from_this());
        } catch (...) {
            Cancel();
        }
    }

    // Pure CPU work only, on a pool worker. Everything this reads is an input the node owns;
    // everything it writes is `artifacts` (and diagnostics). Do not add a GL/EGL call, a
    // pActiveBackendObject read, or a pGLContext->RecordError() here - the first two are what
    // CompileEnv exists to replace, and the third is why the deferred-diagnostics mechanism
    // (and JobNode's debug assert on it) exists.
    //
    // This is the whole link. See the one-link-one-handler note in the class comment.
    void ProgramLinkTask::RunBody() {
        // glslang leaves this worker's TLS pool allocator pointing at the last arena it
        // touched (a re-parse's TShader, or the TProgram's); reset it on the way out so an
        // unrelated later job cannot allocate out of a pool the GL thread has since freed.
        const GlslangThreadAllocatorGuard glslangGuard;
        using namespace MG_Util::ShaderTranspiler;

        MOBILEGL_ASSERT(in.env != nullptr, "ProgramLinkTask: the CompileEnv snapshot is missing");
        const CompileEnv& env = *in.env;

        MGLOG_D("ProgramObject %u: Link body start, shaders to link: %zu", in.externalIndex, in.shaders.size());

        Vector<SharedPtr<glslang::TShader>> shaders;
        if (!ConsumeShaders(shaders)) return;

        // Harvest the declared default-block uniform initializers before the TShaders are
        // handed to the linker. They come from the parse itself (glslang folds the constant
        // and hands it over instead of dropping it), not from a lexical scan, so an
        // expression like vec3(10, 20, 30) or int[](1, 2, 3) is already evaluated.
        //
        // Stage order decides a tie. GLSL requires a uniform declared in several stages to be
        // declared identically, initializer included, so a conflict is a malformed program;
        // taking the first stage's value keeps a link that other implementations accept from
        // failing here, and both stages agree in every well-formed one.
        for (const auto& shader : shaders) {
            const glslang::TIntermediate* intermediate = shader ? shader->getIntermediate() : nullptr;
            if (intermediate == nullptr) continue;
            for (const auto& initializer : intermediate->getUniformInitializers()) {
                const auto known = std::find_if(artifacts.uniformInitialValues.begin(),
                                                artifacts.uniformInitialValues.end(),
                                                [&initializer](const auto& existing) {
                                                    return existing.name == initializer.name;
                                                });
                if (known != artifacts.uniformInitialValues.end()) continue;
                artifacts.uniformInitialValues.push_back(initializer);
            }
        }

        // Merge the shaders' lexically extracted explicit uniform locations. The same
        // uniform declared in several stages must agree on its location (config-A glslang
        // enforced this at mapIO; the relaxed parse no longer sees the qualifiers).
        for (const auto& shader : in.shaders) {
            const ShaderCompileArtifacts& compiled = CompiledArtifacts(shader.compiled);
            for (const auto& [name, location] : compiled.explicitUniformLocations) {
                const auto [it, inserted] = artifacts.linkedExplicitUniformLocations.emplace(name, location);
                if (!inserted && it->second != location) {
                    artifacts.infoLog = std::format(
                        "Uniform '{}' is declared with conflicting explicit locations ({} and {}) "
                        "across stages.",
                        name, it->second, location);
                    DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
                    return;
                }
            }
            // Sampler/image layout(binding = N) initial units, likewise invisible to the
            // relaxed parse. Stage order matches the old per-stage mapIO capture, so a
            // name declared in several stages keeps the last stage's binding as before.
            for (const auto& [name, binding] : compiled.explicitOpaqueBindings) {
                artifacts.explicitOpaqueUniformBindings[name] = binding;
            }
        }

        ProgramAttrib attrib{.shaders = Move(shaders),
                             .explicitVertexInLocations = in.explicitAttribLocations,
                             .explicitFragmentOutLocations = in.explicitFragDataLocation,
                             .explicitFragmentOutIndices = in.explicitFragDataIndex,
                             .explicitOpaqueUniformBindings = &artifacts.explicitOpaqueUniformBindings};

        MGLOG_D("ProgramObject %u: Calling ShaderCompiler::LinkProgram", in.externalIndex);
        auto result = ShaderCompiler::LinkProgram(attrib);
        if (result) {
            artifacts.linkStatus = true;
            artifacts.program = result.value();
            artifacts.linkedFragDataLocation = in.explicitFragDataLocation;
            artifacts.linkedFragDataIndex = in.explicitFragDataIndex;
            MGLOG_D("ProgramObject %u: LinkProgram succeeded, TProgram ptr %p", in.externalIndex,
                    artifacts.program.get());
        } else {
            artifacts.infoLog = result.error().log;
            DeferLog(std::format("ProgramObject {}: LinkProgram failed. InfoLog:\n{}", in.externalIndex,
                                 artifacts.infoLog));
            return;
        }

        // A compute program must have a fixed local group size, and GL states that as a
        // property of the PROGRAM: "at least one" of its compute shaders declares it (GL 4.6
        // core 7.13 / GLSL 4.30 4.4.1.4). MobileGL used to answer that question per SHADER,
        // by scanning each source for the text "local_size_" - which rejected the perfectly
        // legal shape KHR-GL42.compute_shader.build-monolithic submits, three compilation
        // units of which only two carry the layout and the third holds nothing but a buffer
        // block and a function. It also could not see a local size that arrived through a
        // macro, and it happily accepted the substring inside an unrelated identifier.
        //
        // glslang already merged the units' modes at link (linkValidate.cpp mergeModes, which
        // also diagnoses two units declaring CONTRADICTORY sizes), so the linked
        // intermediate is the thing that knows - and asking it is both correct and free.
        if (const glslang::TIntermediate* cs = artifacts.program->getIntermediate(EShLangCompute);
            cs != nullptr && !cs->isLocalSizeSet()) {
            artifacts.linkStatus = false;
            // The gate this replaced ran before LinkProgram, so a program that failed it
            // published no TProgram at all. Keep that invariant: everything downstream reads
            // artifacts.program as "the linked program", and a rejected link should not leave
            // one behind for a query surface to find.
            artifacts.program.reset();
            artifacts.infoLog = "Compute shader is missing a local_size layout declaration.";
            DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
            return;
        }

        // GL_GEOMETRY_INPUT_TYPE. A draw's primitive type has to be compatible with it
        // (GL 4.6 core 11.3.1), so it is resolved for every link, not only a capturing one.
        artifacts.gsInputPrimitive = GL_NONE;
        if (const glslang::TIntermediate* gs = artifacts.program->getIntermediate(EShLangGeometry)) {
            switch (gs->getInputPrimitive()) {
            case glslang::ElgPoints: artifacts.gsInputPrimitive = GL_POINTS; break;
            case glslang::ElgLines: artifacts.gsInputPrimitive = GL_LINES; break;
            case glslang::ElgLinesAdjacency: artifacts.gsInputPrimitive = GL_LINES_ADJACENCY; break;
            case glslang::ElgTriangles: artifacts.gsInputPrimitive = GL_TRIANGLES; break;
            case glslang::ElgTrianglesAdjacency: artifacts.gsInputPrimitive = GL_TRIANGLES_ADJACENCY; break;
            default: break;
            }
        }

        // ---- everything below this line up to GenerateSpirv() is the GL query surface ----
        //
        // ORDERING NOTE (rewritten 2026-08-10; the constraint it records was RETESTED, not
        // dropped on a hunch). This block used to insist that SPIR-V be generated BEFORE
        // buildReflection touches artifacts.program, on the grounds that reflection's
        // live-variable analysis mutates the shared intermediates in ways that change
        // subsequent GlslangToSpv output - "observed: catastrophic uniform misbinding on
        // DirectVulkan for UBO-heavy content", recorded with commit 0d052719.
        //
        // Re-measured on the glslang pin this tree vendors, with the same method 0d052719
        // used (per-module SPIR-V hashes, both orders, byte-compared): 636 modules across
        // 320 programs - the whole extracted trace corpus (BSL, Complementary Reimagined,
        // IterationRP, Create/Flywheel) plus adversarial synthetics - came out BYTE-IDENTICAL
        // in both orders, pre-optimize and post-optimize alike. glslang's code structure
        // agrees: reflection.cpp performs no AST write (no getWritableType, no const_cast, no
        // qualifier assignment) and GlslangToSpv takes a const TIntermediate&.
        //
        // Confirmed a third time ON DEVICE, 2026-08-11, and this one closes the gap the
        // desktop A/B could not: the corpus replays captured SOURCES, so it never reproduced
        // Iris's glBindAttribLocation-before-link flow, which is what drives the io-resolver
        // that assigns vertex-input Locations. A Complementary Reimagined pack load on an
        // Adreno 830 was dumped at the pipeline the driver rejects (programHash
        // 0x4a7e9a37fb49caa1) under BOTH orders and under the pre-split build 6ea94877: all
        // three dumps are the same bytes (md5 39ffa10d5186a4d37be82d0b42297a8d). The order
        // does not perturb SPIR-V on this pin, including on the exact flow 0d052719 feared.
        //
        // Not a licence to stop measuring: 0d052719's observation was real once, and the
        // method (per-module hashes, both orders) is cheap. Re-run it on any glslang bump.
        //
        // So the order is now the other way round, and deliberately: reflection, fragment
        // output validation and transform-feedback resolution are what the GL query surface
        // is made of, and they are also the only remaining ways a link can FAIL, so running
        // them first is what lets LINK_STATUS and every query behind it become final without
        // waiting for SPIR-V (and stops a program that fails validation from paying for
        // ~68 s/pack-load of SPIR-V generation it is about to throw away).
        //
        // What has NOT changed: the routing tables are sized and keyed by reflection results
        // AND read the OPTIMIZED SPIR-V, so BuildGlobalUboRouting still runs strictly after
        // both DoReflection and GenerateSpirv.
        MGLOG_D("ProgramObject %u: Starting reflection", in.externalIndex);
        if (!DoReflection(env)) {
            DeferLog(std::format("ProgramObject {}: Link failed during reflection: {}", in.externalIndex,
                                 artifacts.infoLog));
            return;
        }
        MGLOG_D("ProgramObject %u: Reflection done (linkStatus=%d)", in.externalIndex, (int)artifacts.linkStatus);

        if (!ValidateFragmentOutputLocations()) {
            return;
        }
        if (!ResolveTransformFeedbackVaryings()) {
            artifacts.linkStatus = false;
            DeferLog(std::format("ProgramObject {}: transform feedback varying resolution failed: {}",
                                 in.externalIndex, artifacts.infoLog));
            return;
        }

        // ---- past this point the link cannot fail any more ----
        // Everything left is SPIR-V work, and it belongs to phase B. Hand it what it needs
        // and stop: from the join's point of view this program is now fully linked.
        //
        // The TShaders move rather than copy - `attrib` borrowed them into the TProgram as
        // raw pointers and this node is now their owner of record, for as long as phase B
        // (which holds this node) needs the intermediates hanging off them.
        spirvHandoff.shaders = Move(attrib.shaders);
        spirvHandoff.shaderTypes.resize(in.shaders.size());
        for (SizeT i = 0; i < in.shaders.size(); i++) {
            spirvHandoff.shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(in.shaders[i].stage);
        }
        // Copied, not referenced: `artifacts` is MOVED out of this node by the join, and
        // phase B runs after that. Measured at ~20 us per program, which is noise against the
        // ~450 ms phase B spends on the same program.
        spirvHandoff.reflection.program = artifacts.program;
        spirvHandoff.reflection.uniformLocations = artifacts.uniformLocations;
        spirvHandoff.reflection.uniformIndexInTProgram = artifacts.uniformIndexInTProgram;
        spirvHandoff.reflection.tProgramUniformIndexToGl = artifacts.tProgramUniformIndexToGl;
        spirvHandoff.reflection.maxUniformLocation = artifacts.maxUniformLocation;
        spirvHandoff.ready = true;
        MGLOG_D("ProgramObject %u: phase A done, %zu module(s) handed to the SPIR-V job", in.externalIndex,
                spirvHandoff.shaderTypes.size());
    }

    Bool ProgramLinkTask::ConsumeShaders(Vector<SharedPtr<glslang::TShader>>& outShaders) {
        outShaders.assign(in.shaders.size(), nullptr);

        // GL 4.6 core 7.3: a compute shader may only be linked with other compute shaders -
        // the compute pipeline has no other stages to link against, so a program that mixes
        // them must fail to link (KHR-GL43.compute_shader.api-program).
        {
            Bool hasCompute = false;
            Bool hasNonCompute = false;
            for (const LinkShaderInput& input : in.shaders) {
                (input.stage == ShaderStage::Compute ? hasCompute : hasNonCompute) = true;
            }
            if (hasCompute && hasNonCompute) {
                artifacts.infoLog =
                    "A compute shader cannot be linked with shaders of any other stage.";
                DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
                return false;
            }
        }

        for (SizeT i = 0; i < in.shaders.size(); i++) {
            const LinkShaderInput& input = in.shaders[i];
            const GLenum shaderType = MG_Util::ConvertShaderStageToGLEnum(input.stage);
            const ShaderCompileArtifacts& compiled = CompiledArtifacts(input.compiled);
            MGLOG_D("ProgramObject %u: Preparing shader[%zu] stage %s", in.externalIndex, i,
                    MG_Util::ConvertGLEnumToString(shaderType).c_str());

            if (!compiled.compileStatus) {
                // The compile log LEADS the quoted source, and that order is load-bearing:
                // under MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS this string is the
                // application's ONLY compile diagnostic (the per-shader queries answered
                // optimistically), and applications read it through a bounded buffer -
                // Iris uses 32768 bytes - so the actionable text must come before the
                // potentially-100KB source dump. The full source stays: the device log is
                // where a failing pack gets debugged from.
                artifacts.infoLog =
                    std::format("Linking a {} with compilation error, linking will now terminate. Shader error "
                                "log:\n{}\nShader src:\n{}",
                                MG_Util::ConvertGLEnumToString(shaderType), compiled.infoLog,
                                input.source ? *input.source : String());
                DeferLog(std::format("ProgramObject {}: Link failed - shader[{}] compile status false. InfoLog:\n{}",
                                     in.externalIndex, i, artifacts.infoLog));
                return false;
            }
            String reparseLog;
            outShaders[i] = input.compiled->ClaimParsedShader(reparseLog);
            if (!outShaders[i]) {
                // Only reachable when the consume-once re-parse of an already-compiled
                // source fails, which no valid state transition produces.
                artifacts.infoLog = std::format("Internal error: re-parsing an attached {} for linking failed:\n{}",
                                                MG_Util::ConvertGLEnumToString(shaderType), reparseLog);
                DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
                return false;
            }
            // Deliberately no full-source dump here: a shaderpack stage runs to ~100 KB, and
            // one MGLOG line per shader per link is unreadable even single-threaded. Use the
            // transpiler dump paths when a specific source is actually needed.
            MGLOG_D("ProgramObject %u: shader[%zu] compiled shader ptr %p, src len %zu", in.externalIndex, i,
                    outShaders[i].get(), input.source ? input.source->length() : 0u);
        }
        return true;
    }

    Bool ProgramLinkTask::DoReflection(const MG_Util::ShaderTranspiler::CompileEnv& env) {
        if (!artifacts.program) {
            DeferLog(std::format("ProgramObject {}: DoReflection called but the linked program is null",
                                 in.externalIndex));
            artifacts.linkStatus = false;
            artifacts.infoLog = "DoReflection failed: no program.";
            return false;
        }

        MGLOG_D("ProgramObject %u: DoReflection - building reflection", in.externalIndex);
        // GL-style reflection naming (GL CTS uniform_block relies on all four):
        //  - BasicArraySuffix: an array uniform is reported as "arr[0]" per the GL spec.
        //  - StrictArraySuffix: named-block struct arrays expand per element ("s[0].a",
        //    "s[1].a", ...) following ARB_program_interface_query rules. Default-block
        //    (loose) uniforms already expand per element without this option.
        //  - AllBlockVariables: every member of an active named block is active even when
        //    no shader statement reads it (ES 3.0/GL 3.3 named-block semantics).
        //  - SharedStd140UBO: a DECLARED uniform block is active even when no member is
        //    ever read (reflected from the linker objects). PreprocessShaderSource coerces
        //    every block to std140, so this covers all of them.
        //  - IntermediateIO: GL_PROGRAM_INPUT is the input interface of the program's FIRST
        //    stage and GL_PROGRAM_OUTPUT the output interface of its LAST one. Without this
        //    glslang hardcodes those boundaries to vertex/fragment, so a separable program
        //    made of one non-vertex stage has an empty input interface and one made of a
        //    non-fragment stage an empty output interface
        //    (KHR-GL43.program_interface_query.separate-programs-*).
        //  - UnwrapIOBlocks: an inter-stage interface block enumerates as its MEMBERS -
        //    "Color.r", and "gl_Position" for an anonymous gl_PerVertex - not as the block
        //    instance. Only reachable through IntermediateIO: a vertex stage's inputs and a
        //    fragment stage's outputs can never be blocks, so this is inert for a program
        //    whose boundary stages are the hardcoded ones.
        if (!artifacts.program->buildReflection(EShReflectionStrictArraySuffix | EShReflectionBasicArraySuffix |
                                                EShReflectionAllBlockVariables | EShReflectionSharedStd140UBO |
                                                EShReflectionIntermediateIO | EShReflectionUnwrapIOBlocks)) {
            artifacts.linkStatus = false;
            artifacts.infoLog = "Build reflection failed.";
            DeferLog(std::format("ProgramObject {}: DoReflection - buildReflection() returned false",
                                 in.externalIndex));
            return false;
        }

        if (String atomicCounterError = ValidateAtomicCounterLayout(*artifacts.program);
            !atomicCounterError.empty()) {
            artifacts.infoLog = Move(atomicCounterError);
            DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
            ProgramObject::ResetLinkArtifacts(artifacts);
            return false;
        }

        if (String imageUniformError = ValidateImageUniformLimits(*artifacts.program, env);
            !imageUniformError.empty()) {
            artifacts.infoLog = Move(imageUniformError);
            DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
            ProgramObject::ResetLinkArtifacts(artifacts);
            return false;
        }

        // ---------- GL-facing index spaces (relaxed-parse cleanup) ----------
        // Blocks first: global-UBO membership drives the uniform filter below. The
        // synthesized MGL_GLOBAL_UBO is a transpiler artifact - its members are GL
        // default-block uniforms and the block itself must stay invisible to GL (it
        // did not exist in the GL-client parse this replaces).
        const Int tProgramBlockCount = artifacts.program->getNumUniformBlocks();
        artifacts.tProgramBlockIndexToGl.assign(tProgramBlockCount, -1);
        artifacts.glBlockIndexToTProgram.clear();
        for (Int i = 0; i < tProgramBlockCount; i++) {
            const auto& ubo = artifacts.program->getUniformBlock(i);
            if (std::strstr(ubo.name.c_str(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != nullptr) {
                continue;
            }
            artifacts.tProgramBlockIndexToGl[i] = static_cast<Int>(artifacts.glBlockIndexToTProgram.size());
            artifacts.glBlockIndexToTProgram.push_back(i);
        }

        // ------------ Uniforms (GL Plain) ----------------
        // The relaxed parse sweeps every DECLARED default-block uniform into
        // MGL_GLOBAL_UBO whether or not any stage reads it. GL requires a
        // declared-but-unreferenced default-block uniform to be inactive (absent from
        // glGetActiveUniform, glGetUniformLocation == -1): filter global-UBO members no
        // stage references. Named-block members keep GL's every-declared-member-is-active
        // semantics, exactly as before.
        const Int tProgramUniformCount = artifacts.program->getNumUniformVariables();
        artifacts.tProgramUniformIndexToGl.assign(tProgramUniformCount, -1);
        artifacts.glUniformIndexToTProgram.clear();
        const auto isGlobalUboMember = [this](const glslang::TObjectReflection& uniform) {
            return uniform.index >= 0 && uniform.index < static_cast<Int>(artifacts.tProgramBlockIndexToGl.size()) &&
                   artifacts.tProgramBlockIndexToGl[uniform.index] < 0;
        };
        for (Int i = 0; i < tProgramUniformCount; i++) {
            const auto& uniform = artifacts.program->getUniform(i);
            if (isGlobalUboMember(uniform) && uniform.stages == 0) {
                MGLOG_D("ProgramObject %u: Reflection - dead default-block uniform '%s' filtered from the GL "
                        "surface",
                        in.externalIndex, uniform.name.c_str());
                continue;
            }
            artifacts.tProgramUniformIndexToGl[i] = static_cast<Int>(artifacts.glUniformIndexToTProgram.size());
            artifacts.glUniformIndexToTProgram.push_back(i);
        }
        artifacts.activeUniformCount = static_cast<Uint>(artifacts.glUniformIndexToTProgram.size());
        MGLOG_D("ProgramObject %u: Reflection - active uniform count = %d (of %d reflected)", in.externalIndex,
                artifacts.activeUniformCount, tProgramUniformCount);

        // Effective explicit location per TProgram uniform, from two sources:
        //  - the lexical side-channel for default-block uniforms - the relaxed parse
        //    dropped their layout(location = N) qualifiers when collecting them into
        //    MGL_GLOBAL_UBO, so reflection cannot provide them ("source-explicit");
        //  - glslang's layoutLocation() for opaque uniforms, where the qualifier
        //    survives the relaxed parse (and mapIO auto-assigns the rest).
        //
        // "no effective location yet". Deliberately OUTSIDE the location space rather than
        // glslang::TQualifier::layoutLocationEnd, which is the first location past the pool and
        // therefore only one off a legal one - a sentinel that sits at the boundary it guards has
        // to be re-proved safe every time the ceiling moves, and glslang uses that same value for
        // "this opaque uniform has no location" as well.
        constexpr Uint kNoLocation = ~static_cast<Uint>(0);
        // The ceiling glGetIntegerv(GL_MAX_UNIFORM_LOCATIONS) advertises, which is what the
        // allocator below has to honour: locations 0..kMaxUniformLocations-1 and no others.
        constexpr Uint kMaxUniformLocations = static_cast<Uint>(ProgramObject::MAX_UNIFORM_LOCATIONS);
        Vector<Uint> effectiveLocation(tProgramUniformCount, kNoLocation);
        Vector<Bool> locationIsSourceExplicit(tProgramUniformCount, false);
        UnorderedMap<String, Uint> structExplicitCursor; // declared root -> next member location
        const auto findExplicitLocation = [this](const String& reflectedName) -> const Int* {
            auto it = artifacts.linkedExplicitUniformLocations.find(reflectedName);
            if (it == artifacts.linkedExplicitUniformLocations.end() && reflectedName.length() > 3 &&
                reflectedName.compare(reflectedName.length() - 3, 3, "[0]") == 0) {
                it = artifacts.linkedExplicitUniformLocations.find(
                    reflectedName.substr(0, reflectedName.length() - 3));
            }
            return it != artifacts.linkedExplicitUniformLocations.end() ? &it->second : nullptr;
        };
        for (const Int i : artifacts.glUniformIndexToTProgram) {
            const auto& uniform = artifacts.program->getUniform(i);
            const glslang::TType* type = uniform.getType();
            const Bool inNamedBlock = uniform.index >= 0 && !isGlobalUboMember(uniform);
            if (inNamedBlock) continue; // block members never take glUniform locations

            if (const Int* explicitLocation = findExplicitLocation(uniform.name)) {
                effectiveLocation[i] = static_cast<Uint>(*explicitLocation);
                locationIsSourceExplicit[i] = true;
            } else if (!artifacts.linkedExplicitUniformLocations.empty() &&
                       uniform.name.find('.') != String::npos) {
                // A struct uniform's explicit location spreads consecutively over its
                // flattened members ("s.a", "s[1].b", ...) in reflection order.
                const SizeT cut = uniform.name.find_first_of(".[");
                const auto rootIt = artifacts.linkedExplicitUniformLocations.find(uniform.name.substr(0, cut));
                if (rootIt != artifacts.linkedExplicitUniformLocations.end()) {
                    auto [cursor, inserted] =
                        structExplicitCursor.emplace(rootIt->first, static_cast<Uint>(rootIt->second));
                    (void)inserted;
                    effectiveLocation[i] = cursor->second;
                    locationIsSourceExplicit[i] = true;
                    cursor->second += static_cast<Uint>(GetUniformLocationSpan(uniform));
                }
            }
            // glslang parks "no location" at layoutLocationEnd, which is a real location in this
            // table's numbering - test for it explicitly rather than letting it through as one.
            if (effectiveLocation[i] == kNoLocation && type != nullptr && type->isOpaque() &&
                uniform.layoutLocation() != glslang::TQualifier::layoutLocationEnd) {
                effectiveLocation[i] = uniform.layoutLocation();
            }
            if (locationIsSourceExplicit[i] &&
                effectiveLocation[i] + static_cast<Uint>(GetUniformLocationSpan(uniform)) > kMaxUniformLocations) {
                // Config A rejected out-of-range explicit locations at parse; keep them
                // from growing the location table unboundedly. Stated against the advertised
                // GL_MAX_UNIFORM_LOCATIONS, because that is the rule being enforced (GL 4.6 core
                // 7.6.1): an array whose LAST element passes the ceiling is a link error even
                // though its base compiled fine.
                artifacts.infoLog = std::format("Uniform '{}' explicit location {} is out of range.", uniform.name,
                                                effectiveLocation[i]);
                ProgramObject::ResetLinkArtifacts(artifacts);
                return false;
            }
        }

        // ARB_explicit_uniform_location / GL 4.6 core 7.6.1: an explicit location is RESERVED
        // whether or not the uniform turned out to be active. The dead default-block uniforms
        // filtered out of glUniformIndexToTProgram above are invisible to every GL query - which
        // is correct - but their locations must still be kept out of the implicit allocator's
        // reach, or an implicit uniform is handed a location the source already claimed.
        //
        // Deliberately NOT written into artifacts.uniformLocations or uniformIndexInTProgram:
        // glGetUniformLocation must keep answering -1 for a dead uniform, and a location no
        // application can legally obtain must not become writable through glUniform*. The
        // occupancy therefore lives in its own bitset, built once the table has been sized.
        Vector<Pair<Uint, Int>> deadExplicitReservations;
        Int deadReservedLocationCount = 0;
        for (Int i = 0; i < tProgramUniformCount; i++) {
            if (artifacts.tProgramUniformIndexToGl[i] >= 0) continue; // GL-visible: handled above
            const auto& uniform = artifacts.program->getUniform(i);
            if (!isGlobalUboMember(uniform) || uniform.stages != 0) continue;
            const Int* explicitLocation = findExplicitLocation(uniform.name);
            if (explicitLocation == nullptr) continue;

            const Uint location = static_cast<Uint>(*explicitLocation);
            const Int locationSpan = GetUniformLocationSpan(uniform);
            if (location + static_cast<Uint>(locationSpan) > kMaxUniformLocations) {
                artifacts.infoLog = std::format("Uniform '{}' explicit location {} is out of range.", uniform.name,
                                                location);
                ProgramObject::ResetLinkArtifacts(artifacts);
                return false;
            }
            deadExplicitReservations.emplace_back(location, locationSpan);
            deadReservedLocationCount += locationSpan;
            artifacts.maxUniformLocation = std::max(artifacts.maxUniformLocation, location + locationSpan - 1);
            MGLOG_D("ProgramObject %u: Reflection - inactive uniform '%s' reserves locations %u..%u without "
                    "becoming GL-visible",
                    in.externalIndex, uniform.name.c_str(), location, location + locationSpan - 1);
        }

        Int requiredUniformLocations = deadReservedLocationCount;
        // The same count restricted to DEFAULT-BLOCK uniforms, which is the only thing
        // GL_MAX_UNIFORM_LOCATIONS bounds. requiredUniformLocations cannot serve: it also carries
        // named-block members, which take a slot in this allocator's table (an implementation
        // detail) but consume no GL uniform location at all, so a big UBO array would otherwise
        // fail a link the spec allows.
        Int defaultBlockLocationDemand = deadReservedLocationCount;
        for (const Int i : artifacts.glUniformIndexToTProgram) {
            auto& uniform = artifacts.program->getUniform(i);
            const Uint location = effectiveLocation[i];
            const Int locationSpan = GetUniformLocationSpan(uniform);
            requiredUniformLocations += locationSpan;
            const Bool inNamedBlock = uniform.index >= 0 && !isGlobalUboMember(uniform);
            if (!inNamedBlock) defaultBlockLocationDemand += locationSpan;
            if (location != kNoLocation) {
                artifacts.maxUniformLocation = std::max(artifacts.maxUniformLocation, location + locationSpan - 1);
            }
            artifacts.uniformNameMaxLength = std::max(artifacts.uniformNameMaxLength, (Int)uniform.name.length());
            artifacts.uniformLocations[uniform.name] = location;
            MGLOG_D("ProgramObject %u: Reflection - uniform[%d] name='%s' effectiveLocation=%d", in.externalIndex,
                    i, uniform.name.c_str(), location);
        }

        MGLOG_D("ProgramObject %u: Reflection - computed maxUniformLocation=%u uniformNameMaxLength=%d",
                in.externalIndex, artifacts.maxUniformLocation, artifacts.uniformNameMaxLength);

        // GL 4.6 core 7.6.1: explicit, implicit and reserved-but-inactive default-block uniforms
        // all draw from the one GL_MAX_UNIFORM_LOCATIONS pool, and a program asking for more than
        // the implementation advertises FAILS TO LINK
        // (KHR-GL43.explicit_uniform_location.uniform-loc-negative-link-max-num-of-locations).
        // A single uniform whose own span passes the ceiling was already rejected above; this is
        // the aggregate half of the same rule.
        if (defaultBlockLocationDemand > static_cast<Int>(kMaxUniformLocations)) {
            artifacts.infoLog =
                std::format("Uniform locations exhausted: the default-block uniforms need {} locations but "
                            "GL_MAX_UNIFORM_LOCATIONS is {}.",
                            defaultBlockLocationDemand, kMaxUniformLocations);
            DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
            ProgramObject::ResetLinkArtifacts(artifacts);
            return false;
        }

        if (artifacts.maxUniformLocation + 1 < requiredUniformLocations) {
            MGLOG_D("ProgramObject %u: Reflection - maxUniformLocation+1 (%u) < requiredUniformLocations (%d), "
                    "adjusting",
                    in.externalIndex, artifacts.maxUniformLocation + 1, requiredUniformLocations);
            // This means we have fewer than enough gaps to fit
            // unallocated uniforms
            artifacts.maxUniformLocation = requiredUniformLocations - 1;
        }

        // i-th elements refers to uniform at layout(location = i, ...)
        artifacts.uniformIndexInTProgram.resize(artifacts.maxUniformLocation + 1,
                                                glslang::TQualifier::layoutLocationEnd);
        artifacts.uniformSamplerOrImageUnitIndex.resize(artifacts.maxUniformLocation + 1, -1);

        // Occupancy for the inactive explicit uniforms collected above: a set bit means "the
        // source claimed this location", which is enough to keep the two implicit passes off it
        // without making the location reachable through any GL entry point. A location the
        // fallback grow path mints later is past this bitset by construction (every reservation
        // was folded into maxUniformLocation before the table was sized), so the lookup treats
        // out-of-range as free rather than resizing in lockstep.
        // Left empty - and unallocated - when nothing reserved anything, which is every program in
        // the shader-pack corpus; the lookup below reads an empty bitset as "nothing is reserved".
        Vector<Bool> reservedLocation;
        if (!deadExplicitReservations.empty()) {
            reservedLocation.assign(artifacts.maxUniformLocation + 1, false);
            for (const auto& [reservedBase, reservedSpan] : deadExplicitReservations) {
                for (Int element = 0; element < reservedSpan; ++element) {
                    reservedLocation[reservedBase + element] = true;
                }
            }
        }
        const auto locationIsReserved = [&reservedLocation](SizeT location) {
            return location < reservedLocation.size() && reservedLocation[location];
        };

        Vector<int> unallocatedUniformIndex;

        // Pass 1: source-explicit locations. These are API contract
        // (ARB_explicit_uniform_location), and an overlap between distinct uniforms is a
        // link error - config A's mapIO rejected it ("Uniform location overlaps across
        // stages"); the relaxed parse dropped the qualifiers, so it is enforced here.
        for (const Int i : artifacts.glUniformIndexToTProgram) {
            auto& uniform = artifacts.program->getUniform(i);
            if (!locationIsSourceExplicit[i] || effectiveLocation[i] == kNoLocation) continue;
            const Uint location = effectiveLocation[i];
            const Int locationSpan = GetUniformLocationSpan(uniform);
            for (Int element = 0; element < locationSpan; ++element) {
                const Int existing = artifacts.uniformIndexInTProgram[location + element];
                if (existing != glslang::TQualifier::layoutLocationEnd && existing != i) {
                    artifacts.infoLog =
                        std::format("Uniform location overlap: '{}' and '{}' both occupy location {}.",
                                    artifacts.program->getUniform(existing).name, uniform.name, location + element);
                    ProgramObject::ResetLinkArtifacts(artifacts);
                    return false;
                }
                artifacts.uniformIndexInTProgram[location + element] = i;
            }
            MGLOG_D("ProgramObject %u: Reflection - assigned explicit-location uniform '%s' to locations "
                    "%u..%u (indexInTProgram=%d)",
                    in.externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, i);
        }

        // Pass 2: glslang-assigned locations (opaque uniforms under the relaxed parse).
        // Implementation-chosen, so on a collision with an explicit location the uniform
        // is demoted to the first-fit pass below instead of failing the link.
        for (const Int i : artifacts.glUniformIndexToTProgram) {
            auto& uniform = artifacts.program->getUniform(i);
            if (locationIsSourceExplicit[i]) continue;
            const Uint location = effectiveLocation[i];
            if (location == kNoLocation) {
                unallocatedUniformIndex.emplace_back(i);
                MGLOG_D("ProgramObject %u: Reflection - uniform '%s' is unallocated, will assign later",
                        in.externalIndex, uniform.name.c_str());
                continue; // will allocate unallocated uniforms later
            }
            const Int locationSpan = GetUniformLocationSpan(uniform);
            Bool spanIsFree = location + locationSpan - 1 <= artifacts.maxUniformLocation;
            for (Int element = 0; spanIsFree && element < locationSpan; ++element) {
                spanIsFree =
                    artifacts.uniformIndexInTProgram[location + element] == glslang::TQualifier::layoutLocationEnd &&
                    !locationIsReserved(location + element);
            }
            if (!spanIsFree) {
                artifacts.uniformLocations[uniform.name] = kNoLocation;
                unallocatedUniformIndex.emplace_back(i);
                MGLOG_D("ProgramObject %u: Reflection - uniform '%s' auto location %u collides with an "
                        "explicit location, demoting to first-fit",
                        in.externalIndex, uniform.name.c_str(), location);
                continue;
            }
            for (Int element = 0; element < locationSpan; ++element) {
                artifacts.uniformIndexInTProgram[location + element] = i;
            }
            MGLOG_D("ProgramObject %u: Reflection - assigned uniform '%s' to locations %u..%u "
                    "(indexInTProgram=%d)",
                    in.externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, i);
        }

        SizeT locNeedle = 0;
        std::sort(unallocatedUniformIndex.begin(), unallocatedUniformIndex.end(), [this](Int lhs, Int rhs) {
            const auto& lhsUniform = artifacts.program->getUniform(lhs);
            const auto& rhsUniform = artifacts.program->getUniform(rhs);
            return lhsUniform.name < rhsUniform.name;
        });
        for (auto index : unallocatedUniformIndex) {
            auto& uniform = artifacts.program->getUniform(index);
            const Int locationSpan = GetUniformLocationSpan(uniform);
            Bool placed = false;
            for (; locNeedle <= artifacts.maxUniformLocation; locNeedle++) {
                bool hasRoom = locNeedle + locationSpan - 1 <= artifacts.maxUniformLocation;
                for (Int element = 0; hasRoom && element < locationSpan; ++element) {
                    hasRoom = artifacts.uniformIndexInTProgram[locNeedle + element] ==
                                  glslang::TQualifier::layoutLocationEnd &&
                              !locationIsReserved(locNeedle + element);
                }
                if (!hasRoom) continue;
                // Found a vacant location at locNeedle
                for (Int element = 0; element < locationSpan; ++element) {
                    artifacts.uniformIndexInTProgram[locNeedle + element] = index;
                }
                artifacts.uniformLocations[uniform.name] = locNeedle;
                MGLOG_D("ProgramObject %u: Reflection - assigned unallocated uniform '%s' to locations %zu..%zu "
                        "(index %d)",
                        in.externalIndex, uniform.name.c_str(), locNeedle, locNeedle + locationSpan - 1, index);
                locNeedle += locationSpan;
                placed = true;
                break;
            }
            if (!placed) {
                // Explicit-location uniforms can fragment the space so no contiguous
                // span is left; grow the table instead of leaving the uniform without
                // a location (which would make it unsettable via glUniform*).
                const SizeT base = artifacts.uniformIndexInTProgram.size();
                artifacts.uniformIndexInTProgram.resize(base + locationSpan,
                                                        glslang::TQualifier::layoutLocationEnd);
                artifacts.uniformSamplerOrImageUnitIndex.resize(base + locationSpan, -1);
                artifacts.maxUniformLocation = static_cast<Uint>(base + locationSpan - 1);
                for (Int element = 0; element < locationSpan; ++element) {
                    artifacts.uniformIndexInTProgram[base + element] = index;
                }
                artifacts.uniformLocations[uniform.name] = static_cast<Uint>(base);
                MGLOG_D("ProgramObject %u: Reflection - grew location table to place uniform '%s' at %zu..%zu",
                        in.externalIndex, uniform.name.c_str(), base, base + locationSpan - 1);
                locNeedle = base + locationSpan;
            }
        }

        for (const Int i : artifacts.glUniformIndexToTProgram) {
            auto& uniform = artifacts.program->getUniform(i);
            const auto locationIt = artifacts.uniformLocations.find(uniform.name);
            if (locationIt == artifacts.uniformLocations.end()) {
                continue;
            }

            const Uint location = locationIt->second;
            if (location >= artifacts.uniformSamplerOrImageUnitIndex.size() || uniform.getType() == nullptr ||
                !uniform.getType()->isOpaque() || (!uniform.getType()->isTexture() && !uniform.getType()->isImage())) {
                continue;
            }

            // Reflection names an array "texs[0]" while the layout(binding = N) map from the IO
            // resolver is keyed by the declared name ("texs"); look up both spellings.
            auto explicitBinding = artifacts.explicitOpaqueUniformBindings.find(uniform.name);
            if (explicitBinding == artifacts.explicitOpaqueUniformBindings.end() && uniform.name.length() > 3 &&
                uniform.name.compare(uniform.name.length() - 3, 3, "[0]") == 0) {
                explicitBinding = artifacts.explicitOpaqueUniformBindings.find(
                    uniform.name.substr(0, uniform.name.length() - 3));
            }
            const int initialUnit = explicitBinding != artifacts.explicitOpaqueUniformBindings.end()
                                        ? static_cast<int>(explicitBinding->second)
                                        : 0;
            const Int locationSpan = GetUniformLocationSpan(uniform);
            for (Int element = 0; element < locationSpan &&
                                  location + element < artifacts.uniformSamplerOrImageUnitIndex.size(); ++element) {
                artifacts.uniformSamplerOrImageUnitIndex[location + element] =
                    initialUnit + (explicitBinding != artifacts.explicitOpaqueUniformBindings.end() ? element : 0);
            }
            MGLOG_D("ProgramObject %u: Reflection - opaque uniform '%s' locations=%u..%u initialUnit=%d",
                    in.externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, initialUnit);
        }

        // ------------ attributes (vertex in) ---------------
        // The pipe-input list is the input interface of the program's FIRST stage, which is only
        // the vertex attribute set when the program actually HAS a vertex stage. A separable
        // fragment/geometry/tessellation program reflects its own stage inputs here, and those are
        // varyings - registering them as vertex attributes would hand glGetActiveAttrib and the
        // attribute location table interstage varyings.
        Int inCount = artifacts.program->getIntermediate(EShLangVertex) != nullptr
                          ? artifacts.program->getNumPipeInputs()
                          : 0;
        MGLOG_D("ProgramObject %u: Reflection - pipe input count (attributes) = %d", in.externalIndex, inCount);

        Int maxLoc = -1;
        for (int i = 0; i < inCount; ++i) {
            Int loc = (Int)artifacts.program->getPipeInput(i).layoutLocation();
            if (loc >= 0 && loc != glslang::TQualifier::layoutLocationEnd) {
                const Int locationSpan = GetVertexInputTotalLocationSpan(artifacts.program->getPipeInput(i));
                maxLoc = std::max(maxLoc, loc + locationSpan - 1);
            }
            MGLOG_D("ProgramObject %u: Reflection - pipe input[%d] name='%s' layoutLocation=%d glType=%u",
                    in.externalIndex, i, artifacts.program->getPipeInput(i).name.c_str(), loc,
                    artifacts.program->getPipeInput(i).glDefineType);
        }

        if (maxLoc < 0) {
            maxLoc = std::max(0, inCount - 1);
        }

        const GLint maxAttribs = GetReflectionVertexAttribLimit(env);
        MGLOG_D("ProgramObject %u: Reflection - computed maxLoc=%d, using maxAttribs=%d", in.externalIndex, maxLoc,
                maxAttribs);

        if (maxLoc >= maxAttribs) {
            DeferLog(std::format("ProgramObject {}: ProgramLinkTask::DoReflection - required attrib location {} >= "
                                 "GL_MAX_VERTEX_ATTRIBS ({}). Clamping.",
                                 in.externalIndex, maxLoc, maxAttribs));
            maxLoc = maxAttribs - 1;
        }

        artifacts.attribs.resize(maxLoc + 1);
        artifacts.attribTypes.resize(maxLoc + 1);

        for (int i = 0; i < inCount; ++i) {
            auto& inVar = artifacts.program->getPipeInput(i);
            Int location = (Int)inVar.layoutLocation();
            // Builtins reflect under their SPIR-V names here; GL_ACTIVE_ATTRIBUTE_MAX_LENGTH
            // must measure the GL spelling glGetActiveAttrib will report.
            artifacts.attribInNameMaxLength =
                std::max(artifacts.attribInNameMaxLength,
                         (Int)ProgramObject::NormalizeBuiltinPipeInputName(inVar.name).length());

            if (location >= 0 && location < (int)artifacts.attribs.size()) {
                const Int locationSpan = GetVertexInputTotalLocationSpan(inVar);
                const GLenum locationType = GetVertexInputLocationType(inVar.glDefineType);
                for (Int locationOffset = 0; locationOffset < locationSpan; ++locationOffset) {
                    const Int expandedLocation = location + locationOffset;
                    if (expandedLocation < 0 || expandedLocation >= static_cast<Int>(artifacts.attribs.size())) {
                        break;
                    }

                    artifacts.attribs[expandedLocation] = inVar.name;
                    artifacts.attribTypes[expandedLocation] = locationType;
                    MGLOG_D(
                        "ProgramObject %u: Reflection - got attrib '%s' at expanded location %d (baseLocation=%d glType=%u expandedType=%u)",
                        in.externalIndex,
                        inVar.name.c_str(),
                        expandedLocation,
                        location,
                        inVar.glDefineType,
                        static_cast<Uint32>(locationType));
                }
            }
        }

        // ---------- UBO ----------
        // GL-visible blocks only (MGL_GLOBAL_UBO was filtered out above).
        const Int uboCount = static_cast<Int>(artifacts.glBlockIndexToTProgram.size());
        MGLOG_D("ProgramObject %u: Reflection - uniform block count (UBO) = %d", in.externalIndex, uboCount);
        artifacts.uniformBlockBinding.resize(uboCount, -1);
        for (Int i = 0; i < uboCount; i++) {
            auto& ubo = artifacts.program->getUniformBlock(artifacts.glBlockIndexToTProgram[i]);
            artifacts.uniformBlockNameMaxLength =
                std::max(artifacts.uniformBlockNameMaxLength, (Int)ubo.name.length());
            artifacts.uniformBlockIndexByName[ubo.name] = i;
            // if there's binding defined in shader as layout(binding = ...),
            // retrieve it here.
            //
            // An instance array takes CONSECUTIVE binding points: "layout(binding = 2)
            // uniform GOKU {...} goku[14];" puts goku[0] on 2 and goku[13] on 15 (GL 4.6
            // 7.6.2 / GLSL 4.20 4.4.5). glslang expands the array into one reflection
            // record per element but hands every one of them the DECLARED binding, because
            // they all share the block's TType - so the element offset has to be added
            // here. Without it every element reported the base binding, and since both
            // backends feed a block from GetUniformBlockBinding() at draw time
            // (DirectGLES.cpp / UniformManager.cpp), all 14 elements also read the same
            // buffer. This is the rule the storage-block path in ProgramInterface.cpp
            // already applies, and whose comment there claims uniform blocks follow.
            const Int declaredBinding = ubo.getBinding();
            artifacts.uniformBlockBinding[i] =
                declaredBinding < 0 ? declaredBinding : declaredBinding + BlockArrayElement(ubo.name);
            MGLOG_D("ProgramObject %u: Reflection - UBO[%d] name='%s' size=%u binding=%d", in.externalIndex, i,
                    ubo.name.c_str(), ubo.size, ubo.getBinding());
        }
        return true;
    }

    Bool ProgramLinkTask::ValidateFragmentOutputLocations() {
        if (!artifacts.program) return false;
        // The pipe-output list is the output interface of the program's LAST stage. Only a
        // fragment stage's outputs are color numbers indexed against GL_MAX_DRAW_BUFFERS; a
        // separable vertex/geometry/tessellation program's outputs are varyings, and holding
        // them to the draw-buffer range fails the link of every such program.
        if (artifacts.program->getIntermediate(EShLangFragment) == nullptr) return true;

        UnorderedMap<Int, String> colorNumberOwners;
        const Int outputCount = artifacts.program->getNumPipeOutputs();
        for (Int index = 0; index < outputCount; ++index) {
            const auto& output = artifacts.program->getPipeOutput(index);
            if (IsBuiltInPipelineOutput(output)) {
                continue;
            }

            const String outputName = StripArrayElementSuffix(output.name);
            const auto explicitLocation = in.explicitFragDataLocation.find(outputName);
            const Int location = explicitLocation != in.explicitFragDataLocation.end()
                                     ? static_cast<Int>(explicitLocation->second)
                                     : static_cast<Int>(output.layoutLocation());
            const Int span = std::max<Int>(output.size, 1);

            if (location < 0 || location + span > in.maxFragmentOutputColorNumber) {
                artifacts.infoLog =
                    std::format("Fragment output '{}' location range [{}, {}) exceeds GL_MAX_DRAW_BUFFERS {}.",
                                outputName, location, location + span, in.maxFragmentOutputColorNumber);
                DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
                ProgramObject::ResetLinkArtifacts(artifacts);
                return false;
            }

            for (Int colorNumber = location; colorNumber < location + span; ++colorNumber) {
                auto [owner, inserted] = colorNumberOwners.emplace(colorNumber, outputName);
                if (!inserted) {
                    artifacts.infoLog = std::format("Fragment outputs '{}' and '{}' alias color number {}.",
                                                    owner->second, outputName, colorNumber);
                    DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
                    ProgramObject::ResetLinkArtifacts(artifacts);
                    return false;
                }
            }
        }

        return true;
    }

    Bool ProgramLinkTask::ResolveTransformFeedbackVaryings() {
        artifacts.xfbVaryings.clear();
        // The GL_TRANSFORM_FEEDBACK_VARYING interface enumerates the request verbatim -
        // pseudo-varyings included - while xfbVaryings below keeps only what is actually
        // captured. Snapshot it before the loop consumes gl_NextBuffer/gl_SkipComponentsN.
        artifacts.xfbInterfaceNames = in.requestedXfbVaryings;
        artifacts.xfbStrides.clear();
        artifacts.xfbBufferMode = in.requestedXfbBufferMode;
        artifacts.xfbVaryingNameMaxLength = 0;
        artifacts.xfbNeedsScatteredCapture = false;
        artifacts.xfbPackedStride = 0;
        if (in.requestedXfbVaryings.empty()) {
            return true;
        }

        // Capture happens at the last vertex-processing stage (geometry, then
        // tessellation evaluation, then vertex).
        const glslang::TIntermediate* captureIntermediate = nullptr;
        for (EShLanguage stage : {EShLangGeometry, EShLangTessEvaluation, EShLangVertex}) {
            captureIntermediate = artifacts.program->getIntermediate(stage);
            if (captureIntermediate != nullptr) {
                break;
            }
        }
        if (captureIntermediate == nullptr) {
            artifacts.infoLog =
                "Transform feedback varyings requested but the program has no vertex-processing stage.";
            return false;
        }
        const glslang::TIntermAggregate* linkerObjects = captureIntermediate->findLinkerObjects();

        const Bool interleaved = artifacts.xfbBufferMode == GL_INTERLEAVED_ATTRIBS;
        Uint32 interleavedOffset = 0;
        // ARB_transform_feedback3 lets an interleaved capture leave holes (gl_SkipComponents1..4)
        // and move on to the next buffer (gl_NextBuffer). Both only affect where the following
        // varyings land, so they are consumed here and never become XfbVaryings of their own -
        // which also keeps them out of the name list a backend declares on its own driver.
        Uint32 interleavedBufferIndex = 0;
        Vector<Uint32> interleavedStrides;
        for (SizeT i = 0; i < in.requestedXfbVaryings.size(); ++i) {
            const String& name = in.requestedXfbVaryings[i];
            if (interleaved && name == "gl_NextBuffer") {
                interleavedStrides.push_back(interleavedOffset);
                interleavedOffset = 0;
                ++interleavedBufferIndex;
                artifacts.xfbNeedsScatteredCapture = true;
                continue;
            }
            if (interleaved && name.size() == 18 && name.compare(0, 17, "gl_SkipComponents") == 0 &&
                name[17] >= '1' && name[17] <= '4') {
                interleavedOffset += static_cast<Uint32>(name[17] - '0') * 4;
                artifacts.xfbNeedsScatteredCapture = true;
                continue;
            }
            for (SizeT j = 0; j < i; ++j) {
                if (in.requestedXfbVaryings[j] == name) {
                    artifacts.infoLog = "Transform feedback varying '" + name + "' is specified more than once.";
                    return false;
                }
            }

            ProgramObject::XfbVarying varying;
            varying.name = name;
            Uint32 bytesPerElement = 0;
            Bool resolved = false;
            if (name == "gl_Position") {
                varying.type = GL_FLOAT_VEC4;
                varying.size = 1;
                bytesPerElement = 16;
                resolved = true;
            } else if (name == "gl_PointSize") {
                varying.type = GL_FLOAT;
                varying.size = 1;
                bytesPerElement = 4;
                resolved = true;
            } else if (linkerObjects != nullptr) {
                // GL lets a capture name a single element of an output array ("b[0]"), which
                // captures one element of the element type - not the whole array. Strip a
                // trailing strict-decimal subscript and look the base declaration up.
                String declaredName = name;
                Bool singleElement = false;
                Uint element = 0;
                if (name.size() > 3 && name.back() == ']') {
                    const SizeT bracket = name.rfind('[');
                    if (bracket != String::npos && bracket + 1 < name.size() - 1) {
                        Bool digitsOnly = true;
                        for (SizeT c = bracket + 1; c + 1 < name.size(); ++c) {
                            if (name[c] < '0' || name[c] > '9') {
                                digitsOnly = false;
                                break;
                            }
                            element = element * 10 + static_cast<Uint>(name[c] - '0');
                        }
                        if (digitsOnly) {
                            declaredName = name.substr(0, bracket);
                            singleElement = true;
                        }
                    }
                }
                // GL 4.6 core 11.1.2.1 (and the resource-name rule of 7.3.1.1): a member of
                // an output interface block is named "<BLOCK name>.<member>" - the block's
                // TYPE name, never the instance name, and that holds for an anonymous
                // instance too. glslang's linker object for such a block is the *instance*
                // symbol ("vs_out", or "anon@N" when there is none), so the head of the
                // dotted path has to be matched against getType().getTypeName() instead of
                // getName(). Without this every capture of a block member resolved to
                // nothing and the link failed with "is not an output of the vertex stage".
                String blockName;
                String memberName;
                if (const SizeT dot = declaredName.find('.'); dot != String::npos) {
                    blockName = declaredName.substr(0, dot);
                    memberName = declaredName.substr(dot + 1);
                    // An array of block instances is spelled "<block>[i].<member>"; every
                    // instance shares one member list, so the subscript only has to go.
                    if (!blockName.empty() && blockName.back() == ']') {
                        const SizeT bracket = blockName.rfind('[');
                        if (bracket != String::npos) blockName.resize(bracket);
                    }
                }

                for (const auto* node : linkerObjects->getSequence()) {
                    const glslang::TIntermSymbol* symbol = node->getAsSymbolNode();
                    if (symbol == nullptr || symbol->getType().getQualifier().storage != glslang::EvqVaryingOut) {
                        continue;
                    }
                    const glslang::TType& symbolType = symbol->getType();
                    const glslang::TType* capturedType = nullptr;
                    if (memberName.empty()) {
                        if (symbol->getName() != declaredName.c_str()) {
                            continue;
                        }
                        capturedType = &symbolType;
                    } else {
                        if (symbolType.getBasicType() != glslang::EbtBlock) {
                            continue;
                        }
                        // The spec spelling is the block name; the instance name is accepted
                        // as a fallback so a request written the (common, non-conformant)
                        // instance-qualified way resolves instead of failing the whole link.
                        if (symbolType.getTypeName() != blockName.c_str() &&
                            symbol->getName() != blockName.c_str()) {
                            continue;
                        }
                        const glslang::TTypeList* members = symbolType.getStruct();
                        if (members == nullptr) {
                            continue;
                        }
                        for (SizeT m = 0; m < members->size(); ++m) {
                            const glslang::TType* memberType = (*members)[m].type;
                            if (memberType == nullptr || memberType->getFieldName() != memberName.c_str()) {
                                continue;
                            }
                            capturedType = memberType;
                            varying.blockMemberIndex = static_cast<Int>(m);
                            break;
                        }
                        if (capturedType == nullptr) {
                            // Right block, wrong member: no other linker object can match.
                            break;
                        }
                        varying.blockName = symbolType.getTypeName().c_str();
                        varying.blockInstanceName = symbol->getName().c_str();
                    }
                    resolved = ResolveXfbSymbolType(*capturedType, varying.type, varying.size, bytesPerElement);
                    if (resolved && singleElement) {
                        if (static_cast<Int>(element) >= varying.size) {
                            resolved = false;
                            break;
                        }
                        varying.size = 1;
                        if (varying.blockMemberIndex >= 0) {
                            varying.blockMemberElement = static_cast<Int>(element);
                        }
                    }
                    break;
                }
            }
            if (!resolved) {
                artifacts.infoLog =
                    "Transform feedback varying '" + name + "' is not an output of the vertex stage.";
                return false;
            }

            varying.byteSize = bytesPerElement * static_cast<Uint32>(varying.size);
            varying.packedOffsetBytes = artifacts.xfbPackedStride;
            artifacts.xfbPackedStride += varying.byteSize;
            if (interleaved) {
                varying.bufferIndex = interleavedBufferIndex;
                varying.offsetBytes = interleavedOffset;
                interleavedOffset += varying.byteSize;
            } else {
                varying.bufferIndex = static_cast<Uint32>(artifacts.xfbVaryings.size());
                varying.offsetBytes = 0;
            }
            artifacts.xfbVaryingNameMaxLength =
                std::max(artifacts.xfbVaryingNameMaxLength, static_cast<Int>(name.size()) + 1);
            artifacts.xfbVaryings.push_back(Move(varying));
        }

        constexpr Uint32 kMaxSeparateAttribs = 4;
        constexpr Uint32 kMaxSeparateComponents = 4;
        constexpr Uint32 kMaxInterleavedComponents = 64;
        constexpr Uint32 kMaxTransformFeedbackBuffers = 4;
        if (interleaved) {
            interleavedStrides.push_back(interleavedOffset);
            if (interleavedStrides.size() > kMaxTransformFeedbackBuffers) {
                artifacts.infoLog = "Transform feedback capture uses more buffers than "
                                    "GL_MAX_TRANSFORM_FEEDBACK_BUFFERS.";
                return false;
            }
            for (const Uint32 stride : interleavedStrides) {
                if (stride > kMaxInterleavedComponents * 4) {
                    artifacts.infoLog = "Transform feedback interleaved capture exceeds "
                                        "GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS.";
                    return false;
                }
            }
            artifacts.xfbStrides = Move(interleavedStrides);
        } else {
            if (artifacts.xfbVaryings.size() > kMaxSeparateAttribs) {
                artifacts.infoLog = "Transform feedback separate capture exceeds "
                                    "GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS.";
                return false;
            }
            artifacts.xfbStrides.resize(artifacts.xfbVaryings.size());
            for (SizeT i = 0; i < artifacts.xfbVaryings.size(); ++i) {
                if (artifacts.xfbVaryings[i].byteSize > kMaxSeparateComponents * 4) {
                    artifacts.infoLog = "Transform feedback varying '" + artifacts.xfbVaryings[i].name +
                                        "' exceeds GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS.";
                    return false;
                }
                artifacts.xfbStrides[i] = artifacts.xfbVaryings[i].byteSize;
            }
        }

        ResolveGsTriangleStripCapture(captureIntermediate);
        return true;
    }

    void ProgramLinkTask::ResolveGsTriangleStripCapture(const glslang::TIntermediate* captureIntermediate) {
        artifacts.gsStripTriangles.clear();
        artifacts.gsStripCaptureFixup = false;
        if (captureIntermediate == nullptr || artifacts.program == nullptr) {
            return;
        }
        if (artifacts.program->getIntermediate(EShLangGeometry) != captureIntermediate) {
            return;
        }
        if (captureIntermediate->getOutputPrimitive() != glslang::ElgTriangleStrip) {
            return;
        }
        GsEmitSequenceTraverser traverser;
        const_cast<glslang::TIntermediate*>(captureIntermediate)->getTreeRoot()->traverse(&traverser);
        traverser.FlushStrip(); // the invocation end acts as an implicit EndPrimitive
        if (!traverser.hasEmit || traverser.inControlFlow || traverser.stripTriangles.empty()) {
            return;
        }
        artifacts.gsStripTriangles = Move(traverser.stripTriangles);
        artifacts.gsStripCaptureFixup = true;
    }
} // namespace MobileGL::MG_State::GLState
