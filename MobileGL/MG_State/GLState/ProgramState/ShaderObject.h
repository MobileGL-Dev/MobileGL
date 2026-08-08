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
            ShaderObject(const ShaderStage stage, Uint externalIndex,
                         SharedPtr<ShaderPreprocessCache> preprocessCache = nullptr)
                : m_stage(stage), m_externalIndex(externalIndex), m_preprocessCache(Move(preprocessCache)) {}
            // Cancel-not-join: the node owns its inputs, so an in-flight compile whose
            // object just went away is safe to abandon where it stands. Nothing can observe
            // its result any more - this object was the only route to it.
            ~ShaderObject() { CancelCompile(); }

            ShaderObject(const ShaderObject&) = delete;
            ShaderObject& operator=(const ShaderObject&) = delete;

            void SetShaderSource(const String& source);
            void SetShaderSource(String&& source);
            void Compile();
            // Drops a compile that is still in flight, without waiting for it. Called at the
            // points where the object's compiled state stops being observable: a real source
            // change, and the release of an orphaned shader name.
            void CancelCompile();
            void MarkAsDeleted();

            // Hands out a link-consumable TShader. glslang's mapIO mutates the TShader's
            // aliased intermediate, so the parse stored by Compile() may feed exactly one
            // link; every later link (relink, or the same shader attached to a second
            // program) gets a fresh parse of the stored preprocessed source through the
            // byte-identical CompileShader path (including the legacy-460 retry). Only
            // callable while GetCompileStatus() is true. Returns null only if that
            // re-parse fails - outReparseLog then carries its diagnostics.
            SharedPtr<glslang::TShader> TakeShaderForLink(String& outReparseLog);

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
            SharedPtr<const String> m_source = EmptySource();

            // P0b layer 2: the owning context's cross-object memo, or null. Internally
            // locked, because several workers hit it at once.
            const SharedPtr<ShaderPreprocessCache> m_preprocessCache;

            Bool m_deleteStatus = false;

            // ---- Compile OUTPUT ---- pending OR completed; reachable only through Compiled().
            // Mutable because the join is a read-side operation: a const getter has to be
            // able to settle an outstanding job before answering.
            mutable SharedPtr<ShaderCompileTask> m_compiled;
            // Exactly-once latch for the pull above. Armed with every new job node, set by
            // the one join that consumes it.
            mutable Bool m_compileJoined = false;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
