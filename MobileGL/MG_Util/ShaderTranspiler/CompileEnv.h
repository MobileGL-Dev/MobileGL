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
    // GL_MAX_COMPUTE_WORK_GROUP_COUNT / _SIZE core minimums (GL 4.6 core table 23.45), in ONE
    // place because three separate readers have to agree on them: CaptureCompileEnv (which floors
    // the backend's answer at them), GL_Getter (which answers the same query the same way) and
    // BuildTBuiltInResource (whose gl_MaxComputeWorkGroup* constants a shader compares against
    // the query - KHR-GL43.compute_shader.max does exactly that). They used to be three copies,
    // and the z one disagreed: glslang compiled against 1024 while the context advertised 64.
    inline constexpr Uint MIN_COMPUTE_WORK_GROUP_COUNT[3] = {65535, 65535, 65535};
    inline constexpr Uint MIN_COMPUTE_WORK_GROUP_SIZE[3] = {1024, 1024, 64};
    // GL_MAX_COMPUTE_UNIFORM_COMPONENTS, the same invariant with no backend input: the number
    // glGetIntegerv answers and the number gl_MaxComputeUniformComponents expands to.
    inline constexpr Int MAX_COMPUTE_UNIFORM_COMPONENTS = 1024;

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
        Uint maxComputeWorkGroupSize[3] = {MIN_COMPUTE_WORK_GROUP_SIZE[0], MIN_COMPUTE_WORK_GROUP_SIZE[1],
                                           MIN_COMPUTE_WORK_GROUP_SIZE[2]};
        // GL_MAX_COMPUTE_WORK_GROUP_COUNT, likewise. Carried for the same reason the size is:
        // gl_MaxComputeWorkGroupCount expands from it at parse time, so the compile pipeline
        // needs the number the context advertises without reaching back to the live backend.
        Uint maxComputeWorkGroupCount[3] = {MIN_COMPUTE_WORK_GROUP_COUNT[0], MIN_COMPUTE_WORK_GROUP_COUNT[1],
                                            MIN_COMPUTE_WORK_GROUP_COUNT[2]};
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
        // THE LINE THIS DRAWS. "Backend-agnostic" means BACKEND IDENTITY is out - the vendor,
        // the extension list, which of DirectGLES/DirectVulkan is active, every capability bit
        // that merely steers the transpile. It does NOT mean backend-DERIVED VALUES are out: a
        // resource limit that glslang enforces at parse, or expands into a built-in constant,
        // is a front-end INPUT no matter where the number came from, and dropping it would be
        // a silent miscompile rather than a backend leak. A driver with 16 vertex attribs and
        // one with 32 genuinely reflect the same GLSL differently.
        //
        // WHAT IS IN IT (audited; re-audit whenever a new env read appears in the front end):
        //   * the DynamicBackendParameters fields BuildTBuiltInResource copies into
        //     TBuiltInResource - MaxImageUnits, MaxDrawBuffers, MaxVertexImageUniforms,
        //     MaxGeometryImageUniforms, MaxFragmentImageUniforms, MaxComputeImageUniforms,
        //     MaxCombinedImageUniforms, MaxComputeTextureImageUnits. glslang enforces those at
        //     parse, so they decide whether a shader compiles at all and can change the link
        //     result.
        //   * maxComputeWorkGroupSize and maxComputeWorkGroupCount, all three components each.
        //     These moved IN at the dev merge that brought wave3's cb155c5b, which made
        //     BuildTBuiltInResource read them from the env instead of hardcoding a permissive
        //     cap - exactly the migration the old exclusion note said would force them in
        //     here. They are not merely a reject gate: glslang expands both into built-in
        //     CONSTANTS (gl_MaxComputeWorkGroupSize, gl_MaxComputeWorkGroupCount), so a
        //     compute module that reads one generates different SPIR-V under two drivers that
        //     report different numbers.
        //   * MaxVertexAttribs and the HasBackend() bit: the two inputs to ProgramLinkTask's
        //     GetReflectionVertexAttribLimit, which bounds how many vertex input locations
        //     reflection records - so they change the REFLECTION the memo carries.
        //
        // The sharding this costs is nil in practice and worth naming so nobody re-litigates
        // it: a process has ONE active backend at a time and CompileEnv is re-captured when
        // that changes, so no live run ever has two of these fingerprints competing for the
        // same L1 entries. The cost would only appear on a future cross-device DISK tier,
        // where it is the correct cost - those devices really do compile that GLSL differently.
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
        //   * maxComputeWorkGroupInvocations - and ONLY this one; its two former companions
        //     moved into the list above at the wave3 merge. glslang has no
        //     gl_MaxComputeWorkGroupInvocations built-in and BuildTBuiltInResource does not
        //     read this field, so its sole consumer is still ValidateComputeLocalSizeLimits, a
        //     pre-parse ACCEPT/REJECT gate. A rejected shader fails its compile, so its
        //     program never reaches the tail of the link and no L1 entry is ever created under
        //     a rejecting environment; an accepted one parses identically at any value.
        //     THIS ONE IS A REACHABILITY ARGUMENT, NOT AN INDEPENDENCE ONE, and it is now the
        //     only such argument left in this classification. The moment anything hands this
        //     value to glslang - a TBuiltInResource field, a built-in constant - it MUST move
        //     into the fingerprint, exactly as its companions just did.
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
