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
                // object and its backend state version pins the link generation.
                //
                // The backend state version is BLUNTER than that description: glUniform1i on a
                // sampler and glUniformBlockBinding bump it too, so either one throws the
                // composite away and relinks it on the next draw. That is correct but slow, and
                // it is a shape the SSO conformance cases hit in a loop. Narrowing it to
                // GetLinkVersion() means the composite must instead pick those two up the way it
                // picks up uniform values (below) - the sampler half already works that way,
                // the block-binding half does not yet, which is why this still keys on the
                // blunter version.
                // It covers
                // exactly the stages the composite is built from, so attaching or relinking a
                // compute stage never invalidates a perfectly good graphics composite - and the
                // compute stage, having no composite of its own, can never collide with it.
                using DrawProgramSignature = Array<Uint64, kGraphicsStageCount * 2>;

                DrawProgramSignature ComputeDrawProgramSignature() const {
                    DrawProgramSignature signature{};
                    for (SizeT stage = 0; stage < kGraphicsStageCount; ++stage) {
                        const auto& program = m_stagePrograms[stage];
                        if (!program) continue;
                        signature[stage * 2] = program->GetLifetimeId();
                        signature[stage * 2 + 1] = program->GetBackendStateVersion();
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
                using UniformMirrorVersions = Array<Uint64, kGraphicsStageCount * 2>;

                UniformMirrorVersions ComputeUniformMirrorVersions() const {
                    UniformMirrorVersions versions{};
                    for (SizeT stage = 0; stage < kGraphicsStageCount; ++stage) {
                        const auto& program = m_stagePrograms[stage];
                        if (!program) continue;
                        versions[stage * 2] = (static_cast<Uint64>(program->GetBackendStateVersion()) << 32) |
                                              static_cast<Uint64>(program->GetUBOContentVersion());
                        // Its own slot rather than folded into the pair above: the storage-block
                        // setter moves this and NOTHING else, so a rebinding would otherwise be
                        // invisible to the refresh gate.
                        versions[stage * 2 + 1] = program->GetBlockBindingVersion();
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
