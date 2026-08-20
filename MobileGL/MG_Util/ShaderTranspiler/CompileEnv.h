// MobileGL - MobileGL/MG_Util/ShaderTranspiler/CompileEnv.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <Config.h>
#include <MG_Backend/BackendObject.h>

namespace MobileGL::MG_Util::ShaderTranspiler {
    // everything outside (stage, source) this reads - advertised extensions and backend limits -
    // so the transformation is a pure function of its three arguments and can run on a worker
    // thread.
    //
    // Why it exists (P1): every one of those reads is a reach-back into
    // MG_Backend::pActiveBackendObject / gBackendFunctionsTable, and one of them
    // (GL_MAX_COMPUTE_WORK_GROUP_SIZE) is a *real driver call* that on the DirectGLES
    // backend silently no-ops off the context thread - which would turn a perfectly legal
    // `local_size_z` into COMPILE_STATUS=FALSE the moment compilation moved to a worker.
    // Snapshotting the whole set once per context, on the GL thread, removes every
    // reach-back at once and makes the pipeline a pure function of (stage, source, env).
    //
    // Lifetime: captured lazily on first use by GLState::GLContext::GetCompileEnv(), and
    // RE-captured if the active backend object changes. Immutable once published; held by
    // value/`SharedPtr<const CompileEnv>` so a worker can never observe a torn update.
    //
    // Memo-hazard rule: `fingerprint` hashes every member above it and is part of the P0b
    // ShaderPreprocessCache key, so a memo computed against one env can never be returned
    // against another. ADDING A FIELD HERE MEANS ADDING IT TO ComputeFingerprint().
    struct CompileEnv {
        // --- compute limits: the ONLY former real-driver read in the pipeline ---
        // GL_MAX_COMPUTE_WORK_GROUP_SIZE, already max()'d with the frontend minimum.
        Uint maxComputeWorkGroupSize[3] = {1024, 1024, 64};
        // GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, likewise.
        Uint64 maxComputeWorkGroupInvocations = 1024;

        // --- backend identity + limits ---
        // Unknown means "no backend was active at capture time". Every consumer keeps the
        // exact no-backend fallback it had before: extensions read as advertised, limits
        // read as the frontend defaults.
        BackendType backend = BackendType::Unknown;
        MG_Backend::DynamicBackendParameters params{};   // by value, never by reference
        Vector<GLExtension> advertisedExtensions;

        Uint64 fingerprint = 0;   // set by CaptureCompileEnv()

        // The FRONT-END half of the environment: the subset of the fields above that can
        // change what glslang PRODUCES - the SPIR-V or the reflection - as opposed to what a
        // BACKEND later does with the result. This, and never `fingerprint`, is what the L1
        // shader translation memo keys on, because L1 is backend-agnostic BY CONTRACT: two
        // contexts on different GPUs compiling the same GLSL must share one L1 entry.
        //
        // WHAT IS IN IT (audited; re-audit whenever a new env read appears in the front end):
        //   * the seven DynamicBackendParameters fields BuildTBuiltInResource actually copies
        //     into TBuiltInResource - MaxImageUnits, MaxDrawBuffers, MaxVertexImageUniforms,
        //     MaxGeometryImageUniforms, MaxFragmentImageUniforms, MaxComputeImageUniforms,
        //     MaxCombinedImageUniforms. glslang enforces those at parse, so they decide
        //     whether a shader compiles at all and can change the link result.
        //   * MaxVertexAttribs and the HasBackend() bit: the two inputs to ProgramLinkTask's
        //     GetReflectionVertexAttribLimit, which bounds how many vertex input locations
        //     reflection records - so they change the REFLECTION the memo carries.
        // Both are backend-DERIVED but front-end-CONSUMED. Dropping them would be a
        // miscompile, not a backend leak: a driver with 16 vertex attribs and one with 32
        // genuinely reflect the same GLSL differently.
        //
        // WHAT IS DELIBERATELY OUT:
        //   * `backend` beyond the HasBackend() bit. Nothing in the parse, the link or
        //     GlslangToSpv branches on which backend is active - ShaderAttrib::flags is 0 on
        //     both production parse paths (ShaderCompileTask::RunCompilePipeline and
        //     ClaimParsedShader). Backend identity steers the TRANSPILE, which is L2's key.
        //   * `advertisedExtensions`. Its only front-end consumer is ShaderSourceProcessor's
        //     FilterUnsupportedGpuShaderInt64, which REWRITES THE SOURCE TEXT - and the
        //     preprocessed text is in the L1 key verbatim, a strictly finer discriminator
        //     than the extension list. (E_GL_ARB_gpu_shader_fp64 is never read by the front
        //     end at all: MOBILEGL_ADVERTISE_FP64 only adds it to the extension STRING the
        //     application queries, and DemoteFloat64Pass runs unconditionally either way, so
        //     fp64 GLSL translates identically with the flag on or off.)
        //   * the other ~50 DynamicBackendParameters fields: read by the GL getters and by
        //     the backends, never by the parse, the link or reflection.
        //   * maxComputeWorkGroupSize / maxComputeWorkGroupInvocations. Consumed ONLY by
        //     ValidateComputeLocalSizeLimits, a pre-parse ACCEPT/REJECT gate. A rejected
        //     shader fails its compile, so its program never reaches the tail of the link and
        //     no L1 entry is ever created under a rejecting environment; an accepted one
        //     produces the same SPIR-V under any limits, because BuildTBuiltInResource
        //     HARDCODES the compute maxima instead of reading these.
        //     THIS ONE IS A REACHABILITY ARGUMENT, NOT AN INDEPENDENCE ONE. If the TODO in
        //     BuildTBuiltInResource ("Drive glslang compute resource limits from the active
        //     backend") is ever done, these MUST move into this fingerprint.
        Uint64 frontendFingerprint = 0;   // set by CaptureCompileEnv()

        Bool HasBackend() const { return backend != BackendType::Unknown; }
        // Matches the historical rule exactly: with no active backend every extension counts
        // as advertised, because the frontend then has nothing to gate against.
        Bool IsExtensionAdvertised(GLExtension extension) const {
            if (!HasBackend()) return true;
            return std::find(advertisedExtensions.begin(), advertisedExtensions.end(), extension) !=
                   advertisedExtensions.end();
        }
    };

    // Hashes every semantically relevant member. Public so a test can assert that two
    // different envs really do produce different P0b cache keys.
    Uint64 ComputeCompileEnvFingerprint(const CompileEnv& env);

    // The backend-agnostic half; see CompileEnv::frontendFingerprint for the classification
    // and the evidence behind each call. Public so a test can assert both directions: that a
    // backend-only difference produces the SAME value (which is what pins L1's
    // backend-agnosticism) and that a front-end limit produces a different one.
    Uint64 ComputeFrontendCompileEnvFingerprint(const CompileEnv& env);

    // GL thread only: this is where the GL_MAX_COMPUTE_WORK_GROUP_SIZE queries live now.
    SharedPtr<const CompileEnv> CaptureCompileEnv();

    // The env a context-less caller gets: exactly what CaptureCompileEnv() would produce
    // with no active backend. Used by the unit tests that drive the transpiler directly and
    // by the internal shader objects that compile before any context exists.
    const SharedPtr<const CompileEnv>& GetDefaultCompileEnv();

    // The env of the current GL context, or GetDefaultCompileEnv() when there is none.
    // GL thread only (it may trigger a capture). This is the compatibility shim for the
    // handful of entry points that still resolve their env implicitly; the pipeline itself
    // always takes an explicit `const CompileEnv&`.
    const SharedPtr<const CompileEnv>& GetCurrentCompileEnv();
} // namespace MobileGL::MG_Util::ShaderTranspiler
