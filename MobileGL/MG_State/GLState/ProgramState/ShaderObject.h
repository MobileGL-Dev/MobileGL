// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderStage.h>
#include <MG_State/GLState/ProgramState/ShaderCompileTask.h>
#include <MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.h>

namespace MobileGL {
    namespace MG_State::GLState {
        // The GL-visible shader name. It owns the source text and one compile job node; the
        // job node owns everything a compile produces.
        //
        // Every member below is GL-thread-owned, and every read of worker-produced state
        // goes through Compiled(), which joins first. That is invariant I5 of the P1 design:
        // because Compiled() is the SOLE accessor of the node's artifacts, the compiler
        // enumerates every reader for us and none can be forgotten.
        class ShaderObject {
        public:
            // `preprocessCache` is the owning context's cross-object memo (P0b layer 2).
            // Null is fully supported and means two things at once: "no sharing", and
            // "compile inline, never on a worker". Those coincide exactly - the only
            // cache-less shader objects are the internal ones (ProgramObject's default
            // fragment shader, the DirectVulkan blit and depth-mipmap shaders) and every one
            // of them compiles and reads its status in the same breath, so a job would only
            // add a round trip. Shared ownership rather than a raw pointer: a compile job
            // outlives neither the object nor the context deterministically, and the cache
            // has to stay alive for whoever is still reading it.
            //
            // `adoptionMap` is the same context's stage-6 index of adoptable compile nodes.
            // It is non-null exactly when `preprocessCache` is (ProgramState hands both out
            // together, and nobody else hands out either), which is what makes "no cache"
            // keep meaning "compile inline, share nothing": an internal shader object has
            // neither, so it neither adopts nor registers and its path is byte-identical to
            // the pre-stage-6 one. GL-thread-only, so unlike the cache it carries no lock -
            // shared ownership only because a ShaderObject may outlive the context's tables.
            ShaderObject(const ShaderStage stage, Uint externalIndex,
                         SharedPtr<ShaderPreprocessCache> preprocessCache = nullptr,
                         SharedPtr<ShaderCompileAdoptionMap> adoptionMap = nullptr)
                : m_stage(stage), m_externalIndex(externalIndex), m_preprocessCache(Move(preprocessCache)),
                  m_adoptionMap(Move(adoptionMap)) {}
            // Cancel-not-join: the node owns its inputs, so an in-flight compile whose
            // object just went away is safe to abandon where it stands. Nothing can observe
            // its result any more - unless another shader object adopted the same node, or a
            // link pinned it, which is precisely what ReleaseCompileNode() checks.
            ~ShaderObject() {
                ReleaseCompileNode();
                // ReleaseCompileNode KEEPS a node that has already gone terminal - there is
                // nothing left to stop, so it is not a release at all. This object is going
                // away regardless, so hand the adopter slot back here. That is what keeps
                // ShaderCompileTask::AdopterCount() exactly "how many live ShaderObjects hold
                // this node" instead of merely an upper bound.
                DropCompileNode();
            }

            ShaderObject(const ShaderObject&) = delete;
            ShaderObject& operator=(const ShaderObject&) = delete;

            void SetShaderSource(const String& source);
            void SetShaderSource(String&& source);
            void Compile();
            // Gives up this object's claim on its compile node, cancelling the node only if
            // this object was its LAST claimant. Called at the points where the object's
            // compiled state stops being observable through THIS name: a real source change,
            // and the release of an orphaned shader name.
            //
            // Named for what it does rather than for what it used to do: before stage 6 a
            // node had exactly one shader object, so giving up the claim and cancelling the
            // compile were the same act and this was CancelCompile(). They are not the same
            // act any more - see ShaderCompileTask::AddAdopter for the count discipline and
            // its single-threadedness argument. Never waits, in either case.
            void ReleaseCompileNode();
            void MarkAsDeleted();

            // The compile job node itself, for ProgramObject::Link()'s input snapshot.
            // DELIBERATELY DOES NOT JOIN, and that is the entire point of stage 4: the link
            // takes the node as a dependency and is posted only once the node is terminal,
            // so glLinkProgram never blocks on glCompileShader. Null means this object has
            // never been compiled (or its last compile was abandoned), which the link reads
            // as COMPILE_STATUS false - the same verdict the joining path produces.
            //
            // The caller must MarkLinkReferenced() whatever it keeps: from here on the node's
            // result has an observer this object knows nothing about (see the marker's
            // comment in ShaderCompileTask.h).
            const SharedPtr<ShaderCompileTask>& CompiledNodeForLink() const { return m_compiled; }

            Uint GetExternalIndex() const { return m_externalIndex; }
            ShaderStage GetShaderStage() const { return m_stage; }
            // No join: the source is GL-thread-owned, and a worker only ever reads the
            // immutable snapshot it was handed at enqueue.
            const String& GetShaderSource() const { return *m_source; }
            // The snapshot itself, for whoever needs to hand it to a job.
            const SharedPtr<const String>& GetShaderSourcePtr() const { return m_source; }

            const SharedPtr<glslang::TShader>& GetCompiledShader() const { return Compiled().shader; }
            const String& GetInfoLog() const { return Compiled().infoLog; }
            // Explicit layout(location = N) qualifiers on this shader's default-block
            // uniforms, captured lexically at Compile() because the relaxed parse drops
            // them from reflection (see ExtractExplicitUniformLocations).
            const UnorderedMap<String, Int>& GetExplicitUniformLocations() const {
                return Compiled().explicitUniformLocations;
            }
            // Explicit layout(binding = N) on sampler/image uniforms - their initial
            // texture/image units - captured lexically for the same reason (see
            // ExtractExplicitOpaqueBindings).
            const UnorderedMap<String, Uint>& GetExplicitOpaqueBindings() const {
                return Compiled().explicitOpaqueBindings;
            }
            Bool GetCompileStatus() const { return Compiled().compileStatus; }
            Bool GetDeleteStatus() const { return m_deleteStatus; }

            // Blocks until a pending compile has published its artifacts. Public for the
            // sites that must join without reading anything - ProgramObject::Link's
            // prologue, which needs every attached shader settled before it runs.
            void JoinCompile() const { EnsureCompileJoined(); }

            // True while this object holds the outcome (success OR failure) of a Compile()
            // of exactly the source it currently holds - i.e. while the P0b layer-1 memo is
            // armed and a glCompileShader would be a no-op. Diagnostics and tests only;
            // nothing in the GL frontend branches on it.
            //
            // Tri-state, and deliberately NOT joining: an in-flight compile of the current
            // source counts as memoized (a second glCompileShader must not enqueue a
            // duplicate job), but asking that question must never block.
            // A node that settled as Cancelled (the job body threw, or the enqueue failed)
            // carries no result, so it must NOT satisfy the memo: otherwise a second
            // glCompileShader on the same source enqueues nothing and the eventual join
            // reports GL_FALSE forever. The synchronous path retries in exactly this case.
            Bool HasMemoizedCompile() const {
                return m_compiled != nullptr && m_compiled->source == m_source && !m_compiled->IsCancelled();
            }

            // MUST NOT JOIN - this is what GL_COMPLETION_STATUS_KHR will read when the
            // extension surface lands. "No job at all" counts as complete: there is nothing
            // outstanding to wait for.
            Bool IsCompileComplete() const { return m_compiled == nullptr || m_compiled->IsTerminal(); }

        private:
            // ---- The one and only join gate for compile output (P1 invariant I5) ----
            // The fast path - no job, or a job whose result this object has already pulled -
            // is two predictable branches and stays inline: it runs on every Compiled() read
            // and the project never builds with LTO, so an out-of-line body would be a real
            // cross-TU call at each of those sites. The blocking half is out of line.
            //
            // The gate keys on "has this object pulled the job's result yet", NOT on "is the
            // job terminal". Those differ in the case that matters: a worker can finish a
            // compile before the GL thread ever looks at it, and the pull is where deferred
            // diagnostics get replayed and an abandoned node gets dropped. Keying on
            // terminality would silently skip both.
            void EnsureCompileJoined() const {
                if (m_compiled && !m_compileJoined) JoinPendingCompile();
            }
            void JoinPendingCompile() const;

            // The artifacts of a compile that ran to completion. A node that was abandoned
            // (cancelled at teardown, or whose body threw) never publishes: JoinPendingCompile
            // drops it, so anything reachable here is either Complete or absent, and "absent"
            // reads as the never-compiled defaults - COMPILE_STATUS false, empty info log,
            // which is exactly what GL requires before the first glCompileShader.
            static const ShaderCompileArtifacts& EmptyArtifacts() {
                static const ShaderCompileArtifacts empty;
                return empty;
            }
            const ShaderCompileArtifacts& Compiled() const {
                EnsureCompileJoined();
                return m_compiled ? m_compiled->artifacts : EmptyArtifacts();
            }

            void InvalidateCompiledState();

            // ---- the ONLY two writers of m_compiled (P1 stage 6) ----
            // Every adopter-count mutation lives in these two, which is what makes "exactly
            // one AddAdopter per hold, exactly one ReleaseAdopter per hold" auditable rather
            // than something review has to re-derive at each call site. DropCompileNode is
            // null-guarded, so calling it on an object that already let go is a no-op and a
            // double release is unrepresentable.
            void AdoptCompileNode(SharedPtr<ShaderCompileTask> node) const;
            void DropCompileNode() const;

            // ---- P0b layer 1: per-object no-op recompile ----
            // True iff `candidate` is byte-identical to the source that produced (or is
            // producing) the compiled state this object currently holds.
            Bool SourceMatchesCompiledState(const String& candidate) const;

            // ---- GL-thread-owned state: never produced by a compile, so it never joins ----
            const ShaderStage m_stage;
            const Uint m_externalIndex = 0;
            // The pre-glShaderSource state, shared by every untouched object rather than
            // allocated per glCreateShader.
            static const SharedPtr<const String>& EmptySource() {
                static const SharedPtr<const String> empty = MakeShared<const String>();
                return empty;
            }
            // glShaderSource text, as an immutable snapshot. Never null. A job holds its own
            // SharedPtr to the exact string it was given, so replacing the source under a
            // running compile cannot race its storage - and the layer-1 memo collapses to a
            // pointer comparison against the job's snapshot, because the setter only swaps
            // the pointer when the text genuinely differs.
            //
            // Not necessarily unique to this object from stage 6 on: adopting a node also
            // takes that node's source snapshot (see Compile()), so N shader objects sharing
            // one compile share one copy of the text. The string is immutable and shared-
            // owned, so that is invisible to every reader.
            SharedPtr<const String> m_source = EmptySource();

            // P0b layer 2: the owning context's cross-object memo, or null. Internally
            // locked, because several workers hit it at once.
            const SharedPtr<ShaderPreprocessCache> m_preprocessCache;
            // P1 stage 6: the owning context's index of adoptable compile nodes, or null.
            // Touched only from Compile(), i.e. only on the GL thread, so it carries no lock.
            const SharedPtr<ShaderCompileAdoptionMap> m_adoptionMap;

            Bool m_deleteStatus = false;

            // ---- Compile OUTPUT ---- pending OR completed; reachable only through Compiled().
            // Mutable because the join is a read-side operation: a const getter has to be
            // able to settle an outstanding job before answering.
            //
            // SHARED from stage 6 on: several shader objects holding byte-identical source
            // under the same CompileEnv point at one node. Every read below still goes
            // through the same join gate, and a second joiner finds the node already
            // terminal, so nothing about the read path changes - only the release path does
            // (ReleaseCompileNode).
            mutable SharedPtr<ShaderCompileTask> m_compiled;
            // Exactly-once latch for the pull above. Armed with every new job node, set by
            // the one join that consumes it.
            mutable Bool m_compileJoined = false;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
