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

            // ---- GL_ARB_gl_spirv ----
            // glShaderBinary(GL_SHADER_BINARY_FORMAT_SPIR_V): the object stops standing for a
            // GLSL source and starts standing for an application-supplied SPIR-V module. The
            // module is held verbatim until glSpecializeShader names an entry point for it -
            // ARB_gl_spirv makes the pair a two-step operation, and glCompileShader in between is
            // INVALID_OPERATION rather than a compile of anything.
            //
            // Both directions clear the other: glShaderSource on a SPIR-V shader takes it back to
            // being a GLSL shader with GL_SPIR_V_BINARY reading FALSE, which the conformance suite
            // checks explicitly.
            void SetSpirvBinary(Vector<Uint32>&& binary);
            Bool HasSpirvBinary() const { return m_hasSpirvBinary; }
            // ARB_gl_spirv: "Once specialized, a shader may not be re-specialized without first
            // re-associating the original SPIR-V module with it, through ShaderBinary." A second
            // glSpecializeShader is GL_INVALID_OPERATION, and this latch is what answers that.
            //
            // Set ONLY on the success path. A specialization that FAILED did not specialize the
            // shader, and the conformance suite relies on that distinction: it deliberately fails
            // specialization (a bad entry point, then an unknown constant id) on one shader object
            // and then requires the next, well-formed call on that same object to be accepted.
            Bool HasBeenSpecialized() const { return m_specialized; }
            const Vector<Uint32>& GetSpirvBinary() const { return m_spirvBinary; }
            // glSpecializeShader's half: hand the object the GLSL its module specializes to and
            // let the ordinary pipeline compile it.
            void SpecializeFromSpirv(String&& glsl, Vector<String>&& xfbVaryings, GLenum xfbBufferMode);
            // The capture the object's SPIR-V module DECLARED, as the equivalent
            // glTransformFeedbackVaryings request. Empty for a GLSL shader and for a SPIR-V module
            // that declares no transform feedback. ProgramObject::Link picks this up from the
            // program's last vertex-processing stage, because ARB_gl_spirv makes decorations the
            // only declaration form for a SPIR-V program and glTransformFeedbackVaryings has no
            // effect on one.
            const Vector<String>& GetSpirvXfbVaryings() const { return m_spirvXfbVaryings; }
            GLenum GetSpirvXfbBufferMode() const { return m_spirvXfbBufferMode; }
            // What glGetShaderSource / GL_SHADER_SOURCE_LENGTH must answer. A shader created from
            // glShaderBinary never had glShaderSource called on it, so GL 4.6 core 7.1 makes its
            // source the empty string - even after glSpecializeShader, when m_source holds the
            // SPIRV-Cross GLSL the module was translated into. That text is MobileGL's, not the
            // application's, and handing it back invites an application to cache and re-submit it.
            const String& GetApplicationShaderSource() const;
            // The other half: specialization itself failed (a bad entry point, a constant id the
            // module does not declare, a module spirv-val rejects). There is nothing to compile,
            // so the verdict is recorded directly - COMPILE_STATUS false with this log - and both
            // queries answer from it without touching the compile pipeline.
            void RecordSpecializationFailure(String&& infoLog);
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
            const String& GetInfoLog() const {
                return m_specializationFailed ? m_specializationInfoLog : Compiled().infoLog;
            }
            // Explicit layout(location = N) qualifiers on this shader's default-block
            // uniforms, as glslang recorded them at the point its Vulkan-relaxed remap
            // discarded them (see CollectExplicitUniformLocations).
            const UnorderedMap<String, Int>& GetExplicitUniformLocations() const {
                return Compiled().explicitUniformLocations;
            }
            Bool GetCompileStatus() const { return m_specializationFailed ? false : Compiled().compileStatus; }
            Bool GetDeleteStatus() const { return m_deleteStatus; }

            // Blocks until a pending compile has published its artifacts. Public for the
            // sites that must join without reading anything - ProgramState::
            // JoinAllPendingWork, the glMaxShaderCompilerThreadsKHR(0) path that settles
            // every outstanding job. glLinkProgram deliberately does NOT come through
            // here: its prologue takes the nodes unjoined via CompiledNodeForLink().
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

            // MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS's one-story-per-compile memory. The
            // three optimistic getter sites in GL_Program ask THIS instead of a raw
            // IsCompileComplete() peek, and the difference is the latch: without it, a job
            // that settles between two adjacent queries hands the application a torn pair -
            // an empty info log from the optimistic read, then the real GL_FALSE from the
            // truthful one - and an application that aborts on that status never reaches
            // the link join that quotes the real log. So the first optimistic answer
            // latches: until the next AdoptCompileNode/DropCompileNode this object keeps
            // answering optimistically even after the job settles, and a real failure
            // surfaces exactly once, at the link. Returns whether the caller should answer
            // optimistically; the caller has already checked the quirk is active.
            Bool TakeOptimisticCompileAnswer() const {
                if (!m_optimisticAnswerLatched && IsCompileComplete()) return false;
                m_optimisticAnswerLatched = true;
                return true;
            }

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
            // TakeOptimisticCompileAnswer's memory: this object has answered a compile
            // query optimistically for the current node. Cleared wherever the node
            // changes hands (AdoptCompileNode) or goes away (DropCompileNode).
            mutable Bool m_optimisticAnswerLatched = false;
            // The application-supplied SPIR-V module and the flag GL_SPIR_V_BINARY reports. The
            // module is kept after specialization too: glSpecializeShader may legally run again on
            // the same object with different constants, and the second call has to re-specialize
            // the ORIGINAL words rather than the ones the first call folded.
            Vector<Uint32> m_spirvBinary;
            Bool m_hasSpirvBinary = false;
            // "This shader has been specialized"; see HasBeenSpecialized. Cleared by anything that
            // re-associates a module (SetSpirvBinary) or turns the object back into a GLSL shader
            // (either SetShaderSource overload) - which is exactly the re-association ARB_gl_spirv
            // names as the way to make a second specialization legal again.
            Bool m_specialized = false;
            Vector<String> m_spirvXfbVaryings;
            GLenum m_spirvXfbBufferMode = GL_INTERLEAVED_ATTRIBS;
            // A specialization that failed before any compile could start. Kept beside the
            // compile artifacts rather than inside them because there is no compile job to hang
            // it on - see RecordSpecializationFailure. Cleared by anything that gives the object
            // a new meaning (a new source, a new module, a fresh specialization).
            Bool m_specializationFailed = false;
            String m_specializationInfoLog;
        };
    } // namespace MG_State::GLState
} // namespace MobileGL
