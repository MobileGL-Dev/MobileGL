// MobileGL - MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_Util/ShaderTranspiler/TranslationCache.h>

namespace MobileGL::MG_State::GLState {
    // ===================================================================================
    // L1 of the shader translation memo: THE WHOLE FRONT END of one glLinkProgram.
    //
    // A hit skips the glslang parse, the glslang link and mapIO, GlslangToSpv, the 11-pass
    // SanitizeAndOptimizeBinary chain, buildReflection, and the global-UBO routing pass. No
    // TShader and no TProgram is constructed at all - which is only possible because the GL
    // query surface no longer reads one (see ProgramObject::UniformReflection and
    // ProgramLinkTask::SnapshotGlslangReflection).
    //
    // WHY THE PAYLOAD IS THE WHOLE THING rather than just the SPIR-V: the frontend answers
    // glGetActiveUniform, glGetProgramResource*, glGetUniformLocation and the rest out of
    // LinkArtifacts, and glUniform*/glGetUniform* out of SpirvArtifacts. Caching only the
    // modules would have left the parse and the link on the hot path to rebuild exactly the
    // data the payload can carry - and the parse alone is ~48% of a CTS-shaped program build.
    //
    // WHY IT LIVES HERE AND NOT IN MG_Util: the payload is a ProgramObject::LinkArtifacts
    // plus a ProgramObject::SpirvArtifacts, and MG_Util must not depend on MG_State. The
    // KEY is plain bytes and stays in MG_Util (BuildSpirvTranslationKey), so both layers
    // agree on exactly one definition of "the same front-end input".
    //
    // EVERYTHING IN THE PAYLOAD IS PLAIN OWNED DATA. `link.program` is null by construction:
    // the whole point is that a hit never has a glslang arena to point into. Both structs
    // were audited field by field - the only member that ever pointed into glslang-owned
    // memory was `program` itself, and TUniformInitializer / XfbVarying, which look like
    // glslang types, are std::string + std::vector aggregates.
    struct ProgramTranslationResult {
        // program == nullptr, always. Asserted at insert.
        ProgramObject::LinkArtifacts link;
        ProgramObject::SpirvArtifacts spirv;
    };
    using ProgramTranslationResultPtr = SharedPtr<const ProgramTranslationResult>;

    SizeT ProgramTranslationResultBytes(const ProgramTranslationResult& result);

    // Process-global, and safe to be: the FRONT-END environment fingerprint is in the key
    // (see CompileEnv::frontendFingerprint), so a program built under one context's glslang
    // limits can never be handed to a context with different ones - while two contexts on
    // DIFFERENT GPUs that agree on those limits deliberately share entries.
    //
    // Global rather than per-context because the producer runs on a ShaderCompilePool worker
    // and must not reach MG_State::pGLContext.
    MG_Util::ShaderTranspiler::BoundedTranslationCache<ProgramTranslationResult>&
    GetProgramTranslationCache();

    void ClearProgramTranslationCache();
    void LogProgramTranslationCacheStats();
} // namespace MobileGL::MG_State::GLState
