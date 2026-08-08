// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderObject.h"
#include "ShaderPreprocessCache.h"
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

namespace MobileGL::MG_State::GLState {
    void ShaderObject::SetShaderSource(const String& source) {
        // P0b layer 1. glShaderSource always REPLACES the source, but replacing it with a
        // byte-identical one cannot change what a compile would produce: the whole
        // pipeline (preprocess -> lexical checks -> glslang parse) is a pure function of
        // (stage, source, CompileEnv). So keeping the compiled state is not an optimization
        // that changes observable behaviour - the COMPILE_STATUS, the info log and the
        // reflection a caller can query are exactly what a real recompile would have
        // rebuilt, byte for byte. A compile still IN FLIGHT is left running for the same
        // reason: it is computing the right answer for text this object still holds.
        if (SourceMatchesCompiledState(source)) return;
        // The text genuinely changed, so whatever a running job is computing is now about
        // an old source. Drop it where it stands - it owns its own copy of that old string,
        // so swapping the pointer below cannot race its storage.
        CancelCompile();
        m_source = MakeShared<const String>(source);
        InvalidateCompiledState();
    }

    void ShaderObject::SetShaderSource(String&& source) {
        if (SourceMatchesCompiledState(source)) return;
        CancelCompile();
        m_source = MakeShared<const String>(Move(source));
        InvalidateCompiledState();
    }

    Bool ShaderObject::SourceMatchesCompiledState(const String& candidate) const {
        // The memo is armed exactly while a job exists that was built from the string this
        // object still points at - pending or finished, success or failure.
        if (!HasMemoizedCompile()) return false;
        if (candidate.length() != m_source->length()) return false;
        // Never let correctness ride on a hash: the answer is the full text comparison.
        // (The stored hash on the node is a cache-lookup accelerator, not a substitute.)
        return candidate == *m_source;
    }

    void ShaderObject::JoinPendingCompile() const {
        MOBILEGL_ASSERT(!MG_Util::Async::ShaderCompilePool::IsPoolThread(),
                        "ShaderObject::EnsureCompileJoined() reached from a pool thread; a job body must never read "
                        "GL-thread-owned objects");
        m_compiled->Wait();
        m_compileJoined = true;
        // Errors and worker-side log lines are raised HERE, on the GL thread, at the first
        // join of the job that produced them - which for a single shader is trivially the
        // order a serial implementation would have produced them in.
        MG_Util::Async::ApplyDeferredDiagnostics(*m_compiled);
        // A node that settled as Cancelled published nothing. Dropping it here is what keeps
        // the object's state machine to two reachable cases - "no job" and "a job that
        // completed" - so every reader below can treat a live node as authoritative.
        if (!m_compiled->IsComplete()) m_compiled.reset();
    }

    void ShaderObject::InvalidateCompiledState() {
        // The job node holds exactly what one Compile() produces, so discarding it IS the
        // invalidation - and it re-arms nothing, so the next Compile() genuinely recompiles.
        m_compiled.reset();
    }

    void ShaderObject::CancelCompile() {
        if (!m_compiled || m_compiled->IsTerminal()) return;
        // Cooperative and non-blocking. A node that no worker has picked up settles
        // immediately; one that is running is flagged and settles when its body returns,
        // writing only into itself the whole time.
        //
        // Unless a pending LINK is waiting on it. Cancelling is about discarding a result
        // nothing can observe any more, and this object is no longer the only route to this
        // one: an enqueued ProgramLinkTask holds the node as a dependency, and a cancel would
        // turn its link into GL_FALSE. Reached by the ordinary link-then-detach-then-delete
        // shader teardown - see ShaderCompileTask::MarkLinkReferenced. Dropping our own
        // reference is still right; the link keeps the node alive and finishes it.
        if (!m_compiled->IsLinkReferenced()) m_compiled->Cancel();
        m_compiled.reset();
    }

    void ShaderObject::Compile() {
        // P0b layer 1, as a tri-state: the memo is "the node in m_compiled was built from
        // the string m_source still points at". SetShaderSource only swaps that pointer when
        // the text actually differs, so this is a pointer compare, and it covers Pending as
        // well as Complete - a second glCompileShader on an in-flight object is a no-op, not
        // a duplicate job racing to write the same fields.
        //
        // The failure case is covered too: the info log stays queryable because nothing is
        // cleared. And if the stored TShader already fed a link, the no-op leaves
        // preprocessedSource and both side-channel maps intact, which is precisely what
        // ClaimParsedShader's on-demand re-parse needs - a real recompile would have handed
        // the next link a fresh parse, the no-op hands it a fresh re-parse of the identical
        // source instead. Same result, one parse either way.
        if (HasMemoizedCompile()) return;

        // The compile-environment snapshot is taken HERE, on the GL thread, and handed to
        // the job. Everything the pipeline needs to know about the device comes through it,
        // never through pActiveBackendObject - that is what makes the body movable.
        m_compiled = MakeShared<ShaderCompileTask>(m_stage, m_source, ShaderPreprocessCache::HashSource(*m_source),
                                                   MG_Util::ShaderTranspiler::GetCurrentCompileEnv(),
                                                   m_preprocessCache, m_externalIndex);
        m_compileJoined = false;

        // Two reasons to stay on this thread, one rule. Without the async flag the whole
        // path must be byte-identical to the synchronous implementation, and a cache-less
        // object is an internal shader that compiles and reads its status in the same
        // breath (see the constructor comment) - a job would only add a round trip.
        if (!m_preprocessCache || !MG_Util::Async::AsyncShaderCompileEnabled()) {
            m_compiled->RunInline();
            // Inline means the node is already terminal, so this join only replays
            // diagnostics; it is here so the synchronous and asynchronous paths publish
            // through the identical code.
            EnsureCompileJoined();
            return;
        }
        MG_Util::Async::ShaderCompilePool::Get().Post(m_compiled);
    }

    void ShaderObject::MarkAsDeleted() {
        m_deleteStatus = true;
    }
} // namespace MobileGL::MG_State::GLState
