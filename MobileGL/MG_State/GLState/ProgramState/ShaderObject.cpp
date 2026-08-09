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
        // an old source. Give up our claim on it - it owns its own copy of that old string,
        // so swapping the pointer below cannot race its storage. Note "our claim", not "the
        // job": another shader object may have adopted the same node and still be waiting for
        // exactly this answer, which is what ReleaseCompileNode's count discipline protects.
        ReleaseCompileNode();
        m_source = MakeShared<const String>(source);
        InvalidateCompiledState();
    }

    void ShaderObject::SetShaderSource(String&& source) {
        if (SourceMatchesCompiledState(source)) return;
        ReleaseCompileNode();
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
        //
        // ApplyDeferredDiagnostics DRAINS, so a node shared by several shader objects
        // (stage 6) replays its worker-side log line exactly once, at whichever object joins
        // first. That is the honest report - one compile ran - and it is log text only: the
        // GL-observable half of a failure, COMPILE_STATUS and the info log, lives in
        // `artifacts` and every sharer reads the identical copy of it.
        MG_Util::Async::ApplyDeferredDiagnostics(*m_compiled);
        // A node that settled as Cancelled published nothing. Dropping it here is what keeps
        // the object's state machine to two reachable cases - "no job" and "a job that
        // completed" - so every reader below can treat a live node as authoritative.
        //
        // Through DropCompileNode, not a bare reset: this object is letting the node go, so
        // its adopter slot has to go with it. A node shared with another object stays alive
        // and gets dropped once more when that object joins - once per holder, never twice
        // for the same one, because DropCompileNode is null-guarded.
        if (!m_compiled->IsComplete()) DropCompileNode();
    }

    void ShaderObject::AdoptCompileNode(SharedPtr<ShaderCompileTask> node) const {
        // Never overwrite a hold without giving its slot back first.
        DropCompileNode();
        m_compiled = Move(node);
        m_compiled->AddAdopter();
        // Re-arm the join gate: whether this node was just created or just adopted from
        // another object, THIS object has not pulled its result yet. (An adopted node may
        // already be terminal - the join then only replays what is left of its diagnostics.)
        m_compileJoined = false;
    }

    void ShaderObject::DropCompileNode() const {
        if (!m_compiled) return;
        m_compiled->ReleaseAdopter();
        m_compiled.reset();
    }

    void ShaderObject::InvalidateCompiledState() {
        // The job node holds exactly what one Compile() produces, so discarding it IS the
        // invalidation - and it re-arms nothing, so the next Compile() genuinely recompiles.
        DropCompileNode();
    }

    void ShaderObject::ReleaseCompileNode() {
        if (!m_compiled) return;
        // Already terminal: there is nothing left to stop, so this is not a release at all -
        // the node and this object's claim on it both stay. That early return is older than
        // stage 6 and it is load-bearing: ProgramState::ReleaseShaderNameIfOrphaned calls
        // this on a shader whose name is going away but whose object a ProgramObject may
        // still hold, and dropping a COMPLETED compile there would turn that program's link
        // into GL_FALSE.
        if (m_compiled->IsTerminal()) return;
        // Two independent claimants have to be checked before a cancel, and this object is
        // authorized to cancel only if BOTH say the result has become unobservable.
        //
        // 1. Other shader objects. From stage 6 a node can be SHARED by several GL shader
        //    names that were handed byte-identical source; cancelling here would turn a
        //    compile they must still see as GL_TRUE into GL_FALSE. Only the releaser that
        //    takes the count to zero - i.e. the last holder - may cancel. See
        //    ShaderCompileTask::AddAdopter for why a plain Int is sound here and for the
        //    exactness argument under the worker race.
        // 2. A pending LINK. An enqueued ProgramLinkTask holds the node in its input snapshot
        //    and a cancel would turn its link into GL_FALSE; reached by the ordinary
        //    link-then-detach-then-delete shader teardown. See MarkLinkReferenced. Never
        //    cleared, so this is a one-way pin.
        //
        // The cancel itself is cooperative and non-blocking, exactly as before: a node no
        // worker has picked up settles immediately, a running one is flagged and settles when
        // its body returns, writing only into itself the whole time.
        if (m_compiled->AdopterCount() == 1 && !m_compiled->IsLinkReferenced()) m_compiled->Cancel();
        DropCompileNode();
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

        // Two reasons to stay on this thread, one rule. Without the async flag the whole
        // path must be byte-identical to the synchronous implementation, and a cache-less
        // object is an internal shader that compiles and reads its status in the same
        // breath (see the constructor comment) - a job would only add a round trip.
        // AsyncShaderCompileActive(), not ...Enabled(): a glMaxShaderCompilerThreadsKHR(0)
        // has to put compilation back on this thread even though the extension is still
        // advertised, and that is exactly what makes the GL_COMPLETION_STATUS_KHR the
        // extension mandates after a zero count (immediately GL_TRUE) fall out for free.
        //
        // Hoisted above the node construction because stage 6 keys off it too: this same
        // answer decides whether the adoption map is consulted at all, so a
        // glMaxShaderCompilerThreadsKHR(0) and a flag-off build both bypass sharing exactly
        // as they bypass the pool, and their behaviour stays byte-identical to pre-stage-6.
        const Bool runOnPool = m_preprocessCache && MG_Util::Async::AsyncShaderCompileActive();

        // The compile-environment snapshot is taken HERE, on the GL thread, and handed to
        // the job. Everything the pipeline needs to know about the device comes through it,
        // never through pActiveBackendObject - that is what makes the body movable.
        const SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env =
            MG_Util::ShaderTranspiler::GetCurrentCompileEnv();
        const Uint64 sourceHash = ShaderPreprocessCache::HashSource(*m_source);

        // ---- P1 stage 6: adopt an equivalent compile instead of enqueueing a duplicate ----
        // ~21% of all glCompileShader calls in the shaderpack corpus are a DIFFERENT shader
        // object handed byte-identical source. P0b's memo only pays off once one of them has
        // finished; under async they are all enqueued in the same burst, so without this each
        // one runs the whole pipeline on its own worker. The map hands back the node the
        // first of them created - in flight or already complete - and this object simply
        // holds it too.
        if (runOnPool && m_adoptionMap) {
            if (SharedPtr<ShaderCompileTask> shared =
                    m_adoptionMap->FindAdoptable(m_stage, sourceHash, *m_source, env->fingerprint)) {
                // Take the node's own source snapshot as ours. FindAdoptable just compared
                // the two strings in full, so this changes nothing observable - but it is not
                // optional: the layer-1 memo (HasMemoizedCompile) is a POINTER comparison
                // against the node's snapshot, so leaving our own equal-but-distinct copy in
                // place would make the very next glCompileShader on this object decide it had
                // no memo and enqueue the duplicate this whole stage exists to avoid - and
                // would make an identical glShaderSource re-source cancel a shared compile.
                // It also collapses N copies of a ~100 KB shaderpack stage into one.
                m_source = shared->source;
                AdoptCompileNode(Move(shared));
                return;
            }
        }

        AdoptCompileNode(MakeShared<ShaderCompileTask>(m_stage, m_source, sourceHash, env, m_preprocessCache,
                                                       m_externalIndex));

        if (!runOnPool) {
            m_compiled->RunInline();
            // Inline means the node is already terminal, so this join only replays
            // diagnostics; it is here so the synchronous and asynchronous paths publish
            // through the identical code.
            EnsureCompileJoined();
            return;
        }
        // Registered BEFORE the post, so the very next glCompileShader in this burst can
        // adopt it however fast a worker picks it up. Registration is an index entry only -
        // the map holds a WeakPtr and never keeps a node alive.
        if (m_adoptionMap) m_adoptionMap->Register(m_compiled);
        MG_Util::Async::ShaderCompilePool::Get().Post(m_compiled);
    }

    void ShaderObject::MarkAsDeleted() {
        m_deleteStatus = true;
    }
} // namespace MobileGL::MG_State::GLState
