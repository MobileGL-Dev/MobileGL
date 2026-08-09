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

    static bool IsBuiltInPipelineOutput(const glslang::TObjectReflection& output) {
        const auto* type = output.getType();
        return type && type->getQualifier().builtIn != glslang::EbvNone;
    }

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

    static bool ComputeShaderDeclaresLocalSize(const MobileGL::String& source) {
        bool inLineComment = false;
        bool inBlockComment = false;
        for (MobileGL::SizeT i = 0; i < source.length(); ++i) {
            if (inLineComment) {
                inLineComment = source[i] != '\n';
                continue;
            }
            if (inBlockComment) {
                if (source[i] == '*' && i + 1 < source.length() && source[i + 1] == '/') {
                    inBlockComment = false;
                    ++i;
                }
                continue;
            }
            if (source[i] == '/' && i + 1 < source.length()) {
                if (source[i + 1] == '/') {
                    inLineComment = true;
                    ++i;
                    continue;
                }
                if (source[i + 1] == '*') {
                    inBlockComment = true;
                    ++i;
                    continue;
                }
            }
            if (source.compare(i, 11, "local_size_") == 0) {
                return true;
            }
        }
        return false;
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

        // SPIR-V must be generated BEFORE buildReflection touches artifacts.program:
        // reflection's live-variable analysis mutates the intermediates in ways that
        // change subsequent GlslangToSpv output (observed: catastrophic uniform
        // misbinding on DirectVulkan for UBO-heavy content). The old two-link pipeline
        // never ran buildReflection on the SPIR-V-producing program; this order keeps
        // that property with the single link. The glUniform*-to-scratch routing
        // tables, in contrast, are sized and keyed by reflection results, so they are
        // built strictly AFTER DoReflection. (Everything else on the reflection
        // surface - locations, sampler units, block bindings/sizes - was measured
        // identical in either order.)
        MGLOG_D("ProgramObject %u: Starting SPIR-V generation", in.externalIndex);
        GenerateSpirv();

        MGLOG_D("ProgramObject %u: Starting reflection", in.externalIndex);
        if (!DoReflection(env)) {
            DeferLog(std::format("ProgramObject {}: Link failed during reflection: {}", in.externalIndex,
                                 artifacts.infoLog));
            return;
        }

        MGLOG_D("ProgramObject %u: Building global-UBO routing tables", in.externalIndex);
        BuildGlobalUboRouting();
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
        MGLOG_D("ProgramObject %u: Binary generation finished (generatedSpirv size=%zu)", in.externalIndex,
                artifacts.generatedSpirv.size());
    }

    Bool ProgramLinkTask::ConsumeShaders(Vector<SharedPtr<glslang::TShader>>& outShaders) {
        outShaders.assign(in.shaders.size(), nullptr);

        for (SizeT i = 0; i < in.shaders.size(); i++) {
            const LinkShaderInput& input = in.shaders[i];
            const GLenum shaderType = MG_Util::ConvertShaderStageToGLEnum(input.stage);
            const ShaderCompileArtifacts& compiled = CompiledArtifacts(input.compiled);
            MGLOG_D("ProgramObject %u: Preparing shader[%zu] stage %s", in.externalIndex, i,
                    MG_Util::ConvertGLEnumToString(shaderType).c_str());

            if (!compiled.compileStatus) {
                artifacts.infoLog =
                    std::format("Linking a {} with compilation error, linking will now terminate. Shader error "
                                "log:\n{}\nShader src:\n{}",
                                MG_Util::ConvertGLEnumToString(shaderType), compiled.infoLog,
                                input.source ? *input.source : String());
                DeferLog(std::format("ProgramObject {}: Link failed - shader[{}] compile status false. InfoLog:\n{}",
                                     in.externalIndex, i, artifacts.infoLog));
                return false;
            }
            if (input.stage == ShaderStage::Compute &&
                !ComputeShaderDeclaresLocalSize(input.source ? *input.source : String())) {
                artifacts.infoLog = "Compute shader is missing a local_size layout declaration.";
                DeferLog(std::format("ProgramObject {}: Link failed - {}", in.externalIndex, artifacts.infoLog));
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
        if (!artifacts.program->buildReflection(EShReflectionStrictArraySuffix | EShReflectionBasicArraySuffix |
                                                EShReflectionAllBlockVariables | EShReflectionSharedStd140UBO)) {
            artifacts.linkStatus = false;
            artifacts.infoLog = "Build reflection failed.";
            DeferLog(std::format("ProgramObject {}: DoReflection - buildReflection() returned false",
                                 in.externalIndex));
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
        constexpr Uint kNoLocation = glslang::TQualifier::layoutLocationEnd;
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
            if (effectiveLocation[i] == kNoLocation && type != nullptr && type->isOpaque()) {
                effectiveLocation[i] = uniform.layoutLocation();
            }
            if (locationIsSourceExplicit[i] &&
                effectiveLocation[i] + static_cast<Uint>(GetUniformLocationSpan(uniform)) > kNoLocation) {
                // Config A rejected out-of-range explicit locations at parse; keep them
                // from growing the location table unboundedly.
                artifacts.infoLog = std::format("Uniform '{}' explicit location {} is out of range.", uniform.name,
                                                effectiveLocation[i]);
                ProgramObject::ResetLinkArtifacts(artifacts);
                return false;
            }
        }

        Int requiredUniformLocations = 0;
        for (const Int i : artifacts.glUniformIndexToTProgram) {
            auto& uniform = artifacts.program->getUniform(i);
            const Uint location = effectiveLocation[i];
            const Int locationSpan = GetUniformLocationSpan(uniform);
            requiredUniformLocations += locationSpan;
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
                    artifacts.uniformIndexInTProgram[location + element] == glslang::TQualifier::layoutLocationEnd;
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
                              glslang::TQualifier::layoutLocationEnd;
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
        Int inCount = artifacts.program->getNumPipeInputs();
        MGLOG_D("ProgramObject %u: Reflection - pipe input count (attributes) = %d", in.externalIndex, inCount);

        Int maxLoc = -1;
        for (int i = 0; i < inCount; ++i) {
            Int loc = (Int)artifacts.program->getPipeInput(i).layoutLocation();
            if (loc >= 0 && loc != glslang::TQualifier::layoutLocationEnd) {
                const Int locationSpan = GetVertexInputLocationSpan(artifacts.program->getPipeInput(i).glDefineType);
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
                const Int locationSpan = GetVertexInputLocationSpan(inVar.glDefineType);
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
            // retrieve it here
            artifacts.uniformBlockBinding[i] = ubo.getBinding();
            MGLOG_D("ProgramObject %u: Reflection - UBO[%d] name='%s' size=%u binding=%d", in.externalIndex, i,
                    ubo.name.c_str(), ubo.size, ubo.getBinding());
        }
        return true;
    }

    void ProgramLinkTask::GenerateSpirv() {
        /* As we passed first stage compilation/linking,
         * we'll assume all the operations here should
         * pass. We may be able to employ some optimizations
         * here without the burden of error reporting.
         */
        using namespace MG_Util::ShaderTranspiler;
        MGLOG_D("ProgramObject %u: GenerateSpirv - start", in.externalIndex);

        // The shaders were parsed once, in the link-compatible (relaxed Vulkan-rules)
        // configuration, and artifacts.program linked those parses - so artifacts.program IS
        // the program the backends consume. Generate SPIR-V straight from its
        // intermediates; the full re-parse + re-link that used to live here (one
        // glslang pass per shader per link) is gone.
        Vector<GLenum> shaderTypes(in.shaders.size());
        for (SizeT i = 0; i < in.shaders.size(); i++) {
            shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(in.shaders[i].stage);
        }

        ProgramBinaryAttrib binaryAttrib{
            .shaderTypes = shaderTypes,
            .program = *artifacts.program,
        };
        MGLOG_D("ProgramObject %u: GenerateSpirv - requesting SPIR-V binary from program", in.externalIndex);
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult) {
            DeferLog(std::format("ProgramObject {}: GenerateSpirv - GetSpirvBinaryFromProgram failed",
                                 in.externalIndex));
        }
        MOBILEGL_ASSERT(binaryResult, "GetSpirvBinaryFromProgram failed");
        artifacts.generatedSpirv = Move(binaryResult.value());
        MGLOG_D("ProgramObject %u: GenerateSpirv - generated %zu SPIR-V modules", in.externalIndex,
                artifacts.generatedSpirv.size());

        // Linked SPIR-V generated, sanitize and optimize it
        for (auto& spv : artifacts.generatedSpirv) {
            auto success = ShaderCompiler::SanitizeAndOptimizeBinary(spv, spv);
            MOBILEGL_ASSERT(success, "SanitizeBinary failed");
        }
    }

    void ProgramLinkTask::BuildGlobalUboRouting() {
        using namespace MG_Util::ShaderTranspiler;
        Vector<GLenum> shaderTypes(in.shaders.size());
        for (SizeT i = 0; i < in.shaders.size(); i++) {
            shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(in.shaders[i].stage);
        }

        artifacts.uniformSizesInBytes.clear();
        artifacts.uniformOffsets.clear();
        artifacts.globalUboScratch.clear();
        // kInvalidUniformOffset marks locations that end up without global-UBO backing
        // (e.g. the optimizer eliminated every use of the uniform); the fallback pass
        // below gives those locations tail storage so glUniform* always has a target.
        artifacts.uniformOffsets.resize(artifacts.maxUniformLocation + 1, ProgramObject::kInvalidUniformOffset);
        artifacts.uniformSizesInBytes.resize(artifacts.maxUniformLocation + 1, 0);
        for (SizeT i = 0; i < artifacts.generatedSpirv.size(); i++) {
            auto& spv = artifacts.generatedSpirv[i];

            auto shaderType = shaderTypes[i];
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - parsing SPIR-V meta data for module %zu "
                    "(shaderType=%u, wordCount=%zu)",
                    in.externalIndex, i, shaderType, spv.size());
            SpvcSession session(spv, SessionUsageBit::Reflection);
            auto result = session.ParseMetaData();
            if (result < 0) {
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SpvcSession::ParseMetaData failed for module %zu, "
                        "err = %d%s",
                        in.externalIndex, i, result,
                        (result == SPVC_ERROR_INVALID_SPIRV ? ". Probably no global UBO?" : ""));
                continue;
            } else {
                auto& meta = session.GetMetadata();
                auto size = meta.globalUboSize;
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SPIR-V meta: uboSize=%zu plainUniformCount=%zu "
                        "plainUniformOffsets=%zu",
                        in.externalIndex, meta.globalUboSize, meta.plainUniformMemberSizesInBytes.size(),
                        meta.plainUniformOffsetsInUBO.size());
                if (size == 0) {
                    continue;
                }
                if (artifacts.globalUboScratch.size() < size) {
                    artifacts.globalUboScratch.resize(size);
                }
                for (const auto& [name, offset] : meta.plainUniformOffsetsInUBO) {
                    // SPIRV-Reflect leaf names never carry a "[0]" suffix; frontend
                    // reflection keys arrays as "arr[0]" (GL naming), so retry with the
                    // suffix before declaring the uniform unbacked.
                    auto locationIt = artifacts.uniformLocations.find(name);
                    if (locationIt == artifacts.uniformLocations.end()) {
                        locationIt = artifacts.uniformLocations.find(name + "[0]");
                    }
                    if (locationIt == artifacts.uniformLocations.end()) {
                        MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u but not found in "
                                "uniformLocations",
                                in.externalIndex, name.c_str(), offset);
                        continue;
                    }
                    const Uint baseLocation = locationIt->second;
                    if (!ProgramObject::IsValidUniformLocation(artifacts, static_cast<Int>(baseLocation))) {
                        continue;
                    }

                    const Int uniformIndex = artifacts.uniformIndexInTProgram[baseLocation];
                    const GLint arraySize = ProgramObject::GetUniformArraySizeByTIndex(artifacts, uniformIndex);
                    SizeT memberSize = 0;
                    const auto sizeIt = meta.plainUniformMemberSizesInBytes.find(name);
                    if (sizeIt != meta.plainUniformMemberSizesInBytes.end()) {
                        memberSize = sizeIt->second;
                    }
                    Uint arrayStride = 0;
                    const auto strideIt = meta.plainUniformArrayStridesInUBO.find(name);
                    if (strideIt != meta.plainUniformArrayStridesInUBO.end()) {
                        arrayStride = strideIt->second;
                    }

                    // Array uniforms span one location per element (see DoReflection);
                    // give each element its real byte offset inside the UBO.
                    const GLint elementCount = (arraySize > 1 && arrayStride == 0) ? 1 : std::max(arraySize, 1);
                    for (GLint element = 0; element < elementCount; ++element) {
                        const Uint location = baseLocation + static_cast<Uint>(element);
                        if (location > artifacts.maxUniformLocation ||
                            artifacts.uniformIndexInTProgram[location] != uniformIndex) {
                            break;
                        }
                        artifacts.uniformOffsets[location] = offset + static_cast<Uint>(element) * arrayStride;
                        const SizeT consumed = static_cast<SizeT>(element) * arrayStride;
                        artifacts.uniformSizesInBytes[location] = memberSize > consumed ? memberSize - consumed : 0;
                    }
                    MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u stride=%u size=%zu assigned "
                            "to locations %u..%u",
                            in.externalIndex, name.c_str(), offset, arrayStride, memberSize, baseLocation,
                            baseLocation + static_cast<Uint>(elementCount) - 1);
                }
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - finished parsing module %zu metadata",
                        in.externalIndex, i);
            }
        }

        // Fallback pass: a linked program's active non-opaque uniforms must accept
        // glUniform*/glGetUniform* even when the optimized SPIR-V no longer contains
        // them (AggressiveDCE can remove a dead loop together with the only loads of a
        // uniform -- or the entire global UBO, leaving the scratch unallocated). Hand
        // such locations CPU-side storage at the (16-byte aligned) tail of the shadow
        // buffer; backends bind at least the SPIR-V-declared UBO range, and the GPU
        // never reads these bytes, so this only keeps the GL-visible state coherent.
        for (Uint location = 0; location <= artifacts.maxUniformLocation; ++location) {
            if (artifacts.uniformOffsets[location] != ProgramObject::kInvalidUniformOffset) continue;
            if (!ProgramObject::IsValidUniformLocation(artifacts, static_cast<Int>(location))) continue;
            const auto& uniform = artifacts.program->getUniform(artifacts.uniformIndexInTProgram[location]);
            const glslang::TType* type = uniform.getType();
            if (type != nullptr && type->isOpaque()) continue;
            if (uniform.index >= 0 && uniform.index < artifacts.program->getNumUniformBlocks() &&
                std::strstr(artifacts.program->getUniformBlock(uniform.index).name.c_str(),
                            MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) == nullptr) {
                // Member of a named uniform block: not settable through glUniform*, so it
                // needs no global-UBO shadow storage.
                continue;
            }

            // std140-style slot: the matrix upload paths write column vectors at
            // 16-byte strides, so a matrix slot must cover cols * 16 bytes.
            SizeT slotSize = MG_Util::GetGLTypeSize(uniform.glDefineType);
            if (type != nullptr && type->isMatrix()) {
                slotSize = static_cast<SizeT>(type->getMatrixCols()) * 16u;
            }
            slotSize = (slotSize + 15u) & ~static_cast<SizeT>(15u);
            const SizeT slotOffset = (artifacts.globalUboScratch.size() + 15u) & ~static_cast<SizeT>(15u);
            artifacts.globalUboScratch.resize(slotOffset + slotSize, 0);
            artifacts.uniformOffsets[location] = static_cast<Uint>(slotOffset);
            artifacts.uniformSizesInBytes[location] = slotSize;
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' location %u has no UBO backing in the "
                    "generated SPIR-V (optimized out?); allocated %zu fallback bytes at scratch offset %zu",
                    in.externalIndex, uniform.name.c_str(), location, slotSize, slotOffset);
        }
    }

    Bool ProgramLinkTask::ValidateFragmentOutputLocations() {
        if (!artifacts.program) return false;

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
                for (const auto* node : linkerObjects->getSequence()) {
                    const glslang::TIntermSymbol* symbol = node->getAsSymbolNode();
                    if (symbol == nullptr || symbol->getType().getQualifier().storage != glslang::EvqVaryingOut) {
                        continue;
                    }
                    if (symbol->getName() != declaredName.c_str()) {
                        continue;
                    }
                    resolved = ResolveXfbSymbolType(symbol->getType(), varying.type, varying.size, bytesPerElement);
                    if (resolved && singleElement) {
                        if (static_cast<Int>(element) >= varying.size) {
                            resolved = false;
                            break;
                        }
                        varying.size = 1;
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
