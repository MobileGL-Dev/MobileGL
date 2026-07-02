// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramObject.h"
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
    static MobileGL::String StripArrayElementSuffix(const MobileGL::String& name) {
        const MobileGL::SizeT bracket = name.find('[');
        return bracket == MobileGL::String::npos ? name : name.substr(0, bracket);
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
    void ProgramObject::ResetLinkArtifacts() {
        m_program.reset();
        m_generatedSpirv.clear();
        m_uniformLocations.clear();
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
        m_linkStatus = false;
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
            shaders[i] = m_shaders[i]->GetCompiledShader();
            MGLOG_D("ProgramObject %u: shader[%zu] compiled shader ptr %p, src len %zu", m_externalIndex, i,
                    shaders[i].get(), m_shaders[i]->GetShaderSource().length());
            MGLOG_D("ProgramObject %u: shader[%zu] source:\n%s", m_externalIndex, i,
                    m_shaders[i]->GetShaderSource().c_str());
        }

        MG_Util::ShaderTranspiler::ProgramAttrib attrib{.shaders = Move(shaders),
                                                        .explicitVertexInLocations = m_explicitAttribLocations,
                                                        .explicitFragmentOutLocations = m_explicitFragDataLocation,
                                                        .explicitOpaqueUniformBindings =
                                                            &m_explicitOpaqueUniformBindings};

        MGLOG_D("ProgramObject %u: Calling ShaderCompiler::LinkProgram", m_externalIndex);
        auto result = MG_Util::ShaderTranspiler::ShaderCompiler::LinkProgram(attrib);
        if (result) {
            m_linkStatus = true;
            m_program = result.value();
            m_linkedFragDataLocation = m_explicitFragDataLocation;
            MGLOG_D("ProgramObject %u: LinkProgram succeeded, TProgram ptr %p", m_externalIndex, m_program.get());
        } else {
            m_infoLog = result.error().log;
            MGLOG_E("ProgramObject %u: LinkProgram failed. InfoLog:\n%s", m_externalIndex, m_infoLog.c_str());
            return;
        }

        MGLOG_D("ProgramObject %u: Starting reflection", m_externalIndex);
        DoReflection();
        MGLOG_D("ProgramObject %u: Reflection done (linkStatus=%d)", m_externalIndex, (int)m_linkStatus);
        if (!ValidateFragmentOutputLocations()) {
            return;
        }

        MGLOG_D("ProgramObject %u: Starting binary generation", m_externalIndex);
        GenerateBinary();
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

    void ProgramObject::DoReflection() {
        if (!m_program) {
            MGLOG_E("ProgramObject %u: DoReflection called but m_program is null", m_externalIndex);
            m_linkStatus = false;
            m_infoLog = "DoReflection failed: no program.";
            return;
        }

        MGLOG_D("ProgramObject %u: DoReflection - building reflection", m_externalIndex);
        if (!m_program->buildReflection()) {
            m_linkStatus = false;
            m_infoLog = "Build reflection failed.";
            MGLOG_E("ProgramObject %u: DoReflection - buildReflection() returned false", m_externalIndex);
            return;
        }

        // ------------ Uniforms (GL Plain) ----------------
        // Allocate uniform locations
        m_activeUniformCount = m_program->getNumUniformVariables();
        MGLOG_D("ProgramObject %u: Reflection - active uniform count = %d", m_externalIndex, m_activeUniformCount);
        for (int i = 0; i < m_activeUniformCount; i++) {
            auto& uniform = m_program->getUniform(i);
            auto location = uniform.layoutLocation();
            if (location != glslang::TQualifier::layoutLocationEnd) {
                m_maxUniformLocation = std::max(m_maxUniformLocation, location);
            }
            m_uniformNameMaxLength = std::max(m_uniformNameMaxLength, (Int)uniform.name.length());
            m_uniformLocations[uniform.name] = location;
            MGLOG_D("ProgramObject %u: Reflection - uniform[%d] name='%s' layoutLocation=%d", m_externalIndex, i,
                    uniform.name.c_str(), location);
        }

        MGLOG_D("ProgramObject %u: Reflection - computed m_maxUniformLocation=%u m_uniformNameMaxLength=%d",
                m_externalIndex, m_maxUniformLocation, m_uniformNameMaxLength);

        if (m_maxUniformLocation + 1 < m_activeUniformCount) {
            MGLOG_D("ProgramObject %u: Reflection - maxUniformLocation+1 (%u) < activeUniformCount (%d), "
                    "adjusting",
                    m_externalIndex, m_maxUniformLocation + 1, m_activeUniformCount);
            // This means we have fewer than enough gaps to fit
            // unallocated uniforms
            m_maxUniformLocation = m_activeUniformCount;
        }

        // i-th elements refers to uniform at layout(location = i, ...)
        m_uniformIndexInTProgram.resize(m_maxUniformLocation + 1, glslang::TQualifier::layoutLocationEnd);
        m_uniformSamplerOrImageUnitIndex.resize(m_maxUniformLocation + 1, -1);

        Vector<int> unallocatedUniformIndex;

        // Populate vector with already allocated location
        for (int i = 0; i < m_activeUniformCount; i++) {
            auto& uniform = m_program->getUniform(i);
            auto location = uniform.layoutLocation();
            if (m_uniformLocations[uniform.name] == glslang::TQualifier::layoutLocationEnd) {
                unallocatedUniformIndex.emplace_back(i);
                MGLOG_D("ProgramObject %u: Reflection - uniform '%s' is unallocated, will assign later",
                        m_externalIndex, uniform.name.c_str());
                continue; // will allocate unallocated uniforms later
            }
            m_uniformIndexInTProgram[location] = i;
            MGLOG_D("ProgramObject %u: Reflection - assigned uniform '%s' to location %d (indexInTProgram=%d)",
                    m_externalIndex, uniform.name.c_str(), location, i);
        }

        SizeT locNeedle = 0;
        std::sort(unallocatedUniformIndex.begin(), unallocatedUniformIndex.end(), [this](Int lhs, Int rhs) {
            const auto& lhsUniform = m_program->getUniform(lhs);
            const auto& rhsUniform = m_program->getUniform(rhs);
            return lhsUniform.name < rhsUniform.name;
        });
        for (auto index : unallocatedUniformIndex) {
            auto& uniform = m_program->getUniform(index);
            for (; locNeedle <= m_maxUniformLocation; locNeedle++) {
                if (m_uniformIndexInTProgram[locNeedle] != glslang::TQualifier::layoutLocationEnd) continue;
                // Found a vacant location at locNeedle
                m_uniformIndexInTProgram[locNeedle] = index;
                m_uniformLocations[uniform.name] = locNeedle;
                MGLOG_D("ProgramObject %u: Reflection - assigned unallocated uniform '%s' to location %zu "
                        "(index %d)",
                        m_externalIndex, uniform.name.c_str(), locNeedle, index);
                locNeedle++;
                break;
            }
        }

        for (int i = 0; i < m_activeUniformCount; i++) {
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

            const auto explicitBinding = m_explicitOpaqueUniformBindings.find(uniform.name);
            const int initialUnit =
                explicitBinding != m_explicitOpaqueUniformBindings.end() ? static_cast<int>(explicitBinding->second) : 0;
            m_uniformSamplerOrImageUnitIndex[location] = initialUnit;
            MGLOG_D("ProgramObject %u: Reflection - opaque uniform '%s' location=%u initialUnit=%d",
                    m_externalIndex, uniform.name.c_str(), location, initialUnit);
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

        GLint maxAttribs = 16; // TODO: get from backend
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
            m_attribInNameMaxLength = std::max(m_attribInNameMaxLength, (Int)inVar.name.length());

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
        Int uboCount = m_program->getNumUniformBlocks();
        MGLOG_D("ProgramObject %u: Reflection - uniform block count (UBO) = %d", m_externalIndex, uboCount);
        m_uniformBlockBinding.resize(uboCount, -1);
        for (int i = 0; i < uboCount; i++) {
            auto& ubo = m_program->getUniformBlock(i);
            m_uniformBlockNameMaxLength = std::max(m_uniformBlockNameMaxLength, (Int)ubo.name.length());
            m_uniformBlockIndexByName[ubo.name] = i;
            // if there's binding defined in shader as layout(binding = ...),
            // retrieve it here
            m_uniformBlockBinding[i] = ubo.getBinding();
            MGLOG_D("ProgramObject %u: Reflection - UBO[%d] name='%s' size=%u binding=%d", m_externalIndex, i,
                    ubo.name.c_str(), ubo.size, ubo.getBinding());
        }
    }

    void ProgramObject::GenerateBinary() {
        /* As we passed first stage compilation/linking,
         * we'll assume all the operations here should
         * pass. We may be able to employ some optimizations
         * here without the burden of error reporting.
         */
        using namespace MG_Util::ShaderTranspiler;
        MGLOG_D("ProgramObject %u: GenerateBinary - start", m_externalIndex);
        Vector<SharedPtr<glslang::TShader>> shaders(m_shaders.size());
        Vector<GLenum> shaderTypes(m_shaders.size());

        // 1. Compile shaders
        for (SizeT i = 0; i < m_shaders.size(); i++) {
            auto shaderStage = m_shaders[i]->GetShaderStage();
            auto shaderType = MG_Util::ConvertShaderStageToGLEnum(shaderStage);
            String compileSource = m_shaders[i]->GetShaderSource();
            PreprocessShaderSource(shaderStage, compileSource);
            shaderTypes[i] = shaderType;
            ShaderAttrib attrib{.shaderType = shaderType,
                                .sourceStr = compileSource,
                                .flags = 0}; // Will need patched glslang to work
            MGLOG_D("ProgramObject %u: GenerateBinary - compiling shader[%zu] type %u", m_externalIndex, i, shaderType);
            auto res = ShaderCompiler::CompileShader(attrib);
            if (!res) {
                MGLOG_E("ProgramObject %u: GenerateBinary - CompileShader failed for shader[%zu], aborting "
                        "binary generation",
                        m_externalIndex, i);
                MGLOG_E("ProgramObject %u: GenerateBinary - CompileShader return code %d, log:\n%s", m_externalIndex,
                        res.error().errc, res.error().log.c_str());
                MGLOG_E("ProgramObject %u: GenerateBinary - last compiled shader src: \n%s", m_externalIndex,
                        compileSource.c_str());
            }
            MOBILEGL_ASSERT(res, "CompileShader failed during binary generation");
            shaders[i] = res.value();
            MGLOG_D("ProgramObject %u: GenerateBinary - compiled shader[%zu] -> TShader ptr %p", m_externalIndex, i,
                    shaders[i].get());
        }

        // 2. Do actual linking
        ProgramAttrib attrib{.shaders = Move(shaders),
                             .explicitVertexInLocations = m_explicitAttribLocations,
                             .explicitFragmentOutLocations = m_explicitFragDataLocation,
                             .explicitOpaqueUniformBindings = &m_explicitOpaqueUniformBindings};
        MGLOG_D("ProgramObject %u: GenerateBinary - linking program for binary", m_externalIndex);
        auto programResult = ShaderCompiler::LinkProgram(attrib);
        if (!programResult) {
            MGLOG_E("ProgramObject %u: GenerateBinary - LinkProgram failed during binary generation", m_externalIndex);
        }
        MOBILEGL_ASSERT(programResult, "LinkProgram failed during binary generation");
        auto& program = programResult.value();
        MGLOG_D("ProgramObject %u: GenerateBinary - got linked program object", m_externalIndex);

        ProgramBinaryAttrib binaryAttrib{
            .shaderTypes = shaderTypes,
            .program = *program,
        };
        MGLOG_D("ProgramObject %u: GenerateBinary - requesting SPIR-V binary from program", m_externalIndex);
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult) {
            MGLOG_E("ProgramObject %u: GenerateBinary - GetSpirvBinaryFromProgram failed", m_externalIndex);
        }
        MOBILEGL_ASSERT(binaryResult, "GetSpirvBinaryFromProgram failed");
        m_generatedSpirv = Move(binaryResult.value());
        MGLOG_D("ProgramObject %u: GenerateBinary - generated %zu SPIR-V modules", m_externalIndex,
                m_generatedSpirv.size());

        // 3. Linked SPIR-V generated, sanitize and optimize it
        for (auto& spv : m_generatedSpirv) {
            auto success = ShaderCompiler::SanitizeAndOptimizeBinary(spv, spv);
            MOBILEGL_ASSERT(success, "SanitizeBinary failed");
        }

        // 4. Do reflection (find global UBO etc.)
        m_uniformSizesInBytes.clear();
        m_uniformOffsets.clear();
        m_globalUboScratch.clear();
        m_uniformOffsets.resize(m_maxUniformLocation + 1);
        m_uniformSizesInBytes.resize(m_maxUniformLocation + 1);
        for (SizeT i = 0; i < m_generatedSpirv.size(); i++) {
            auto& spv = m_generatedSpirv[i];

            auto shaderType = shaderTypes[i];
            MGLOG_D("ProgramObject %u: GenerateBinary - parsing SPIR-V meta data for module %zu "
                    "(shaderType=%u, wordCount=%zu)",
                    m_externalIndex, i, shaderType, spv.size());
            SpvcSession session(spv, SessionUsageBit::Reflection);
            auto result = session.ParseMetaData();
            if (result < 0) {
                MGLOG_D("ProgramObject %u: GenerateBinary - SpvcSession::ParseMetaData failed for module %zu, "
                        "err = %d%s",
                        m_externalIndex, i, result,
                        (result == SPVC_ERROR_INVALID_SPIRV ? ". Probably no global UBO?" : ""));
                continue;
            } else {
                auto& meta = session.GetMetadata();
                auto size = meta.globalUboSize;
                MGLOG_D("ProgramObject %u: GenerateBinary - SPIR-V meta: uboSize=%zu plainUniformCount=%zu "
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
                    if (m_uniformLocations.find(name) != m_uniformLocations.end()) {
                        m_uniformOffsets[m_uniformLocations[name]] = offset;
                        MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' offset=%u assigned to location %u",
                                m_externalIndex, name.c_str(), offset, m_uniformLocations[name]);
            } else {
                MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' offset=%u but not found in "
                        "m_uniformLocations",
                        m_externalIndex, name.c_str(), offset);
            }
                }
                for (const auto& [name, size] : meta.plainUniformMemberSizesInBytes) {
                    if (m_uniformLocations.find(name) != m_uniformLocations.end()) {
                        m_uniformSizesInBytes[m_uniformLocations[name]] = size;
                        MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' size=%u assigned to location %u",
                                m_externalIndex, name.c_str(), size, m_uniformLocations[name]);
            } else {
                MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' size=%u but not found in "
                        "m_uniformLocations",
                        m_externalIndex, name.c_str(), size);
            }
                }
                MGLOG_D("ProgramObject %u: GenerateBinary - finished parsing module %zu metadata",
                        m_externalIndex, i);
            }
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

    Bool ProgramObject::ValidateFragmentOutputLocations() {
        if (!m_program) return false;

        UnorderedMap<Int, String> colorNumberOwners;
        const Int outputCount = m_program->getNumPipeOutputs();
        for (Int index = 0; index < outputCount; ++index) {
            const auto& output = m_program->getPipeOutput(index);
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
} // namespace MobileGL::MG_State::GLState
