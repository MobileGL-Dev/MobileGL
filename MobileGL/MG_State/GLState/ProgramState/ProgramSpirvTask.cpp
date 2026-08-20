// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ProgramSpirvTask.h"

#include <MG_State/GLState/ProgramState/ShaderCompileTask.h> // GlslangThreadAllocatorGuard
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/TranslationCache.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <cstring>

namespace MobileGL::MG_State::GLState {
    void ProgramSpirvTask::DeferLog(String line) { diagnostics.logLines.push_back(Move(line)); }

    void ProgramSpirvTask::SubmitAfter(const SharedPtr<ProgramLinkTask>& phaseA) {
        MOBILEGL_ASSERT(phaseA != nullptr, "ProgramSpirvTask::SubmitAfter: the phase-A node is missing");
        m_phaseA = phaseA;

        auto self = std::static_pointer_cast<ProgramSpirvTask>(shared_from_this());
        // ONE dependency, so no counter and no guard slot: the whole race
        // ProgramLinkTask::SubmitAfter's +1 exists to close (a dependency settling while the
        // remaining edges are still being registered) cannot arise with a single edge.
        //
        // Runs inline, right here, if phase A is already terminal.
        phaseA->OnTerminal([self, phaseA] {
            // "Dependency did not complete, publish nothing" - the same collapse
            // ProgramLinkTask::CompiledArtifacts() performs for an abandoned compile. Note
            // this reads the HANDOFF, never phaseA->artifacts: the GL thread may already be
            // moving those out (see the class comment).
            if (!phaseA->IsComplete() || !phaseA->spirvHandoff.ready) {
                self->Cancel();
                return;
            }
            // A cancel that landed before phase A settled (relink, glDeleteProgram, teardown).
            // Posting would only make a worker pick up a node that immediately falls out of
            // Run() again.
            if (self->IsCancellationRequested()) {
                self->Cancel();
                return;
            }
            // Non-throwing by construction, and it has to be: this is a JobNode continuation,
            // so on the pool side it runs inside an Asio handler. Post() contains its own
            // allocation failures, and the catch below CANCELS rather than swallowing - a
            // phase B that is never posted is a GL thread blocked forever in
            // EnsureSpirvJoined(), which is far worse than a program reported as not drawable.
            try {
                MG_Util::Async::ShaderCompilePool::Get().Post(self);
            } catch (...) {
                self->Cancel();
            }
        });
    }

    void ProgramSpirvTask::RunInlineAfter(const SharedPtr<ProgramLinkTask>& phaseA) {
        MOBILEGL_ASSERT(phaseA != nullptr, "ProgramSpirvTask::RunInlineAfter: the phase-A node is missing");
        MOBILEGL_ASSERT(phaseA->IsTerminal(),
                        "ProgramSpirvTask::RunInlineAfter: phase A has not settled; the inline path must run the "
                        "two bodies in order on the same thread");
        m_phaseA = phaseA;
        RunInline();
    }

    // Pure CPU work only, on a pool worker (or on the GL thread in the inline mode).
    // Everything this reads is either owned by this node or published by a terminal phase A;
    // everything it writes is `artifacts` (and diagnostics). Same prohibitions as
    // ProgramLinkTask::RunBody - no GL/EGL call, no pActiveBackendObject read, no
    // pGLContext->RecordError().
    void ProgramSpirvTask::RunBody() {
        // glslang leaves this worker's TLS pool allocator pointing at the last arena it
        // touched; reset it on the way out so an unrelated later job cannot allocate out of a
        // pool that has since been freed. Declared FIRST so it is destroyed LAST - the phase-A
        // release below drops the TShaders (and their pools) and must happen inside it.
        const GlslangThreadAllocatorGuard glslangGuard;
        using namespace MG_Util::ShaderTranspiler;

        // Drop phase A - and with it the TShaders, the TProgram reference and phase A's whole
        // input snapshot - the moment this body is done, rather than at some later join. For a
        // pack load that is the difference between W glslang arenas alive and all of them.
        struct PhaseAReleaser {
            SharedPtr<ProgramLinkTask>& node;
            ~PhaseAReleaser() { node.reset(); }
        } const phaseAReleaser{m_phaseA};

        if (!m_phaseA) return;
        // Non-const: the TShaders are dropped below, the moment GlslangToSpv is finished with
        // them. This is safe by ownership rather than by locking - phase A is terminal and
        // therefore immutable to everyone else, the GL-thread join touches only `artifacts`
        // and `diagnostics`, and this node is the sole reader of the handoff.
        ProgramLinkTask::SpirvHandoff& handoff = m_phaseA->spirvHandoff;
        const Uint externalIndex = m_phaseA->in.externalIndex;
        if (!handoff.ready || !handoff.reflection.program) {
            // Phase A did not reach its tail (it failed the link, or was cancelled mid-body).
            // Publish nothing; spirvStatus stays false.
            return;
        }

        MGLOG_D("ProgramObject %u: Starting SPIR-V generation", externalIndex);
        const Bool deferOutputValidationForDirectVulkan =
            m_phaseA->in.env != nullptr && m_phaseA->in.env->backend == BackendType::DirectVulkan;
        const Bool enableSpirvValidation = m_phaseA->in.enableSpirvValidation;
        artifacts.enableSpirvValidation = enableSpirvValidation;
        GenerateSpirv(handoff, externalIndex, deferOutputValidationForDirectVulkan, enableSpirvValidation);
        // GlslangToSpv was the only consumer of the parsed ASTs; everything after this point
        // works on the SPIR-V and on the TProgram's own self-contained reflection pool. Drop
        // them here rather than at the end of the body, which is ~87% of this node's runtime
        // earlier (spirv-opt plus routing).
        //
        // WHAT THIS ACTUALLY FREES, precisely - it is LESS than "the glslang arenas", and the
        // difference matters for the peak-RSS story:
        //   * CAS-LOSER shaders (the re-parse in ShaderCompileTask::ClaimParsedShader, i.e.
        //     the 2nd..Nth link of a shared shader): freed here in full. The handoff is their
        //     ONLY owner.
        //   * CAS-WINNER shaders (the common case - one shader object linked into one
        //     program, which is every program of an Iris pack load): NOT freed here. The
        //     winner branch returns a COPY of ShaderCompileTask::artifacts.shader
        //     (ShaderCompileTask.cpp:320) and the node never releases its own reference, while
        //     phase A holds that node through in.shaders[i].compiled for its whole life - and
        //     phase A lives until PhaseAReleaser fires at the end of this body. So the
        //     refcount goes 2 -> 1 here and the arena dies where it would have died anyway.
        //
        // Making it free the winner's arena too means releasing whatever pins the TShader
        // inside the compile node, and neither obvious route is safe as a drive-by: moving out
        // of artifacts.shader at claim time races ShaderObject::GetCompiledShader() on the GL
        // thread and breaks JobNode's "a terminal node is immutable" invariant, and dropping
        // phase A's in.shaders[i].compiled reference only helps when nothing else holds the
        // node (the adoption map is a WeakPtr index, so it would also change which nodes stay
        // adoptable). Both belong in a change that can be reviewed against the consume-once
        // and adoption semantics on their own terms.
        handoff.shaders.clear();

        MGLOG_D("ProgramObject %u: Building global-UBO routing tables", externalIndex);
        BuildGlobalUboRouting(handoff, externalIndex);
        MGLOG_D("ProgramObject %u: Binary generation finished (generatedSpirv size=%zu)", externalIndex,
                artifacts.generatedSpirv.size());
    }

    void ProgramSpirvTask::GenerateSpirv(const ProgramLinkTask::SpirvHandoff& handoff, const Uint externalIndex,
                                         const Bool deferOutputValidationForDirectVulkan,
                                         const Bool enableSpirvValidation) {
        /* As we passed first stage compilation/linking,
         * we'll assume all the operations here should
         * pass. We may be able to employ some optimizations
         * here without the burden of error reporting.
         */
        using namespace MG_Util::ShaderTranspiler;
        MGLOG_D("ProgramObject %u: GenerateSpirv - start", externalIndex);

        // L1 of the shader translation memo. The segment this short-circuits is the whole
        // of GlslangToSpv plus the 11-pass SanitizeAndOptimizeBinary chain, for every stage
        // of the program at once - ~136 us per stage on the RelWithDebInfo host measurement.
        // The key was built at the tail of phase A (ProgramLinkTask::BuildSpirvCacheKey) and
        // covers every input that can move these bytes; see TranslationCache.h.
        //
        // Note what a HIT does NOT skip: the glslang parse and link, which already happened
        // in phase A because the frontend's whole GL query surface is built out of the
        // TProgram they produce.
        auto& spirvCache = GetSpirvTranslationCache();
        const TranslationCacheKey& cacheKey = handoff.spirvCacheKey;
        if (cacheKey.Valid()) {
            if (const SpirvTranslationResultPtr hit = spirvCache.Find(cacheKey);
                hit && hit->modules.size() == handoff.shaderTypes.size()) {
                artifacts.generatedSpirv = hit->modules;
                artifacts.spirvStatus = true;
                MGLOG_D("ProgramObject %u: GenerateSpirv - L1 cache hit, %zu module(s) reused",
                        externalIndex, artifacts.generatedSpirv.size());
                return;
            }
        }

        // The shaders were parsed once, in the link-compatible (relaxed Vulkan-rules)
        // configuration, and the handoff's program linked those parses - so it IS the program
        // the backends consume. Generate SPIR-V straight from its intermediates, which the
        // handoff's TShaders keep alive.
        ProgramBinaryAttrib binaryAttrib{
            .shaderTypes = handoff.shaderTypes,
            .program = *handoff.reflection.program,
        };
        MGLOG_D("ProgramObject %u: GenerateSpirv - requesting SPIR-V binary from program", externalIndex);
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult) {
            DeferLog(std::format("ProgramObject {}: GenerateSpirv - GetSpirvBinaryFromProgram failed", externalIndex));
            MOBILEGL_ASSERT(binaryResult, "GetSpirvBinaryFromProgram failed");
            return; // spirvStatus stays false: linked, but not drawable.
        }
        artifacts.generatedSpirv = Move(binaryResult.value());
        MGLOG_D("ProgramObject %u: GenerateSpirv - generated %zu SPIR-V modules", externalIndex,
                artifacts.generatedSpirv.size());

        // Linked SPIR-V generated, sanitize and optimize it
        Bool allOptimized = true;
        {
            for (auto& spv : artifacts.generatedSpirv) {
                auto success = ShaderCompiler::SanitizeAndOptimizeBinary(
                    spv, spv, !deferOutputValidationForDirectVulkan, enableSpirvValidation);
                if (!success) {
                    // The one genuine phase-B failure mode: one of the seven optimizer passes
                    // reported failure, so `spv` is whatever the run left behind. A fordebug
                    // build trips the assert below; a release build used to hand that binary
                    // to the backend regardless. It no longer does - the program keeps its
                    // (truthful) LINK_STATUS and its whole query surface, and the routing
                    // tables below still give every settable uniform storage so glUniform*
                    // and glGetUniform* keep working, but spirvStatus stays false and the
                    // backends refuse to build or draw with it.
                    allOptimized = false;
                    DeferLog(std::format("ProgramObject {}: SanitizeAndOptimizeBinary failed; the program is linked "
                                         "and queryable but not drawable",
                                         externalIndex));
                }
                MOBILEGL_ASSERT(success, "SanitizeBinary failed");
            }
        }
        artifacts.spirvStatus = allOptimized;

        // Only a clean run is memoized. A failed optimizer run leaves `spv` as whatever the
        // chain got to before it gave up, and that is exactly the binary no other program
        // should ever be handed.
        if (allOptimized && cacheKey.Valid()) {
            auto payload = MakeShared<SpirvTranslationResult>();
            payload->modules = artifacts.generatedSpirv;
            const SizeT payloadBytes = SpirvTranslationResultBytes(*payload);
            spirvCache.Insert(cacheKey, SpirvTranslationResultPtr(Move(payload)), payloadBytes);
        }
    }

    void ProgramSpirvTask::BuildGlobalUboRouting(const ProgramLinkTask::SpirvHandoff& handoff,
                                                 const Uint externalIndex) {
        using namespace MG_Util::ShaderTranspiler;
        // The phase-A reflection slice this pass keys off. Carried in the handoff rather than
        // read off the phase-A node's artifacts, which the join has very likely already moved.
        const ProgramObject::LinkArtifacts& reflection = handoff.reflection;

        artifacts.uniformOffsets.clear();
        artifacts.globalUboScratch.clear();
        // kInvalidUniformOffset marks locations that end up without global-UBO backing
        // (e.g. the optimizer eliminated every use of the uniform); the fallback pass
        // below gives those locations tail storage so glUniform* always has a target.
        artifacts.uniformOffsets.resize(reflection.maxUniformLocation + 1, ProgramObject::kInvalidUniformOffset);
        for (SizeT i = 0; i < artifacts.generatedSpirv.size(); i++) {
            auto& spv = artifacts.generatedSpirv[i];

            auto shaderType = i < handoff.shaderTypes.size() ? handoff.shaderTypes[i] : GLenum{0};
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - parsing SPIR-V meta data for module %zu "
                    "(shaderType=%u, wordCount=%zu)",
                    externalIndex, i, shaderType, spv.size());
            SpvcSession session(spv, SessionUsageBit::Reflection);
            auto result = session.ParseMetaData();
            if (result < 0) {
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SpvcSession::ParseMetaData failed for module %zu, "
                        "err = %d%s",
                        externalIndex, i, result,
                        (result == SPVC_ERROR_INVALID_SPIRV ? ". Probably no global UBO?" : ""));
                continue;
            } else {
                auto& meta = session.GetMetadata();
                auto size = meta.globalUboSize;
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - SPIR-V meta: uboSize=%zu plainUniformCount=%zu "
                        "plainUniformOffsets=%zu",
                        externalIndex, meta.globalUboSize, meta.plainUniformMemberSizesInBytes.size(),
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
                    auto locationIt = reflection.uniformLocations.find(name);
                    if (locationIt == reflection.uniformLocations.end()) {
                        locationIt = reflection.uniformLocations.find(name + "[0]");
                    }
                    if (locationIt == reflection.uniformLocations.end()) {
                        MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u but not found in "
                                "uniformLocations",
                                externalIndex, name.c_str(), offset);
                        continue;
                    }
                    const Uint baseLocation = locationIt->second;
                    if (!ProgramObject::IsValidUniformLocation(reflection, static_cast<Int>(baseLocation))) {
                        continue;
                    }

                    const Int uniformIndex = reflection.uniformIndexInTProgram[baseLocation];
                    const GLint arraySize = ProgramObject::GetUniformArraySizeByTIndex(reflection, uniformIndex);
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
                        if (location > reflection.maxUniformLocation ||
                            reflection.uniformIndexInTProgram[location] != uniformIndex) {
                            break;
                        }
                        artifacts.uniformOffsets[location] = offset + static_cast<Uint>(element) * arrayStride;
                    }
                    MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' offset=%u stride=%u assigned "
                            "to locations %u..%u",
                            externalIndex, name.c_str(), offset, arrayStride, baseLocation,
                            baseLocation + static_cast<Uint>(elementCount) - 1);
                }
                MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - finished parsing module %zu metadata",
                        externalIndex, i);
            }
        }

        // Fallback pass: a linked program's active non-opaque uniforms must accept
        // glUniform*/glGetUniform* even when the optimized SPIR-V no longer contains
        // them (AggressiveDCE can remove a dead loop together with the only loads of a
        // uniform -- or the entire global UBO, leaving the scratch unallocated). Hand
        // such locations CPU-side storage at the (16-byte aligned) tail of the shadow
        // buffer; backends bind at least the SPIR-V-declared UBO range, and the GPU
        // never reads these bytes, so this only keeps the GL-visible state coherent.
        for (Uint location = 0; location <= reflection.maxUniformLocation; ++location) {
            if (artifacts.uniformOffsets[location] != ProgramObject::kInvalidUniformOffset) continue;
            if (!ProgramObject::IsValidUniformLocation(reflection, static_cast<Int>(location))) continue;
            const auto& uniform =
                ProgramObject::UniformAtIn(reflection, reflection.uniformIndexInTProgram[location]);
            if (uniform.type.isOpaque) continue;
            // Member of a named uniform block: not settable through glUniform*, so it needs
            // no global-UBO shadow storage. tProgramBlockIndexToGl[i] >= 0 means block i is
            // GL-visible, i.e. NOT the synthesized MGL_GLOBAL_UBO - which is exactly what the
            // strstr(GLOBAL_UBO_NAME) test this replaced was asking, without needing the
            // TProgram to spell the block name.
            if (uniform.index >= 0 &&
                uniform.index < static_cast<Int>(reflection.tProgramBlockIndexToGl.size()) &&
                reflection.tProgramBlockIndexToGl[uniform.index] >= 0) {
                continue;
            }

            // std140-style slot: the matrix upload paths write column vectors at
            // 16-byte strides, so a matrix slot must cover cols * 16 bytes.
            SizeT slotSize = MG_Util::GetGLTypeSize(uniform.glDefineType);
            if (uniform.type.isMatrix) {
                slotSize = static_cast<SizeT>(uniform.type.matrixCols) * 16u;
            }
            slotSize = (slotSize + 15u) & ~static_cast<SizeT>(15u);
            const SizeT slotOffset = (artifacts.globalUboScratch.size() + 15u) & ~static_cast<SizeT>(15u);
            artifacts.globalUboScratch.resize(slotOffset + slotSize, 0);
            artifacts.uniformOffsets[location] = static_cast<Uint>(slotOffset);
            MGLOG_D("ProgramObject %u: BuildGlobalUboRouting - uniform '%s' location %u has no UBO backing in the "
                    "generated SPIR-V (optimized out?); allocated %zu fallback bytes at scratch offset %zu",
                    externalIndex, uniform.name.c_str(), location, slotSize, slotOffset);
        }
    }
} // namespace MobileGL::MG_State::GLState
