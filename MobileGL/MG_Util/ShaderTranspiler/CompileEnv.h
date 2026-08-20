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
