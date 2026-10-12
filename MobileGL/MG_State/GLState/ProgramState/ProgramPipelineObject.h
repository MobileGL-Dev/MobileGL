// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramPipelineObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "ProgramObject.h"

namespace MobileGL {
    namespace MG_State {
        namespace GLState {
            // A GL_ARB_separate_shader_objects program pipeline (GL 4.6 core 7.4): a set of
            // per-stage program references plus the program glProgramUniform* addresses when no
            // program object is in use.
            class ProgramPipelineObject {
            public:
                explicit ProgramPipelineObject(Uint externalIndex) : m_externalIndex(externalIndex) {}

                const SharedPtr<ProgramObject>& GetStageProgram(ShaderStage stage) const {
                    return m_stagePrograms[static_cast<SizeT>(stage)];
                }
                void SetStageProgram(ShaderStage stage, const SharedPtr<ProgramObject>& program) {
                    m_stagePrograms[static_cast<SizeT>(stage)] = program;
                }

                const SharedPtr<ProgramObject>& GetActiveProgram() const { return m_activeProgram; }
                void SetActiveProgram(const SharedPtr<ProgramObject>& program) { m_activeProgram = program; }

                // Distinct from ProgramObject's, which starts true: a pipeline that has never been
                // validated must report GL_VALIDATE_STATUS as 0 (GL 4.6 core table 23.31).
                Bool GetValidateStatus() const { return m_validateStatus; }
                void SetValidateStatus(Bool value) { m_validateStatus = value; }

                const String& GetInfoLog() const { return m_infoLog; }
                void SetInfoLog(String log) { m_infoLog = Move(log); }

                Uint GetExternalIndex() const { return m_externalIndex; }

                // GL 4.6 core 11.1.3.11, the pipeline validation rules - the ONE copy, read by
                // glValidateProgramPipeline and by every draw made through the pipeline (where a
                // failure is INVALID_OPERATION). Returns why the pipeline is not valid, or an empty
                // string when it is.
                //
                // `requireAnyStage`: a pipeline with no program in any stage fails VALIDATION, but a
                // draw through one is merely undefined and is dropped elsewhere without an error.
                //
                // Every question is asked of a program's EXECUTABLE (HasLinkedShaderStage and the
                // separable flag as last linked), never of its live attach list.
                String FindValidationError(Bool requireAnyStage) const {
                    constexpr SizeT kStageCount = static_cast<SizeT>(ShaderStage::ShaderStageCount);
                    const auto executes = [this](SizeT stage) {
                        const auto& program = m_stagePrograms[stage];
                        return program && program->HasLinkedShaderStage(static_cast<ShaderStage>(stage));
                    };

                    Bool anyStage = false;
                    for (SizeT stage = 0; stage < kStageCount; ++stage) {
                        const auto& program = m_stagePrograms[stage];
                        if (!program) continue;
                        anyStage = true;
                        // A stage program whose last relink failed, or that was relinked as not
                        // separable, can no longer serve as a stage (glUseProgramStages checked
                        // both, but only at the time of the call).
                        if (!program->GetLinkStatus()) {
                            return std::format("The program in stage {} is not successfully linked.", stage);
                        }
                        if (!program->GetLinkedSeparable()) {
                            return std::format("The program in stage {} was relinked as not separable.", stage);
                        }
                        // "A program object is active for at least one, but not all of the shader
                        // stages that were present when the program was linked."
                        for (SizeT other = 0; other < kStageCount; ++other) {
                            if (program->HasLinkedShaderStage(static_cast<ShaderStage>(other)) &&
                                m_stagePrograms[other] != program) {
                                return std::format("A program is active for stage {} but not for its linked stage {}.",
                                                   stage, other);
                            }
                        }
                    }
                    if (!anyStage) {
                        return requireAnyStage ? String("The pipeline has no program in any stage.") : String();
                    }

                    // "One program object is active for at least two shader stages and a second
                    // program is active for a shader stage between two stages for which the first
                    // program was active." Over the graphics stages, in pipeline order.
                    for (SizeT first = 0; first < kGraphicsStageCount; ++first) {
                        if (!executes(first)) continue;
                        for (SizeT last = first + 2; last < kGraphicsStageCount; ++last) {
                            if (!executes(last) || m_stagePrograms[last] != m_stagePrograms[first]) continue;
                            for (SizeT between = first + 1; between < last; ++between) {
                                if (executes(between) && m_stagePrograms[between] != m_stagePrograms[first]) {
                                    return std::format("The program in stage {} sits between two stages of another "
                                                       "program.",
                                                       between);
                                }
                            }
                        }
                    }

                    // "There is an active program for tessellation control, tessellation evaluation,
                    // or geometry stages with corresponding executable shader, but there is no
                    // active program with an executable vertex shader."
                    const Bool laterVertexProcessing =
                        executes(static_cast<SizeT>(ShaderStage::TessControl)) ||
                        executes(static_cast<SizeT>(ShaderStage::TessEval)) ||
                        executes(static_cast<SizeT>(ShaderStage::Geometry));
                    if (laterVertexProcessing && !executes(static_cast<SizeT>(ShaderStage::Vertex))) {
                        return "A tessellation or geometry stage is active but no program supplies a vertex shader.";
                    }
                    return {};
                }

                // glIsProgramPipeline's answer, and NOT the same question as "does this object
                // exist" (GL 4.6 core 7.4: a GenProgramPipelines name "acquires program pipeline
                // state only when first bound"). The object is materialized by any of the
                // commands that take state from a reserved name - including the pure queries
                // glGetProgramPipelineiv and glGetProgramPipelineInfoLog, which have to answer
                // out of default state without ever making the name report as an object. So
                // existence is map membership and this is a separate latch, exactly as
                // TransformFeedbackObject::everBound is.
                Bool GetEverBound() const { return m_everBound; }
                void MarkEverBound() { m_everBound = true; }

                // The stages a DRAW is built from: every stage but compute. GL 4.6 core 7.4
                // makes the compute stage exclusive - a program object containing a compute
                // shader may contain no other stage, and a pipeline's compute stage is
                // dispatched on its own and never participates in a draw. So the compute stage
                // is not merely irrelevant to the composite below, it must never enter it: a
                // compute module handed to vkCreateGraphicsPipelines is a driver crash rather
                // than an error return (Adreno 830 SIGSEGVs inside it).
                static constexpr SizeT kGraphicsStageCount = static_cast<SizeT>(ShaderStage::Compute);
                static_assert(static_cast<SizeT>(ShaderStage::Compute) + 1 ==
                                  static_cast<SizeT>(ShaderStage::ShaderStageCount),
                              "ShaderStage must keep Compute last so the graphics stages are a prefix");

                // A draw sees one program, but a pipeline holds one program per stage. The
                // GRAPHICS stages are composited into a single hidden program object, rebuilt
                // whenever the stage set - or any stage program's own link - changes. The
                // signature is what that "changes" means: a stage program's lifetime id pins the
                // object and its LINK version pins the link generation. It covers exactly the
                // stages the composite is built from, so attaching or relinking a compute stage
                // never invalidates a perfectly good graphics composite - and the compute stage,
                // having no composite of its own, can never collide with it.
                //
                // GetLinkVersion() and NOT GetBackendStateVersion(), which is what this used to
                // key on. The backend state version moves on every glUniform1i to a sampler and
                // every glUniformBlockBinding, so the "set a sampler unit, draw" loop that the
                // SSO conformance cases run threw the composite away and REBUILT it on every
                // single draw: a fresh ProgramObject, a full Link(true) settled synchronously
                // (glslang + SPIR-V + spirv-opt), a full re-mirror, and a brand-new program
                // identity that invalidated both backends' per-program registries and pipeline
                // memos along the way. The composite's CONTENT depends on the link generations
                // and nothing else, and m_linkVersion is bumped by exactly those
                // (BumpLinkObservableVersions).
                //
                // The prerequisite that makes the narrowing legal: because the composite no
                // longer rebuilds when per-program uniform STATE changes, every such change must
                // reach it through the refresh below instead. Both do - sampler/image units via
                // MirrorUniformValues, interface block bindings via MirrorBlockBindings - and
                // the two setters that write them still bump the counters the REFRESH gate reads
                // (see ComputeUniformMirrorVersions), which is a separate question from what
                // this signature reads. They are the only two writers of m_backendStateVersion
                // outside the link paths, so nothing else was ever riding on the rebuild.
                using DrawProgramSignature = Array<Uint64, kGraphicsStageCount * 2>;

                DrawProgramSignature ComputeDrawProgramSignature() const {
                    DrawProgramSignature signature{};
                    for (SizeT stage = 0; stage < kGraphicsStageCount; ++stage) {
                        const auto& program = m_stagePrograms[stage];
                        if (!program) continue;
                        signature[stage * 2] = program->GetLifetimeId();
                        signature[stage * 2 + 1] = program->GetLinkVersion();
                    }
                    return signature;
                }

                // Per-program state is written to the STAGE programs - glUniform* addresses the
                // pipeline's active program (GL 4.6 core 7.6.1), glProgramUniform* addresses a
                // named one, and the two block-binding calls address a named one - while the
                // draw reads the composite. Two different objects' state, so the composite is
                // refreshed from its stage programs before each draw that needs it. These are
                // the per-stage versions "needs it" is measured against. All zero after a
                // rebuild, because a fresh composite holds only what its shaders declared and
                // so needs a full refresh.
                //
                // backendStateVersion belongs HERE even though ComputeDrawProgramSignature no
                // longer reads it, and that is the whole point of the split: a sampler-unit or
                // uniform-block-binding write must still trip the MIRROR (it is now the only
                // route those values have to the composite) while deliberately NOT tripping the
                // rebuild. uboContentVersion covers ordinary uniform writes, and
                // blockBindingVersion covers the storage-block setter, which moves neither of
                // the other two.
                using UniformMirrorVersions = Array<Uint64, kGraphicsStageCount * 2>;

                UniformMirrorVersions ComputeUniformMirrorVersions() const {
                    UniformMirrorVersions versions{};
                    for (SizeT stage = 0; stage < kGraphicsStageCount; ++stage) {
                        const auto& program = m_stagePrograms[stage];
                        if (!program) continue;
                        versions[stage * 2] = (static_cast<Uint64>(program->GetBackendStateVersion()) << 32) |
                                              static_cast<Uint64>(program->GetUBOContentVersion());
                        // Their own slot rather than folded into the pair above: the
                        // storage-block setter moves the block-binding version and NOTHING
                        // else, so a rebinding would otherwise be invisible to the refresh
                        // gate - and the write-set version is the only counter that moves for
                        // a write which ENLARGES the set without changing a byte (see
                        // ProgramObject::GetUniformWriteSetVersion), which is what decides
                        // which stage owns a shared name.
                        versions[stage * 2 + 1] = (static_cast<Uint64>(program->GetBlockBindingVersion()) << 32) |
                                                  static_cast<Uint64>(program->GetUniformWriteSetVersion());
                    }
                    return versions;
                }
                const UniformMirrorVersions& GetMirroredUniformVersions() const { return m_mirroredUniformVersions; }
                void SetMirroredUniformVersions(const UniformMirrorVersions& versions) {
                    m_mirroredUniformVersions = versions;
                }

                const SharedPtr<ProgramObject>& GetCachedDrawProgram(const DrawProgramSignature& signature) const {
                    static const SharedPtr<ProgramObject> nullProgram = nullptr;
                    if (!m_drawProgram || m_drawProgramSignature != signature) return nullProgram;
                    return m_drawProgram;
                }
                void SetCachedDrawProgram(const DrawProgramSignature& signature, SharedPtr<ProgramObject> program) {
                    m_drawProgramSignature = signature;
                    m_drawProgram = Move(program);
                    // A rebuilt composite holds none of its stage programs' uniform values yet.
                    m_mirroredUniformVersions = {};
                }

            private:
                Array<SharedPtr<ProgramObject>, static_cast<SizeT>(ShaderStage::ShaderStageCount)> m_stagePrograms{};
                SharedPtr<ProgramObject> m_activeProgram;
                SharedPtr<ProgramObject> m_drawProgram;
                DrawProgramSignature m_drawProgramSignature{};
                UniformMirrorVersions m_mirroredUniformVersions{};
                String m_infoLog;
                const Uint m_externalIndex = 0;
                Bool m_validateStatus = false;
                Bool m_everBound = false;
            };
        } // namespace GLState
    } // namespace MG_State
} // namespace MobileGL
