// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramObject.h"
#include "MG_Util/Converters/GLToStr/GLEnumConverter.h"
#include "MG_Util/ShaderTranspiler/Types.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>

const char* kDefaultFragmentShaderSource = R"(#version 460 core
layout(location = 0) out vec4 FragColor;
void main() {}
)";

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            bool ProgramObject::ShaderIsAttached(SharedPtr<ShaderObject> shader) {
                MGLOG_D("ProgramObject %u: ShaderIsAttached check for shader %p", m_externalIndex, shader.get());
                auto it = std::find_if(m_shaders.begin(), m_shaders.end(),
                                       [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });
                bool attached = it != m_shaders.end();
                MGLOG_D("ProgramObject %u: ShaderIsAttached -> %s", m_externalIndex, attached ? "true" : "false");
                return attached;
            }

            bool ProgramObject::AttachShader(SharedPtr<ShaderObject> shader) {
                MGLOG_D("ProgramObject %u: AttachShader called for shader %p", m_externalIndex, shader.get());
                if (ShaderIsAttached(shader)) {
                    MGLOG_D("ProgramObject %u: AttachShader - shader already attached, skipping", m_externalIndex);
                    return false;
                }
                m_shaders.emplace_back(shader);
                MGLOG_D("ProgramObject %u: AttachShader - attached successfully, total shaders now %zu",
                        m_externalIndex, m_shaders.size());
                return true;
            }

            SizeT ProgramObject::DetachShader(SharedPtr<ShaderObject> shader) {
                MGLOG_D("DetachShader called for shader %p from ProgramObject %u", shader.get(), m_externalIndex);
                if (!ShaderIsAttached(shader)) {
                    MGLOG_D("Shader %p is not attached to ProgramObject %u, cannot detach.", shader.get(),
                            m_externalIndex);
                    return 0;
                }
                m_detachedShaders.push_back(shader);
                MGLOG_D("Shader %p marked for detachment from ProgramObject %u", shader.get(), m_externalIndex);
                return 1;
            }

            SizeT ProgramObject::RemoveShader(SharedPtr<ShaderObject> shader) {
                MGLOG_D("ProgramObject %u: RemoveShader called for shader %p", m_externalIndex, shader.get());
                auto count = std::erase_if(
                    m_shaders, [shader](const SharedPtr<ShaderObject>& s) { return s.get() == shader.get(); });

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

                MGLOG_D("ProgramObject %u: No fragment shader attached, adding default fragment shader.",
                        m_externalIndex);
                SharedPtr<ShaderObject> defaultFS = MakeShared<ShaderObject>(ShaderStage::Fragment, 0);
                defaultFS->SetShaderSource(kDefaultFragmentShaderSource);
                defaultFS->Compile(); // TODO: use a global default FS object.
                auto status = defaultFS->GetCompileStatus();
                if (!status) {
                    MGLOG_E("ProgramObject %u: Failed to compile default fragment shader. InfoLog:\n%s",
                            m_externalIndex, defaultFS->GetInfoLog().c_str());
                    return;
                }
                m_shaders.push_back(defaultFS);
                MGLOG_D("ProgramObject %u: Default fragment shader added.", m_externalIndex);
            }

            void ProgramObject::Link(Bool addDefaultFSIfMissingForRenderingPipelineProgram) {
                MGLOG_D("ProgramObject %u: Link start, shaders to link: %zu", m_externalIndex, m_shaders.size());
                // Remove detached shaders first
                for (const auto& detachedShader : m_detachedShaders) {
                    RemoveShader(detachedShader);
                }
                m_detachedShaders.clear();

                Vector<GLenum> shaderTypes(m_shaders.size());
                Vector<SharedPtr<glslang::TShader>> shaders(m_shaders.size());

                if (addDefaultFSIfMissingForRenderingPipelineProgram) {
                    AddDefaultFragmentShaderIfMissing();
                }

                std::sort(m_shaders.begin(), m_shaders.end(),
                          [](SharedPtr<ShaderObject>& a, SharedPtr<ShaderObject>& b) {
                              return a->GetShaderStage() < b->GetShaderStage();
                          });

                for (SizeT i = 0; i < m_shaders.size(); i++) {
                    shaderTypes[i] = MG_Util::ConvertShaderStageToGLEnum(m_shaders[i]->GetShaderStage());
                    MGLOG_D("ProgramObject %u: Preparing shader[%zu] stage %s at %p", m_externalIndex, i,
                            MG_Util::ConvertGLEnumToString(shaderTypes[i]).c_str(), m_shaders[i].get());

                    if (!m_shaders[i]->GetCompileStatus()) {
                        m_infoLog =
                            std::format("Linking a {} with compilation error, linking will now terminate. Shader error "
                                        "log:\n{}\nShader src:\n{}",
                                        MG_Util::ConvertGLEnumToString(shaderTypes[i]).c_str(),
                                        m_shaders[i]->GetInfoLog().c_str(), m_shaders[i]->GetShaderSource().c_str());
                        m_linkStatus = false;
                        MGLOG_E("ProgramObject %u: Link failed - shader[%zu] compile status false. InfoLog:\n%s",
                                m_externalIndex, i, m_infoLog.c_str());
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
                                                                .explicitFragmentOutLocations =
                                                                    m_explicitFragDataLocation};

                MGLOG_D("ProgramObject %u: Calling ShaderCompiler::LinkProgram", m_externalIndex);
                auto result = MG_Util::ShaderTranspiler::ShaderCompiler::LinkProgram(attrib);
                if (result) {
                    m_linkStatus = true;
                    m_program = result.value();
                    MGLOG_D("ProgramObject %u: LinkProgram succeeded, TProgram ptr %p", m_externalIndex,
                            m_program.get());
                } else {
                    m_linkStatus = false;
                    m_infoLog = result.error().log;
                    MGLOG_E("ProgramObject %u: LinkProgram failed. InfoLog:\n%s", m_externalIndex, m_infoLog.c_str());
                }

                MGLOG_D("ProgramObject %u: Starting reflection", m_externalIndex);
                DoReflection();
                MGLOG_D("ProgramObject %u: Reflection done (linkStatus=%d)", m_externalIndex, (int)m_linkStatus);

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
                MGLOG_D("ProgramObject %u: Reflection - active uniform count = %d", m_externalIndex,
                        m_activeUniformCount);
                for (int i = 0; i < m_activeUniformCount; i++) {
                    auto& uniform = m_program->getUniform(i);
                    auto location = uniform.layoutLocation();
                    if (location != glslang::TQualifier::layoutLocationEnd) {
                        m_maxUniformLocation = std::max(m_maxUniformLocation, location);
                    }
                    m_uniformNameMaxLength = std::max(m_uniformNameMaxLength, (Int)uniform.name.length());
                    m_uniformLocations[uniform.name] = location;
                    MGLOG_D("ProgramObject %u: Reflection - uniform[%d] name='%s' layoutLocation=%d", m_externalIndex,
                            i, uniform.name.c_str(), location);
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

                // ------------ attributes (vertex in) ---------------
                int inCount = m_program->getNumPipeInputs();
                MGLOG_D("ProgramObject %u: Reflection - pipe input count (attributes) = %d", m_externalIndex, inCount);

                int maxLoc = -1;
                for (int i = 0; i < inCount; ++i) {
                    int loc = m_program->getPipeInput(i).layoutLocation();
                    if (loc >= 0 && loc != glslang::TQualifier::layoutLocationEnd) maxLoc = std::max(maxLoc, loc);
                    MGLOG_D("ProgramObject %u: Reflection - pipe input[%d] name='%s' layoutLocation=%d glType=%u",
                            m_externalIndex, i, m_program->getPipeInput(i).name.c_str(), loc,
                            m_program->getPipeInput(i).glDefineType);
                }

                if (maxLoc < 0) {
                    maxLoc = std::max(0, inCount - 1);
                }

                GLint maxAttribs = 16; // TODO: get from backend
                MGLOG_D("ProgramObject %u: Reflection - computed maxLoc=%d, using maxAttribs=%d", m_externalIndex,
                        maxLoc, maxAttribs);

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
                    int location = inVar.layoutLocation();
                    m_attribInNameMaxLength = std::max(m_attribInNameMaxLength, (Int)inVar.name.length());

                    if (location >= 0 && location < (int)m_attribs.size()) {
                        m_attribs[location] = inVar.name;
                        m_attribTypes[location] = inVar.glDefineType;
                        MGLOG_D("ProgramObject %u: Reflection - got attrib '%s' at explicit location %d",
                                m_externalIndex, inVar.name.c_str(), location);
                    }
                    // else if (location >= (int)m_attribs.size()) {
                    //     MGLOG_W("ProgramObject %u: ProgramObject::DoReflection - attrib location %d >= attribs.size()
                    //     "
                    //             "(%zu). Ignoring.",
                    //             m_externalIndex, location, m_attribs.size());
                    //     continue;
                    // }
                    // else {
                    //     bool placed = false;
                    //     for (size_t idx = 0; idx < m_attribs.size(); ++idx) {
                    //         if (m_attribs[idx].empty()) {
                    //             m_attribs[idx] = inVar.name;
                    //             m_attribTypes[idx] = inVar.glDefineType;
                    //             placed = true;
                    //             MGLOG_D("ProgramObject %u: Reflection - placed attrib '%s' into free slot %zu",
                    //                     m_externalIndex, inVar.name.c_str(), idx);
                    //             break;
                    //         }
                    //     }
                    //     if (!placed && (int)m_attribs.size() < maxAttribs) {
                    //         m_attribs.push_back(inVar.name);
                    //         m_attribTypes.push_back(inVar.glDefineType);
                    //         placed = true;
                    //         MGLOG_D("ProgramObject %u: Reflection - pushed attrib '%s' to new slot %zu",
                    //                 m_externalIndex, inVar.name.c_str(), m_attribs.size() - 1);
                    //     }
                    //     if (!placed) {
                    //         MGLOG_W("ProgramObject %u: ProgramObject::DoReflection - cannot place attrib '%s' (no
                    //         free "
                    //                 "slot and at max capacity). Ignoring.",
                    //                 m_externalIndex, inVar.name.c_str());
                    //     }
                    // }
                }

                // Implement glBindAttribLocation semantics (explicit locations set by user)
                // for (auto& [name, location] : m_explicitAttribLocations) {
                //     MGLOG_D("ProgramObject %u: Reflection - explicit attrib location request: name='%s' location=%d",
                //             m_externalIndex, name.c_str(), location);
                //     if (location < 0) continue;
                //     if (location >= (int)m_attribs.size()) {
                //         if (location >= maxAttribs) {
                //             MGLOG_W("ProgramObject %u: SetExplicitVertexInLocation: requested location %d >= "
                //                     "GL_MAX_VERTEX_ATTRIBS (%d). Ignored for attribute '%s'.",
                //                     m_externalIndex, location, maxAttribs, name.c_str());
                //             continue;
                //         }
                //         m_attribs.resize(location + 1);
                //         m_attribTypes.resize(location + 1);
                //         MGLOG_D("ProgramObject %u: Reflection - resized attrib arrays to %zu to accommodate explicit
                //         "
                //                 "location %d",
                //                 m_externalIndex, m_attribs.size(), location);
                //     }
                //
                //     if (m_attribs[location] != name) {
                //         auto it = std::find(m_attribs.begin(), m_attribs.end(), name);
                //         if (it == m_attribs.end()) {
                //             MGLOG_D("ProgramObject %u: Reflection - explicit attrib '%s' not found in current list, "
                //                     "skipping swap",
                //                     m_externalIndex, name.c_str());
                //             continue;
                //         }
                //         auto idx = std::distance(m_attribs.begin(), it);
                //         std::swap(m_attribs[location], m_attribs[idx]);
                //         std::swap(m_attribTypes[location], m_attribTypes[idx]);
                //         MGLOG_D("ProgramObject %u: Reflection - swapped attrib '%s' from idx %zu to explicit "
                //                 "location %d",
                //                 m_externalIndex, name.c_str(), idx, location);
                //     }
                //
                //     for (SizeT idx = 0; idx < m_attribs.size(); ++idx) {
                //         m_attribLocation[m_attribs[idx]] = idx;
                //         MGLOG_D("ProgramObject %u: Reflection - attribLocation['%s'] = %zu", m_externalIndex,
                //                 m_attribs[idx].c_str(), idx);
                //     }
                // }

                // ---------- UBO ----------
                int uboCount = m_program->getNumUniformBlocks();
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
                for (SizeT i = 0; i < m_shaders.size(); i++) {
                    auto shaderType = MG_Util::ConvertShaderStageToGLEnum(m_shaders[i]->GetShaderStage());
                    shaderTypes[i] = shaderType;
                    ShaderAttrib attrib{.shaderType = shaderType,
                                        .sourceStr = m_shaders[i]->GetShaderSource(),
                                        .flags = 0}; // Will need patched glslang to work
                    MGLOG_D("ProgramObject %u: GenerateBinary - compiling shader[%zu] type %u", m_externalIndex, i,
                            shaderType);
                    auto res = ShaderCompiler::CompileShader(attrib);
                    if (!res) {
                        MGLOG_E("ProgramObject %u: GenerateBinary - CompileShader failed for shader[%zu], aborting "
                                "binary generation",
                                m_externalIndex, i);
                        MGLOG_E("ProgramObject %u: GenerateBinary - CompileShader return code %d, log:\n%s",
                                m_externalIndex, res.error().errc, res.error().log.c_str());
                        MGLOG_E("ProgramObject %u: GenerateBinary - last compiled shader src: \n%s", m_externalIndex,
                                m_shaders[i]->GetShaderSource().c_str());
                    }
                    MOBILEGL_ASSERT(res, "CompileShader failed during binary generation");
                    shaders[i] = res.value();
                    MGLOG_D("ProgramObject %u: GenerateBinary - compiled shader[%zu] -> TShader ptr %p",
                            m_externalIndex, i, shaders[i].get());
                }

                ProgramAttrib attrib{.shaders = Move(shaders),
                                     .explicitVertexInLocations = m_explicitAttribLocations,
                                     .explicitFragmentOutLocations = m_explicitFragDataLocation};
                MGLOG_D("ProgramObject %u: GenerateBinary - linking program for binary", m_externalIndex);
                auto programResult = ShaderCompiler::LinkProgram(attrib);
                if (!programResult) {
                    MGLOG_E("ProgramObject %u: GenerateBinary - LinkProgram failed during binary generation",
                            m_externalIndex);
                }
                MOBILEGL_ASSERT(programResult, "LinkProgram failed during binary generation");
                auto program = programResult.value();
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

                for (auto& spv : m_generatedSpirv) {
                    auto success = ShaderCompiler::SanitizeAndOptimizeBinary(spv, spv);
                    MOBILEGL_ASSERT(success, "SanitizeBinary failed");
                }

                for (SizeT i = 0; i < m_generatedSpirv.size(); i++) {
                    auto& spv = m_generatedSpirv[i];

                    auto shaderType = shaderTypes[i];
                    MGLOG_D("ProgramObject %u: GenerateBinary - parsing SPIR-V meta data for module %zu "
                            "(shaderType=%u, wordCount=%zu)",
                            m_externalIndex, i, shaderType, spv.size());
                    SpvcSession session(spv);
                    auto result = session.ParseMetaData();
                    if (result < 0) {
                        MGLOG_D("ProgramObject %u: GenerateBinary - SpvcSession::ParseMetaData failed for module %zu, "
                                "err = %d%s",
                                m_externalIndex, i, result,
                                (result == SPVC_ERROR_INVALID_SPIRV ? ". Probably no global UBO?" : ""));
                        m_uniformSizesInBytes.clear();
                        m_uniformOffsets.clear();
                        m_uboScratch.clear();
                        continue;
                    } else {
                        auto& meta = session.GetMetadata();
                        auto size = meta.uboSize;
                        MGLOG_D("ProgramObject %u: GenerateBinary - SPIR-V meta: uboSize=%zu plainUniformCount=%zu "
                                "plainUniformOffsets=%zu",
                                m_externalIndex, meta.uboSize, meta.plainUniformMemberSizesInBytes.size(),
                                meta.plainUniformOffsetsInUBO.size());
                        m_uboScratch.resize(size);
                        m_uniformOffsets.resize(m_maxUniformLocation + 1);
                        for (const auto& [name, offset] : meta.plainUniformOffsetsInUBO) {
                            if (m_uniformLocations.find(name) != m_uniformLocations.end()) {
                                m_uniformOffsets[m_uniformLocations[name]] = offset;
                                MGLOG_D(
                                    "ProgramObject %u: GenerateBinary - uniform '%s' offset=%u assigned to location %u",
                                    m_externalIndex, name.c_str(), offset, m_uniformLocations[name]);
                            } else {
                                MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' offset=%u but not found in "
                                        "m_uniformLocations",
                                        m_externalIndex, name.c_str(), offset);
                            }
                        }
                        m_uniformSizesInBytes.resize(m_maxUniformLocation + 1);
                        for (const auto& [name, size] : meta.plainUniformMemberSizesInBytes) {
                            if (m_uniformLocations.find(name) != m_uniformLocations.end()) {
                                m_uniformSizesInBytes[m_uniformLocations[name]] = size;
                                MGLOG_D(
                                    "ProgramObject %u: GenerateBinary - uniform '%s' size=%u assigned to location %u",
                                    m_externalIndex, name.c_str(), size, m_uniformLocations[name]);
                            } else {
                                MGLOG_D("ProgramObject %u: GenerateBinary - uniform '%s' size=%u but not found in "
                                        "m_uniformLocations",
                                        m_externalIndex, name.c_str(), size);
                            }
                        }
                        // Only parse first module that contains uniform metadata
                        MGLOG_D("ProgramObject %u: GenerateBinary - finished parsing module %zu; breaking after first "
                                "valid metadata",
                                m_externalIndex, i);
                    }
                    break;
                }
            }

            void ProgramObject::WaitUntilGenerationCompleted() {
                MGLOG_D("ProgramObject %u: WaitUntilGenerationCompleted called (no-op)", m_externalIndex);
                // currently no-op, but keep log for debugging
                // will probably be useful when multi-threaded compilation
            }

            void ProgramObject::SetExplicitVertexInLocation(Uint index, const char* name) {
                MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation called name='%s' index=%u", m_externalIndex,
                        name, index);
                m_explicitAttribLocations[name] = index;
                MGLOG_D("ProgramObject %u: SetExplicitVertexInLocation - stored explicit location for '%s' -> %u",
                        m_externalIndex, name, index);
            }

            void ProgramObject::SetExplicitFragmentOutLocation(Uint index, const char* name) {
                MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation called name='%s' index=%u", m_externalIndex,
                        name, index);
                m_explicitFragDataLocation[name] = index;
                MGLOG_D("ProgramObject %u: SetExplicitFragmentOutLocation - stored explicit location for '%s' -> %u",
                        m_externalIndex, name, index);
            }

            Int ProgramObject::GetFragmentDataLocation(const char* name) {
                // TODO: should retrieve "post-mortem" location from glslang instead
                auto it = m_explicitFragDataLocation.find(name);
                if (it == m_explicitFragDataLocation.end()) return -1;
                return it->second;
            }
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
