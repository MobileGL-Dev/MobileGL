// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramObject.h"
#include <atomic>
#include <cstring>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>

const char* kDefaultFragmentShaderSource = R"(#version 460 core
layout(location = 0) out vec4 FragColor;
void main() {}
)";

namespace {
    // How many vertex input locations reflection may record. Backends consume this through
    // GetActiveAttributeLocationMask()/GetAttribType(), so a value below the advertised
    // GL_MAX_VERTEX_ATTRIBS would make a legal attribute location invisible to them -- DirectGLES would
    // then never feed the shader that attribute's current value. Bounded by the state layer's storage
    // capacity, which is also the width of the Uint32 masks backends build from it.
    static MobileGL::Int GetReflectionVertexAttribLimit() {
        constexpr MobileGL::Int capacity =
            static_cast<MobileGL::Int>(MobileGL::MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
        if (!MobileGL::MG_Backend::pActiveBackendObject) return capacity;

        const MobileGL::Int backendLimit =
            MobileGL::MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxVertexAttribs;
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
}

namespace MobileGL::MG_State::GLState {
    static std::atomic<Uint64> s_nextProgramLifetimeId = 1;

    Uint64 ProgramObject::AllocateLifetimeId() {
        return s_nextProgramLifetimeId.fetch_add(1, std::memory_order_relaxed);
    }

    void ProgramObject::ResetLinkArtifacts() {
        // Relinking regenerates the SPIR-V, so any backend-cached state keyed on
        // m_backendStateVersion (e.g. the content-hash memo) must be invalidated,
        // along with every link-derived backend cache (m_linkVersion) and the
        // last-uploaded-UBO gate (a relink resets uniforms to their initial values,
        // and that reset must reach the GPU).
        ++m_backendStateVersion;
        ++m_linkVersion;
        MarkUBOContentDirty();
        m_program.reset();
        m_generatedSpirv.clear();
        m_uniformLocations.clear();
        m_glUniformIndexToTProgram.clear();
        m_tProgramUniformIndexToGl.clear();
        m_glBlockIndexToTProgram.clear();
        m_tProgramBlockIndexToGl.clear();
        m_linkedExplicitUniformLocations.clear();
        m_uniformIndexInTProgram.clear();
        m_uniformSamplerOrImageUnitIndex.clear();
        m_explicitOpaqueUniformBindings.clear();
        m_uniformBlockIndexByName.clear();
        m_uniformBlockBinding.clear();
        m_uniformOffsets.clear();
        m_uniformSizesInBytes.clear();
        m_globalUboScratch.clear();
        m_attribs.clear();
        m_attribTypes.clear();
        m_activeUniformCount = 0;
        m_maxUniformLocation = 0;
        m_uniformNameMaxLength = 0;
        m_attribInNameMaxLength = 0;
        m_uniformBlockNameMaxLength = 0;
        m_xfbVaryings.clear();
        m_xfbStrides.clear();
        m_xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        m_xfbVaryingNameMaxLength = 0;
        m_xfbNeedsScatteredCapture = false;
        m_xfbPackedStride = 0;
        m_gsInputPrimitive = GL_NONE;
        m_linkStatus = false;
    }

    namespace {
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
    } // namespace

    Bool ProgramObject::ResolveTransformFeedbackVaryings() {
        m_xfbVaryings.clear();
        m_xfbStrides.clear();
        m_xfbBufferMode = m_requestedXfbBufferMode;
        m_xfbVaryingNameMaxLength = 0;
        m_xfbNeedsScatteredCapture = false;
        m_xfbPackedStride = 0;
        if (m_requestedXfbVaryings.empty()) {
            return true;
        }

        // Capture happens at the last vertex-processing stage (geometry, then
        // tessellation evaluation, then vertex).
        const glslang::TIntermediate* captureIntermediate = nullptr;
        for (EShLanguage stage : {EShLangGeometry, EShLangTessEvaluation, EShLangVertex}) {
            captureIntermediate = m_program->getIntermediate(stage);
            if (captureIntermediate != nullptr) {
                break;
            }
        }
        if (captureIntermediate == nullptr) {
            m_infoLog = "Transform feedback varyings requested but the program has no vertex-processing stage.";
            return false;
        }
        const glslang::TIntermAggregate* linkerObjects = captureIntermediate->findLinkerObjects();

        const Bool interleaved = m_xfbBufferMode == GL_INTERLEAVED_ATTRIBS;
        Uint32 interleavedOffset = 0;
        // ARB_transform_feedback3 lets an interleaved capture leave holes (gl_SkipComponents1..4)
        // and move on to the next buffer (gl_NextBuffer). Both only affect where the following
        // varyings land, so they are consumed here and never become XfbVaryings of their own -
        // which also keeps them out of the name list a backend declares on its own driver.
        Uint32 interleavedBufferIndex = 0;
        Vector<Uint32> interleavedStrides;
        for (SizeT i = 0; i < m_requestedXfbVaryings.size(); ++i) {
            const String& name = m_requestedXfbVaryings[i];
            if (interleaved && name == "gl_NextBuffer") {
                interleavedStrides.push_back(interleavedOffset);
                interleavedOffset = 0;
                ++interleavedBufferIndex;
                m_xfbNeedsScatteredCapture = true;
                continue;
            }
            if (interleaved && name.size() == 18 && name.compare(0, 17, "gl_SkipComponents") == 0 &&
                name[17] >= '1' && name[17] <= '4') {
                interleavedOffset += static_cast<Uint32>(name[17] - '0') * 4;
                m_xfbNeedsScatteredCapture = true;
                continue;
            }
            for (SizeT j = 0; j < i; ++j) {
                if (m_requestedXfbVaryings[j] == name) {
                    m_infoLog = "Transform feedback varying '" + name + "' is specified more than once.";
                    return false;
                }
            }

            XfbVarying varying;
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
                for (const auto* node : linkerObjects->getSequence()) {
                    const glslang::TIntermSymbol* symbol = node->getAsSymbolNode();
                    if (symbol == nullptr || symbol->getType().getQualifier().storage != glslang::EvqVaryingOut) {
                        continue;
                    }
                    if (symbol->getName() != name.c_str()) {
                        continue;
                    }
                    resolved = ResolveXfbSymbolType(symbol->getType(), varying.type, varying.size, bytesPerElement);
                    break;
                }
            }
            if (!resolved) {
                m_infoLog = "Transform feedback varying '" + name + "' is not an output of the vertex stage.";
                return false;
            }

            varying.byteSize = bytesPerElement * static_cast<Uint32>(varying.size);
            varying.packedOffsetBytes = m_xfbPackedStride;
            m_xfbPackedStride += varying.byteSize;
            if (interleaved) {
                varying.bufferIndex = interleavedBufferIndex;
                varying.offsetBytes = interleavedOffset;
                interleavedOffset += varying.byteSize;
            } else {
                varying.bufferIndex = static_cast<Uint32>(m_xfbVaryings.size());
                varying.offsetBytes = 0;
            }
            m_xfbVaryingNameMaxLength =
                std::max(m_xfbVaryingNameMaxLength, static_cast<Int>(name.size()) + 1);
            m_xfbVaryings.push_back(Move(varying));
        }

        constexpr Uint32 kMaxSeparateAttribs = 4;
        constexpr Uint32 kMaxSeparateComponents = 4;
        constexpr Uint32 kMaxInterleavedComponents = 64;
        constexpr Uint32 kMaxTransformFeedbackBuffers = 4;
        if (interleaved) {
            interleavedStrides.push_back(interleavedOffset);
            if (interleavedStrides.size() > kMaxTransformFeedbackBuffers) {
                m_infoLog = "Transform feedback capture uses more buffers than "
                            "GL_MAX_TRANSFORM_FEEDBACK_BUFFERS.";
                return false;
            }
            for (const Uint32 stride : interleavedStrides) {
                if (stride > kMaxInterleavedComponents * 4) {
                    m_infoLog = "Transform feedback interleaved capture exceeds "
                                "GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS.";
                    return false;
                }
            }
            m_xfbStrides = Move(interleavedStrides);
        } else {
            if (m_xfbVaryings.size() > kMaxSeparateAttribs) {
                m_infoLog = "Transform feedback separate capture exceeds "
                            "GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS.";
                return false;
            }
            m_xfbStrides.resize(m_xfbVaryings.size());
            for (SizeT i = 0; i < m_xfbVaryings.size(); ++i) {
                if (m_xfbVaryings[i].byteSize > kMaxSeparateComponents * 4) {
                    m_infoLog = "Transform feedback varying '" + m_xfbVaryings[i].name +
                                "' exceeds GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS.";
                    return false;
                }
                m_xfbStrides[i] = m_xfbVaryings[i].byteSize;
            }
        }

        ResolveGsTriangleStripCapture(captureIntermediate);
        return true;
    }

    namespace {
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

    void ProgramObject::ResolveGsTriangleStripCapture(const glslang::TIntermediate* captureIntermediate) {
        m_gsStripTriangles.clear();
        m_gsStripCaptureFixup = false;
        if (captureIntermediate == nullptr || m_program == nullptr) {
            return;
        }
        if (m_program->getIntermediate(EShLangGeometry) != captureIntermediate) {
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
        m_gsStripTriangles = Move(traverser.stripTriangles);
        m_gsStripCaptureFixup = true;
    }

    bool ProgramObject::ShaderIsAttached(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: ShaderIsAttached check for shader %p", m_externalIndex, shader.get());
        auto it = std::find_if(m_shaders.begin(), m_shaders.end(),
                               [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });
        bool attached = it != m_shaders.end();
        MGLOG_D("ProgramObject %u: ShaderIsAttached -> %s", m_externalIndex, attached ? "true" : "false");
        return attached;
    }

    bool ProgramObject::AttachShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: AttachShader called for shader %p", m_externalIndex, shader.get());
        if (ShaderIsAttached(shader)) {
            MGLOG_D("ProgramObject %u: AttachShader - shader already attached, skipping", m_externalIndex);
            return false;
        }
        m_shaders.emplace_back(shader);
        MGLOG_D("ProgramObject %u: AttachShader - attached successfully, total shaders now %zu", m_externalIndex,
                m_shaders.size());
        return true;
    }

    SizeT ProgramObject::DetachShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("DetachShader called for shader %p from ProgramObject %u", shader.get(), m_externalIndex);
        if (!ShaderIsAttached(shader)) {
            MGLOG_D("Shader %p is not attached to ProgramObject %u, cannot detach.", shader.get(), m_externalIndex);
            return 0;
        }
        m_detachedShaders.push_back(shader);
        MGLOG_D("Shader %p marked for detachment from ProgramObject %u", shader.get(), m_externalIndex);
        return 1;
    }

    SizeT ProgramObject::RemoveShader(const SharedPtr<ShaderObject>& shader) {
        MGLOG_D("ProgramObject %u: RemoveShader called for shader %p", m_externalIndex, shader.get());
        auto count =
            std::erase_if(m_shaders, [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });

        MGLOG_D("ProgramObject %u: RemoveShader - removed %zu shader(s), remaining %zu", m_externalIndex, count,
                m_shaders.size());
        return count;
    }

    void ProgramObject::AddDefaultFragmentShaderIfMissing() {
        Bool needsDefaultFS = false;
        for (const auto& shader : m_shaders) {
            auto stage = shader->GetShaderStage();
            if (stage == ShaderStage::Vertex) {
                needsDefaultFS = true;
                continue;
            }
            if (stage == ShaderStage::Fragment) {
                needsDefaultFS = false;
                return;
            }
        }

        if (!needsDefaultFS) return;

        MGLOG_D("ProgramObject %u: No fragment shader attached, adding default fragment shader.", m_externalIndex);
        SharedPtr<ShaderObject> defaultFS = MakeShared<ShaderObject>(ShaderStage::Fragment, 0);
        defaultFS->SetShaderSource(kDefaultFragmentShaderSource);
        defaultFS->Compile(); // TODO: use a global default FS object.
        auto status = defaultFS->GetCompileStatus();
        if (!status) {
            MGLOG_E("ProgramObject %u: Failed to compile default fragment shader. InfoLog:\n%s", m_externalIndex,
                    defaultFS->GetInfoLog().c_str());
            return;
        }
        m_shaders.push_back(defaultFS);
        MGLOG_D("ProgramObject %u: Default fragment shader added.", m_externalIndex);
    }

    void ProgramObject::Link(Bool addDefaultFSIfMissingForRenderingPipelineProgram) {
        MGLOG_D("ProgramObject %u: Link start, shaders to link: %zu", m_externalIndex, m_shaders.size());
        ++m_backendStateVersion;
        ResetLinkArtifacts();
        m_infoLog.clear();
        // Remove detached shaders first
        for (const auto& detachedShader : m_detachedShaders) {
            RemoveShader(detachedShader);
        }
        m_detachedShaders.clear();

        if (addDefaultFSIfMissingForRenderingPipelineProgram) {
            AddDefaultFragmentShaderIfMissing();
        }
        if (m_shaders.empty()) {
            m_infoLog = "No shader objects are attached to program.";
            MGLOG_E("ProgramObject %u: Link failed - no shader objects attached.", m_externalIndex);
            return;
        }

        std::sort(m_shaders.begin(), m_shaders.end(),
                  [](const SharedPtr<ShaderObject>& a, const SharedPtr<ShaderObject>& b) {
                      return a->GetShaderStage() < b->GetShaderStage();
                  });

        Vector<GLenum> shaderTypes(m_shaders.size());
        Vector<SharedPtr<glslang::TShader>> shaders(m_shaders.size());

        for (SizeT i = 0; i < m_shaders.size(); i++) {
            shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(m_shaders[i]->GetShaderStage());
            MGLOG_D("ProgramObject %u: Preparing shader[%zu] stage %s at %p", m_externalIndex, i,
                    MG_Util::ConvertGLEnumToString(shaderTypes[i]).c_str(), m_shaders[i].get());

            if (!m_shaders[i]->GetCompileStatus()) {
                m_infoLog = std::format("Linking a {} with compilation error, linking will now terminate. Shader error "
                                        "log:\n{}\nShader src:\n{}",
                                        MG_Util::ConvertGLEnumToString(shaderTypes[i]), m_shaders[i]->GetInfoLog(),
                                        m_shaders[i]->GetShaderSource());
                MGLOG_E("ProgramObject %u: Link failed - shader[%zu] compile status false. InfoLog:\n%s",
                        m_externalIndex, i, m_infoLog.c_str());
                return;
            }
            if (m_shaders[i]->GetShaderStage() == ShaderStage::Compute &&
                !ComputeShaderDeclaresLocalSize(m_shaders[i]->GetShaderSource())) {
                m_infoLog = "Compute shader is missing a local_size layout declaration.";
                MGLOG_E("ProgramObject %u: Link failed - %s", m_externalIndex, m_infoLog.c_str());
                return;
            }
            String reparseLog;
            shaders[i] = m_shaders[i]->TakeShaderForLink(reparseLog);
            if (!shaders[i]) {
                // Only reachable when the consume-once re-parse of an already-compiled
                // source fails, which no valid state transition produces.
                m_infoLog = std::format("Internal error: re-parsing an attached {} for linking failed:\n{}",
                                        MG_Util::ConvertGLEnumToString(shaderTypes[i]), reparseLog);
                MGLOG_E("ProgramObject %u: Link failed - %s", m_externalIndex, m_infoLog.c_str());
                return;
            }
            MGLOG_D("ProgramObject %u: shader[%zu] compiled shader ptr %p, src len %zu", m_externalIndex, i,
                    shaders[i].get(), m_shaders[i]->GetShaderSource().length());
            MGLOG_D("ProgramObject %u: shader[%zu] source:\n%s", m_externalIndex, i,
                    m_shaders[i]->GetShaderSource().c_str());
        }

        // Merge the shaders' lexically extracted explicit uniform locations. The same
        // uniform declared in several stages must agree on its location (config-A glslang
        // enforced this at mapIO; the relaxed parse no longer sees the qualifiers).
        for (const auto& shader : m_shaders) {
            for (const auto& [name, location] : shader->GetExplicitUniformLocations()) {
                const auto [it, inserted] = m_linkedExplicitUniformLocations.emplace(name, location);
                if (!inserted && it->second != location) {
                    m_infoLog = std::format(
                        "Uniform '{}' is declared with conflicting explicit locations ({} and {}) "
                        "across stages.",
                        name, it->second, location);
                    MGLOG_E("ProgramObject %u: Link failed - %s", m_externalIndex, m_infoLog.c_str());
                    return;
                }
            }
            // Sampler/image layout(binding = N) initial units, likewise invisible to the
            // relaxed parse. Stage order matches the old per-stage mapIO capture, so a
            // name declared in several stages keeps the last stage's binding as before.
            for (const auto& [name, binding] : shader->GetExplicitOpaqueBindings()) {
                m_explicitOpaqueUniformBindings[name] = binding;
            }
        }

        MG_Util::ShaderTranspiler::ProgramAttrib attrib{.shaders = Move(shaders),
                                                        .explicitVertexInLocations = m_explicitAttribLocations,
                                                        .explicitFragmentOutLocations = m_explicitFragDataLocation,
                                                        .explicitFragmentOutIndices = m_explicitFragDataIndex,
                                                        .explicitOpaqueUniformBindings =
                                                            &m_explicitOpaqueUniformBindings};

        MGLOG_D("ProgramObject %u: Calling ShaderCompiler::LinkProgram", m_externalIndex);
        auto result = MG_Util::ShaderTranspiler::ShaderCompiler::LinkProgram(attrib);
        if (result) {
            m_linkStatus = true;
            m_program = result.value();
            m_linkedFragDataLocation = m_explicitFragDataLocation;
            m_linkedFragDataIndex = m_explicitFragDataIndex;
            MGLOG_D("ProgramObject %u: LinkProgram succeeded, TProgram ptr %p", m_externalIndex, m_program.get());
        } else {
            m_infoLog = result.error().log;
            MGLOG_E("ProgramObject %u: LinkProgram failed. InfoLog:\n%s", m_externalIndex, m_infoLog.c_str());
            return;
        }

        // GL_GEOMETRY_INPUT_TYPE. A draw's primitive type has to be compatible with it
        // (GL 4.6 core 11.3.1), so it is resolved for every link, not only a capturing one.
        m_gsInputPrimitive = GL_NONE;
        if (const glslang::TIntermediate* gs = m_program->getIntermediate(EShLangGeometry)) {
            switch (gs->getInputPrimitive()) {
            case glslang::ElgPoints: m_gsInputPrimitive = GL_POINTS; break;
            case glslang::ElgLines: m_gsInputPrimitive = GL_LINES; break;
            case glslang::ElgLinesAdjacency: m_gsInputPrimitive = GL_LINES_ADJACENCY; break;
            case glslang::ElgTriangles: m_gsInputPrimitive = GL_TRIANGLES; break;
            case glslang::ElgTrianglesAdjacency: m_gsInputPrimitive = GL_TRIANGLES_ADJACENCY; break;
            default: break;
            }
        }

        // SPIR-V must be generated BEFORE buildReflection touches m_program:
        // reflection's live-variable analysis mutates the intermediates in ways that
        // change subsequent GlslangToSpv output (observed: catastrophic uniform
        // misbinding on DirectVulkan for UBO-heavy content). The old two-link pipeline
        // never ran buildReflection on the SPIR-V-producing program; this order keeps
        // that property with the single link. The glUniform*-to-scratch routing
        // tables, in contrast, are sized and keyed by reflection results, so they are
        // built strictly AFTER DoReflection. (Everything else on the reflection
        // surface - locations, sampler units, block bindings/sizes - was measured
        // identical in either order.)
        MGLOG_D("ProgramObject %u: Starting SPIR-V generation", m_externalIndex);
        GenerateSpirv();

        MGLOG_D("ProgramObject %u: Starting reflection", m_externalIndex);
        if (!DoReflection()) {
            MGLOG_E("ProgramObject %u: Link failed during reflection: %s", m_externalIndex, m_infoLog.c_str());
            return;
        }

        MGLOG_D("ProgramObject %u: Building global-UBO routing tables", m_externalIndex);
        BuildGlobalUboRouting();
        MGLOG_D("ProgramObject %u: Reflection done (linkStatus=%d)", m_externalIndex, (int)m_linkStatus);
        if (!ValidateFragmentOutputLocations()) {
            return;
        }
        if (!ResolveTransformFeedbackVaryings()) {
            m_linkStatus = false;
            MGLOG_E("ProgramObject %u: transform feedback varying resolution failed: %s", m_externalIndex,
                    m_infoLog.c_str());
            return;
        }
        MGLOG_D("ProgramObject %u: Binary generation finished (generatedSpirv size=%zu)", m_externalIndex,
                m_generatedSpirv.size());
    }

    void ProgramObject::MarkAsDeleted() {
        MGLOG_D("ProgramObject %u: MarkAsDeleted called (was %s)", m_externalIndex,
                m_deleteStatus ? "deleted" : "not deleted");
        m_deleteStatus = true;
        MGLOG_D("ProgramObject %u: MarkAsDeleted - now marked deleted", m_externalIndex);
    }

    Vector<SharedPtr<ShaderObject>>& ProgramObject::GetAttachedShaders() {
        MGLOG_D("ProgramObject %u: GetAttachedShaders called, returning %zu shaders", m_externalIndex,
                m_shaders.size());
        return m_shaders;
    }

    const Vector<SharedPtr<ShaderObject>>& ProgramObject::GetAttachedShaders() const {
        return m_shaders;
    }

    Bool ProgramObject::DoReflection() {
        if (!m_program) {
            MGLOG_E("ProgramObject %u: DoReflection called but m_program is null", m_externalIndex);
            m_linkStatus = false;
            m_infoLog = "DoReflection failed: no program.";
            return false;
        }

        MGLOG_D("ProgramObject %u: DoReflection - building reflection", m_externalIndex);
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
        if (!m_program->buildReflection(EShReflectionStrictArraySuffix | EShReflectionBasicArraySuffix |
                                        EShReflectionAllBlockVariables | EShReflectionSharedStd140UBO)) {
            m_linkStatus = false;
            m_infoLog = "Build reflection failed.";
            MGLOG_E("ProgramObject %u: DoReflection - buildReflection() returned false", m_externalIndex);
            return false;
        }

        // ---------- GL-facing index spaces (relaxed-parse cleanup) ----------
        // Blocks first: global-UBO membership drives the uniform filter below. The
        // synthesized MGL_GLOBAL_UBO is a transpiler artifact - its members are GL
        // default-block uniforms and the block itself must stay invisible to GL (it
        // did not exist in the GL-client parse this replaces).
        const Int tProgramBlockCount = m_program->getNumUniformBlocks();
        m_tProgramBlockIndexToGl.assign(tProgramBlockCount, -1);
        m_glBlockIndexToTProgram.clear();
        for (Int i = 0; i < tProgramBlockCount; i++) {
            const auto& ubo = m_program->getUniformBlock(i);
            if (std::strstr(ubo.name.c_str(), MG_Util::ShaderTranspiler::GLOBAL_UBO_NAME) != nullptr) {
                continue;
            }
            m_tProgramBlockIndexToGl[i] = static_cast<Int>(m_glBlockIndexToTProgram.size());
            m_glBlockIndexToTProgram.push_back(i);
        }

        // ------------ Uniforms (GL Plain) ----------------
        // The relaxed parse sweeps every DECLARED default-block uniform into
        // MGL_GLOBAL_UBO whether or not any stage reads it. GL requires a
        // declared-but-unreferenced default-block uniform to be inactive (absent from
        // glGetActiveUniform, glGetUniformLocation == -1): filter global-UBO members no
        // stage references. Named-block members keep GL's every-declared-member-is-active
        // semantics, exactly as before.
        const Int tProgramUniformCount = m_program->getNumUniformVariables();
        m_tProgramUniformIndexToGl.assign(tProgramUniformCount, -1);
        m_glUniformIndexToTProgram.clear();
        const auto isGlobalUboMember = [this](const glslang::TObjectReflection& uniform) {
            return uniform.index >= 0 && uniform.index < static_cast<Int>(m_tProgramBlockIndexToGl.size()) &&
                   m_tProgramBlockIndexToGl[uniform.index] < 0;
        };
        for (Int i = 0; i < tProgramUniformCount; i++) {
            const auto& uniform = m_program->getUniform(i);
            if (isGlobalUboMember(uniform) && uniform.stages == 0) {
                MGLOG_D("ProgramObject %u: Reflection - dead default-block uniform '%s' filtered from the GL "
                        "surface",
                        m_externalIndex, uniform.name.c_str());
                continue;
            }
            m_tProgramUniformIndexToGl[i] = static_cast<Int>(m_glUniformIndexToTProgram.size());
            m_glUniformIndexToTProgram.push_back(i);
        }
        m_activeUniformCount = static_cast<Uint>(m_glUniformIndexToTProgram.size());
        MGLOG_D("ProgramObject %u: Reflection - active uniform count = %d (of %d reflected)", m_externalIndex,
                m_activeUniformCount, tProgramUniformCount);

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
            auto it = m_linkedExplicitUniformLocations.find(reflectedName);
            if (it == m_linkedExplicitUniformLocations.end() && reflectedName.length() > 3 &&
                reflectedName.compare(reflectedName.length() - 3, 3, "[0]") == 0) {
                it = m_linkedExplicitUniformLocations.find(reflectedName.substr(0, reflectedName.length() - 3));
            }
            return it != m_linkedExplicitUniformLocations.end() ? &it->second : nullptr;
        };
        for (const Int i : m_glUniformIndexToTProgram) {
            const auto& uniform = m_program->getUniform(i);
            const glslang::TType* type = uniform.getType();
            const Bool inNamedBlock = uniform.index >= 0 && !isGlobalUboMember(uniform);
            if (inNamedBlock) continue; // block members never take glUniform locations

            if (const Int* explicitLocation = findExplicitLocation(uniform.name)) {
                effectiveLocation[i] = static_cast<Uint>(*explicitLocation);
                locationIsSourceExplicit[i] = true;
            } else if (!m_linkedExplicitUniformLocations.empty() &&
                       uniform.name.find('.') != String::npos) {
                // A struct uniform's explicit location spreads consecutively over its
                // flattened members ("s.a", "s[1].b", ...) in reflection order.
                const SizeT cut = uniform.name.find_first_of(".[");
                const auto rootIt = m_linkedExplicitUniformLocations.find(uniform.name.substr(0, cut));
                if (rootIt != m_linkedExplicitUniformLocations.end()) {
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
                m_infoLog = std::format("Uniform '{}' explicit location {} is out of range.", uniform.name,
                                        effectiveLocation[i]);
                ResetLinkArtifacts();
                return false;
            }
        }

        Int requiredUniformLocations = 0;
        for (const Int i : m_glUniformIndexToTProgram) {
            auto& uniform = m_program->getUniform(i);
            const Uint location = effectiveLocation[i];
            const Int locationSpan = GetUniformLocationSpan(uniform);
            requiredUniformLocations += locationSpan;
            if (location != kNoLocation) {
                m_maxUniformLocation = std::max(m_maxUniformLocation, location + locationSpan - 1);
            }
            m_uniformNameMaxLength = std::max(m_uniformNameMaxLength, (Int)uniform.name.length());
            m_uniformLocations[uniform.name] = location;
            MGLOG_D("ProgramObject %u: Reflection - uniform[%d] name='%s' effectiveLocation=%d", m_externalIndex,
                    i, uniform.name.c_str(), location);
        }

        MGLOG_D("ProgramObject %u: Reflection - computed m_maxUniformLocation=%u m_uniformNameMaxLength=%d",
                m_externalIndex, m_maxUniformLocation, m_uniformNameMaxLength);

        if (m_maxUniformLocation + 1 < requiredUniformLocations) {
            MGLOG_D("ProgramObject %u: Reflection - maxUniformLocation+1 (%u) < requiredUniformLocations (%d), "
                    "adjusting",
                    m_externalIndex, m_maxUniformLocation + 1, requiredUniformLocations);
            // This means we have fewer than enough gaps to fit
            // unallocated uniforms
            m_maxUniformLocation = requiredUniformLocations - 1;
        }

        // i-th elements refers to uniform at layout(location = i, ...)
        m_uniformIndexInTProgram.resize(m_maxUniformLocation + 1, glslang::TQualifier::layoutLocationEnd);
        m_uniformSamplerOrImageUnitIndex.resize(m_maxUniformLocation + 1, -1);

        Vector<int> unallocatedUniformIndex;

        // Pass 1: source-explicit locations. These are API contract
        // (ARB_explicit_uniform_location), and an overlap between distinct uniforms is a
        // link error - config A's mapIO rejected it ("Uniform location overlaps across
        // stages"); the relaxed parse dropped the qualifiers, so it is enforced here.
        for (const Int i : m_glUniformIndexToTProgram) {
            auto& uniform = m_program->getUniform(i);
            if (!locationIsSourceExplicit[i] || effectiveLocation[i] == kNoLocation) continue;
            const Uint location = effectiveLocation[i];
            const Int locationSpan = GetUniformLocationSpan(uniform);
            for (Int element = 0; element < locationSpan; ++element) {
                const Int existing = m_uniformIndexInTProgram[location + element];
                if (existing != glslang::TQualifier::layoutLocationEnd && existing != i) {
                    m_infoLog =
                        std::format("Uniform location overlap: '{}' and '{}' both occupy location {}.",
                                    m_program->getUniform(existing).name, uniform.name, location + element);
                    ResetLinkArtifacts();
                    return false;
                }
                m_uniformIndexInTProgram[location + element] = i;
            }
            MGLOG_D("ProgramObject %u: Reflection - assigned explicit-location uniform '%s' to locations "
                    "%u..%u (indexInTProgram=%d)",
                    m_externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, i);
        }

        // Pass 2: glslang-assigned locations (opaque uniforms under the relaxed parse).
        // Implementation-chosen, so on a collision with an explicit location the uniform
        // is demoted to the first-fit pass below instead of failing the link.
        for (const Int i : m_glUniformIndexToTProgram) {
            auto& uniform = m_program->getUniform(i);
            if (locationIsSourceExplicit[i]) continue;
            const Uint location = effectiveLocation[i];
            if (location == kNoLocation) {
                unallocatedUniformIndex.emplace_back(i);
                MGLOG_D("ProgramObject %u: Reflection - uniform '%s' is unallocated, will assign later",
                        m_externalIndex, uniform.name.c_str());
                continue; // will allocate unallocated uniforms later
            }
            const Int locationSpan = GetUniformLocationSpan(uniform);
            Bool spanIsFree = location + locationSpan - 1 <= m_maxUniformLocation;
            for (Int element = 0; spanIsFree && element < locationSpan; ++element) {
                spanIsFree =
                    m_uniformIndexInTProgram[location + element] == glslang::TQualifier::layoutLocationEnd;
            }
            if (!spanIsFree) {
                m_uniformLocations[uniform.name] = kNoLocation;
                unallocatedUniformIndex.emplace_back(i);
                MGLOG_D("ProgramObject %u: Reflection - uniform '%s' auto location %u collides with an "
                        "explicit location, demoting to first-fit",
                        m_externalIndex, uniform.name.c_str(), location);
                continue;
            }
            for (Int element = 0; element < locationSpan; ++element) {
                m_uniformIndexInTProgram[location + element] = i;
            }
            MGLOG_D("ProgramObject %u: Reflection - assigned uniform '%s' to locations %u..%u "
                    "(indexInTProgram=%d)",
                    m_externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, i);
        }

        SizeT locNeedle = 0;
        std::sort(unallocatedUniformIndex.begin(), unallocatedUniformIndex.end(), [this](Int lhs, Int rhs) {
            const auto& lhsUniform = m_program->getUniform(lhs);
            const auto& rhsUniform = m_program->getUniform(rhs);
            return lhsUniform.name < rhsUniform.name;
        });
        for (auto index : unallocatedUniformIndex) {
            auto& uniform = m_program->getUniform(index);
            const Int locationSpan = GetUniformLocationSpan(uniform);
            Bool placed = false;
            for (; locNeedle <= m_maxUniformLocation; locNeedle++) {
                bool hasRoom = locNeedle + locationSpan - 1 <= m_maxUniformLocation;
                for (Int element = 0; hasRoom && element < locationSpan; ++element) {
                    hasRoom = m_uniformIndexInTProgram[locNeedle + element] ==
                              glslang::TQualifier::layoutLocationEnd;
                }
                if (!hasRoom) continue;
                // Found a vacant location at locNeedle
                for (Int element = 0; element < locationSpan; ++element) {
                    m_uniformIndexInTProgram[locNeedle + element] = index;
                }
                m_uniformLocations[uniform.name] = locNeedle;
                MGLOG_D("ProgramObject %u: Reflection - assigned unallocated uniform '%s' to locations %zu..%zu "
                        "(index %d)",
                        m_externalIndex, uniform.name.c_str(), locNeedle, locNeedle + locationSpan - 1, index);
                locNeedle += locationSpan;
                placed = true;
                break;
            }
            if (!placed) {
                // Explicit-location uniforms can fragment the space so no contiguous
                // span is left; grow the table instead of leaving the uniform without
                // a location (which would make it unsettable via glUniform*).
                const SizeT base = m_uniformIndexInTProgram.size();
                m_uniformIndexInTProgram.resize(base + locationSpan, glslang::TQualifier::layoutLocationEnd);
                m_uniformSamplerOrImageUnitIndex.resize(base + locationSpan, -1);
                m_maxUniformLocation = static_cast<Uint>(base + locationSpan - 1);
                for (Int element = 0; element < locationSpan; ++element) {
                    m_uniformIndexInTProgram[base + element] = index;
                }
                m_uniformLocations[uniform.name] = static_cast<Uint>(base);
                MGLOG_D("ProgramObject %u: Reflection - grew location table to place uniform '%s' at %zu..%zu",
                        m_externalIndex, uniform.name.c_str(), base, base + locationSpan - 1);
                locNeedle = base + locationSpan;
            }
        }

        for (const Int i : m_glUniformIndexToTProgram) {
            auto& uniform = m_program->getUniform(i);
            const auto locationIt = m_uniformLocations.find(uniform.name);
            if (locationIt == m_uniformLocations.end()) {
                continue;
            }

            const Uint location = locationIt->second;
            if (location >= m_uniformSamplerOrImageUnitIndex.size() || uniform.getType() == nullptr ||
                !uniform.getType()->isOpaque() || (!uniform.getType()->isTexture() && !uniform.getType()->isImage())) {
                continue;
            }

            // Reflection names an array "texs[0]" while the layout(binding = N) map from the IO
            // resolver is keyed by the declared name ("texs"); look up both spellings.
            auto explicitBinding = m_explicitOpaqueUniformBindings.find(uniform.name);
            if (explicitBinding == m_explicitOpaqueUniformBindings.end() && uniform.name.length() > 3 &&
                uniform.name.compare(uniform.name.length() - 3, 3, "[0]") == 0) {
                explicitBinding =
                    m_explicitOpaqueUniformBindings.find(uniform.name.substr(0, uniform.name.length() - 3));
            }
            const int initialUnit =
                explicitBinding != m_explicitOpaqueUniformBindings.end() ? static_cast<int>(explicitBinding->second) : 0;
            const Int locationSpan = GetUniformLocationSpan(uniform);
            for (Int element = 0; element < locationSpan &&
                                  location + element < m_uniformSamplerOrImageUnitIndex.size(); ++element) {
                m_uniformSamplerOrImageUnitIndex[location + element] =
                    initialUnit + (explicitBinding != m_explicitOpaqueUniformBindings.end() ? element : 0);
            }
            MGLOG_D("ProgramObject %u: Reflection - opaque uniform '%s' locations=%u..%u initialUnit=%d",
                    m_externalIndex, uniform.name.c_str(), location, location + locationSpan - 1, initialUnit);
        }

        // ------------ attributes (vertex in) ---------------
        Int inCount = m_program->getNumPipeInputs();
        MGLOG_D("ProgramObject %u: Reflection - pipe input count (attributes) = %d", m_externalIndex, inCount);

        Int maxLoc = -1;
        for (int i = 0; i < inCount; ++i) {
            Int loc = (Int)m_program->getPipeInput(i).layoutLocation();
            if (loc >= 0 && loc != glslang::TQualifier::layoutLocationEnd) {
                const Int locationSpan = GetVertexInputLocationSpan(m_program->getPipeInput(i).glDefineType);
                maxLoc = std::max(maxLoc, loc + locationSpan - 1);
            }
            MGLOG_D("ProgramObject %u: Reflection - pipe input[%d] name='%s' layoutLocation=%d glType=%u",
                    m_externalIndex, i, m_program->getPipeInput(i).name.c_str(), loc,
                    m_program->getPipeInput(i).glDefineType);
        }

        if (maxLoc < 0) {
            maxLoc = std::max(0, inCount - 1);
        }

        const GLint maxAttribs = GetReflectionVertexAttribLimit();
        MGLOG_D("ProgramObject %u: Reflection - computed maxLoc=%d, using maxAttribs=%d", m_externalIndex, maxLoc,
                maxAttribs);

        if (maxLoc >= maxAttribs) {
            MGLOG_W("ProgramObject %u: ProgramObject::DoReflection - required attrib location %d >= "
                    "GL_MAX_VERTEX_ATTRIBS (%d). Clamping.",
                    m_externalIndex, maxLoc, maxAttribs);
            maxLoc = maxAttribs - 1;
        }

        m_attribs.resize(maxLoc + 1);
        m_attribTypes.resize(maxLoc + 1);

        for (int i = 0; i < inCount; ++i) {
            auto& inVar = m_program->getPipeInput(i);
            Int location = (Int)inVar.layoutLocation();
            // Builtins reflect under their SPIR-V names here; GL_ACTIVE_ATTRIBUTE_MAX_LENGTH
            // must measure the GL spelling glGetActiveAttrib will report.
            m_attribInNameMaxLength =
                std::max(m_attribInNameMaxLength, (Int)NormalizeBuiltinPipeInputName(inVar.name).length());

            if (location >= 0 && location < (int)m_attribs.size()) {
                const Int locationSpan = GetVertexInputLocationSpan(inVar.glDefineType);
                const GLenum locationType = GetVertexInputLocationType(inVar.glDefineType);
                for (Int locationOffset = 0; locationOffset < locationSpan; ++locationOffset) {
                    const Int expandedLocation = location + locationOffset;
                    if (expandedLocation < 0 || expandedLocation >= static_cast<Int>(m_attribs.size())) {
                        break;
                    }

                    m_attribs[expandedLocation] = inVar.name;
                    m_attribTypes[expandedLocation] = locationType;
                    MGLOG_D(
                        "ProgramObject %u: Reflection - got attrib '%s' at expanded location %d (baseLocation=%d glType=%u expandedType=%u)",
                        m_externalIndex,
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
        const Int uboCount = GetActiveUniformBlocksCount();
        MGLOG_D("ProgramObject %u: Reflection - uniform block count (UBO) = %d", m_externalIndex, uboCount);
        m_uniformBlockBinding.resize(uboCount, -1);
        for (Int i = 0; i < uboCount; i++) {
            auto& ubo = m_program->getUniformBlock(m_glBlockIndexToTProgram[i]);
            m_uniformBlockNameMaxLength = std::max(m_uniformBlockNameMaxLength, (Int)ubo.name.length());
            m_uniformBlockIndexByName[ubo.name] = i;
            // if there's binding defined in shader as layout(binding = ...),
            // retrieve it here
            m_uniformBlockBinding[i] = ubo.getBinding();
            MGLOG_D("ProgramObject %u: Reflection - UBO[%d] name='%s' size=%u binding=%d", m_externalIndex, i,
                    ubo.name.c_str(), ubo.size, ubo.getBinding());
        }
        return true;
    }

    void ProgramObject::GenerateSpirv() {
        /* As we passed first stage compilation/linking,
         * we'll assume all the operations here should
         * pass. We may be able to employ some optimizations
         * here without the burden of error reporting.
         */
        using namespace MG_Util::ShaderTranspiler;
        MGLOG_D("ProgramObject %u: GenerateSpirv - start", m_externalIndex);

        // The shaders were parsed once, in the link-compatible (relaxed Vulkan-rules)
        // configuration, and m_program linked those parses - so m_program IS the
        // program the backends consume. Generate SPIR-V straight from its
        // intermediates; the full re-parse + re-link that used to live here (one
        // glslang pass per shader per link) is gone.
        Vector<GLenum> shaderTypes(m_shaders.size());
        for (SizeT i = 0; i < m_shaders.size(); i++) {
            shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(m_shaders[i]->GetShaderStage());
        }

        ProgramBinaryAttrib binaryAttrib{
            .shaderTypes = shaderTypes,
            .program = *m_program,
        };
        MGLOG_D("ProgramObject %u: GenerateSpirv - requesting SPIR-V binary from program", m_externalIndex);
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult) {
            MGLOG_E("ProgramObject %u: GenerateSpirv - GetSpirvBinaryFromProgram failed", m_externalIndex);
        }
        MOBILEGL_ASSERT(binaryResult, "GetSpirvBinaryFromProgram failed");
        m_generatedSpirv = Move(binaryResult.value());
        MGLOG_D("ProgramObject %u: GenerateSpirv - generated %zu SPIR-V modules", m_externalIndex,
                m_generatedSpirv.size());

        // Linked SPIR-V generated, sanitize and optimize it
        for (auto& spv : m_generatedSpirv) {
            auto success = ShaderCompiler::SanitizeAndOptimizeBinary(spv, spv);
            MOBILEGL_ASSERT(success, "SanitizeBinary failed");
        }
    }

    void ProgramObject::BuildGlobalUboRouting() {
        using namespace MG_Util::ShaderTranspiler;
        Vector<GLenum> shaderTypes(m_shaders.size());
        for (SizeT i = 0; i < m_shaders.size(); i++) {
            shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(m_shaders[i]->GetShaderStage());
        }

        m_uniformSizesInBytes.clear();
        m_uniformOffsets.clear();
        m_globalUboScratch.clear();
        // kInvalidUniformOffset marks locations that end up without global-UBO backing
        // (e.g. the optimizer eliminated every use of the uniform); the fallback pass
        // below gives those locations tail storage so glUniform* always has a target.
        m_uniformOffsets.resize(m_maxUniformLocation + 1, kInvalidUniformOffset);
        m_uniformSizesInBytes.resize(m_maxUniformLocation + 1, 0);
        for (SizeT i = 0; i < m_generatedSpirv.size(); i++) {
            auto& spv = m_generatedSpirv[i];

            auto shaderType = shaderTypes[i];
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - parsing SPIR-V meta data for module %zu "
                    "(shaderType=%u, wordCount=%zu)",
                    m_externalIndex, i, shaderType, spv.size());
            SpvcSession session(spv, SessionUsageBit::Reflection);
            auto result = session.ParseMetaData();
            if (result < 0) {
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SpvcSession::ParseMetaData failed for module %zu, "
                        "err = %d%s",
                        m_externalIndex, i, result,
                        (result == SPVC_ERROR_INVALID_SPIRV ? ". Probably no global UBO?" : ""));
                continue;
            } else {
                auto& meta = session.GetMetadata();
                auto size = meta.globalUboSize;
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SPIR-V meta: uboSize=%zu plainUniformCount=%zu "
                        "plainUniformOffsets=%zu",
                        m_externalIndex, meta.globalUboSize, meta.plainUniformMemberSizesInBytes.size(),
                        meta.plainUniformOffsetsInUBO.size());
                if (size == 0) {
                    continue;
                }
                if (m_globalUboScratch.size() < size) {
                    m_globalUboScratch.resize(size);
                }
                for (const auto& [name, offset] : meta.plainUniformOffsetsInUBO) {
                    // SPIRV-Reflect leaf names never carry a "[0]" suffix; frontend
                    // reflection keys arrays as "arr[0]" (GL naming), so retry with the
                    // suffix before declaring the uniform unbacked.
                    auto locationIt = m_uniformLocations.find(name);
                    if (locationIt == m_uniformLocations.end()) {
                        locationIt = m_uniformLocations.find(name + "[0]");
                    }
                    if (locationIt == m_uniformLocations.end()) {
                        MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u but not found in "
                                "m_uniformLocations",
                                m_externalIndex, name.c_str(), offset);
                        continue;
                    }
                    const Uint baseLocation = locationIt->second;
                    if (!IsValidUniformLocation(static_cast<Int>(baseLocation))) {
                        continue;
                    }

                    const Int uniformIndex = m_uniformIndexInTProgram[baseLocation];
                    const GLint arraySize = GetUniformArraySizeByTIndex(uniformIndex);
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
                        if (location > m_maxUniformLocation || m_uniformIndexInTProgram[location] != uniformIndex) {
                            break;
                        }
                        m_uniformOffsets[location] = offset + static_cast<Uint>(element) * arrayStride;
                        const SizeT consumed = static_cast<SizeT>(element) * arrayStride;
                        m_uniformSizesInBytes[location] = memberSize > consumed ? memberSize - consumed : 0;
                    }
                    MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u stride=%u size=%zu assigned "
                            "to locations %u..%u",
                            m_externalIndex, name.c_str(), offset, arrayStride, memberSize, baseLocation,
                            baseLocation + static_cast<Uint>(elementCount) - 1);
                }
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - finished parsing module %zu metadata",
                        m_externalIndex, i);
            }
        }

        // Fallback pass: a linked program's active non-opaque uniforms must accept
        // glUniform*/glGetUniform* even when the optimized SPIR-V no longer contains
        // them (AggressiveDCE can remove a dead loop together with the only loads of a
        // uniform -- or the entire global UBO, leaving the scratch unallocated). Hand
        // such locations CPU-side storage at the (16-byte aligned) tail of the shadow
        // buffer; backends bind at least the SPIR-V-declared UBO range, and the GPU
        // never reads these bytes, so this only keeps the GL-visible state coherent.
        for (Uint location = 0; location <= m_maxUniformLocation; ++location) {
            if (m_uniformOffsets[location] != kInvalidUniformOffset) continue;
            if (!IsValidUniformLocation(static_cast<Int>(location))) continue;
            const auto& uniform = m_program->getUniform(m_uniformIndexInTProgram[location]);
            const glslang::TType* type = uniform.getType();
            if (type != nullptr && type->isOpaque()) continue;
            if (uniform.index >= 0 && uniform.index < m_program->getNumUniformBlocks() &&
                std::strstr(m_program->getUniformBlock(uniform.index).name.c_str(),
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
            const SizeT slotOffset = (m_globalUboScratch.size() + 15u) & ~static_cast<SizeT>(15u);
            m_globalUboScratch.resize(slotOffset + slotSize, 0);
            m_uniformOffsets[location] = static_cast<Uint>(slotOffset);
            m_uniformSizesInBytes[location] = slotSize;
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' location %u has no UBO backing in the "
                    "generated SPIR-V (optimized out?); allocated %zu fallback bytes at scratch offset %zu",
                    m_externalIndex, uniform.name.c_str(), location, slotSize, slotOffset);
        }
    }

    void ProgramObject::WaitUntilGenerationCompleted() const {
        MGLOG_D("ProgramObject %u: WaitUntilGenerationCompleted called (no-op)", m_externalIndex);
        // currently no-op, but keep log for debugging
        // will probably be useful when multi-threaded compilation
    }

    void ProgramObject::SetExplicitVertexInLocation(Uint index, const char* name) {
        MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation called name='%s' index=%u", m_externalIndex, name,
                index);
        m_explicitAttribLocations[name] = index;
        MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation - stored explicit location for '%s' -> %u",
                m_externalIndex, name, index);
    }

    void ProgramObject::SetExplicitFragmentOutLocation(Uint index, const char* name) {
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation called name='%s' index=%u", m_externalIndex, name,
                index);
        m_explicitFragDataLocation[name] = index;
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation - stored explicit location for '%s' -> %u",
                m_externalIndex, name, index);
    }

    void ProgramObject::SetExplicitFragmentOutIndex(Uint colorIndex, const char* name) {
        m_explicitFragDataIndex[name] = colorIndex;
        MGLOG_D("ProgramObject %u: SetExplicitFragmentOutIndex - stored color index for '%s' -> %u", m_externalIndex,
                name, colorIndex);
    }

    Bool ProgramObject::ValidateFragmentOutputLocations() {
        if (!m_program) return false;

        UnorderedMap<Int, String> colorNumberOwners;
        const Int outputCount = m_program->getNumPipeOutputs();
        for (Int index = 0; index < outputCount; ++index) {
            const auto& output = m_program->getPipeOutput(index);
            if (IsBuiltInPipelineOutput(output)) {
                continue;
            }

            const String outputName = StripArrayElementSuffix(output.name);
            const auto explicitLocation = m_explicitFragDataLocation.find(outputName);
            const Int location = explicitLocation != m_explicitFragDataLocation.end()
                                     ? static_cast<Int>(explicitLocation->second)
                                     : static_cast<Int>(output.layoutLocation());
            const Int span = std::max<Int>(output.size, 1);

            if (location < 0 || location + span > m_maxFragmentOutputColorNumber) {
                m_infoLog = std::format("Fragment output '{}' location range [{}, {}) exceeds GL_MAX_DRAW_BUFFERS {}.",
                                        outputName, location, location + span, m_maxFragmentOutputColorNumber);
                MGLOG_E("ProgramObject %u: Link failed - %s", m_externalIndex, m_infoLog.c_str());
                ResetLinkArtifacts();
                return false;
            }

            for (Int colorNumber = location; colorNumber < location + span; ++colorNumber) {
                auto [owner, inserted] = colorNumberOwners.emplace(colorNumber, outputName);
                if (!inserted) {
                    m_infoLog = std::format("Fragment outputs '{}' and '{}' alias color number {}.",
                                            owner->second, outputName, colorNumber);
                    MGLOG_E("ProgramObject %u: Link failed - %s", m_externalIndex, m_infoLog.c_str());
                    ResetLinkArtifacts();
                    return false;
                }
            }
        }

        return true;
    }

    Int ProgramObject::GetFragmentDataLocation(const char* name) {
        if (!m_program || !name) return -1;

        const auto explicitLocation = m_linkedFragDataLocation.find(name);
        const Int outputCount = m_program->getNumPipeOutputs();
        for (Int index = 0; index < outputCount; ++index) {
            const auto& output = m_program->getPipeOutput(index);
            if (output.name != name) continue;
            if (explicitLocation != m_linkedFragDataLocation.end()) return static_cast<Int>(explicitLocation->second);
            return static_cast<Int>(output.layoutLocation());
        }
        return -1;
    }

    Int ProgramObject::GetFragmentDataIndex(const char* name) {
        // Only an active user-defined fragment output has an index; reuse the location lookup to test
        // that. The color index defaults to 0 unless glBindFragDataLocationIndexed bound it to 1.
        // (Shader-side layout(index = ...) qualifiers are not reflected here, only API bindings.)
        if (GetFragmentDataLocation(name) < 0) return -1;
        const auto it = m_linkedFragDataIndex.find(name);
        return it != m_linkedFragDataIndex.end() ? static_cast<Int>(it->second) : 0;
    }
} // namespace MobileGL::MG_State::GLState
