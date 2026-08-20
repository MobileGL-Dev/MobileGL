// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderCompileTask.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Async/JobNode.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_State/GLState/ProgramState/ShaderPreprocessCache.h>

namespace MobileGL::MG_State::GLState {
    // glslang has no "detach this thread" API in the vendored revision, but TShader::parse
    // leaves the calling thread's TLS pool allocator pointing at the shader's own pool and
    // never restores it. Left there, the next allocation this thread makes - in an unrelated
    // job, or in glslang code reached from a different object - would come out of a pool the
    // GL thread may already have deleted with the TShader. SetThreadPoolAllocator(nullptr)
    // reverts the thread to its own thread_local default and is the documented idiom.
    //
    // A scope guard, so it also runs when a body throws. Declared here rather than kept
    // file-local because stage 4 gave it a second user: ProgramLinkTask's body parses (the
    // claim-CAS loser's re-parse), links and emits SPIR-V, all on a pool thread.
    struct GlslangThreadAllocatorGuard {
        GlslangThreadAllocatorGuard() = default;
        ~GlslangThreadAllocatorGuard();
        GlslangThreadAllocatorGuard(const GlslangThreadAllocatorGuard&) = delete;
        GlslangThreadAllocatorGuard& operator=(const GlslangThreadAllocatorGuard&) = delete;
    };

    // Everything one glCompileShader PRODUCES, in one block.
    //
    // This is exactly the set a single run of the compile pipeline writes, which is what
    // makes "discard the artifacts" a complete invalidation and "move the artifacts" a
    // complete publish. It lives on the job node rather than on ShaderObject: a worker fills
    // it in, and the GL thread reads it through ShaderObject's join gate.
    struct ShaderCompileArtifacts {
        // The CompileEnv snapshot this compile ran against. Held so the consume-once
        // re-parse in ClaimParsedShader() reproduces the original parse exactly, instead of
        // re-reading whatever the backend says now.
        SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
        // The parse, WHEN THIS COMPILE ACTUALLY PARSED - and null otherwise, including when
        // compileStatus is true.
        //
        // That combination is not a half-finished compile; it is an L1c hit. The translation
        // memo's compile half (TranslationCache.h) knows this exact (stage, preprocessed
        // source, front-end env) parses cleanly, so the verdict is published without running
        // glslang. What a hit cannot hand over is the TShader itself: mapIO mutates its
        // aliased intermediate at link, so a parse feeds exactly ONE link and could never
        // have been shared between compiles.
        //
        // Nothing on the compile side of GL reads this - GL_COMPILE_STATUS, the info log,
        // GL_SHADER_SOURCE, attach/detach and reuse across programs are all answered from the
        // fields below. The one reader is ClaimParsedShader, which treats null as "parse it
        // now", which is the same path the consume-once CAS loser has always taken.
        SharedPtr<glslang::TShader> shader;
        // The source the parse actually consumed (after PreprocessShaderSource), kept for
        // ClaimParsedShader's re-parse so a later link never depends on the preprocessor
        // being deterministic across backend-state changes.
        String preprocessedSource;
        UnorderedMap<String, Int> explicitUniformLocations;
        UnorderedMap<String, Uint> explicitOpaqueBindings;
        String infoLog;
        Bool compileStatus = false;
    };

    // The unit of asynchronous shader compilation: one glCompileShader's worth of pure CPU
    // work - preprocess, the two lexical rejections, the two lexical extractions, and (unless
    // the translation memo's compile half already knows the answer) the glslang parse - with
    // every input it needs owned by the node itself.
    //
    // That ownership is the whole point. The node reads no GL-thread state (the source is a
    // SharedPtr<const String> snapshot, the device limits come from the CompileEnv snapshot,
    // the P0b cross-object memo is shared-owned and internally locked) and writes nothing
    // but its own `artifacts`. So a node whose ShaderObject was re-sourced, deleted, or
    // destroyed while it was still running is safe to simply abandon - no wait, no
    // synchronization with the GL thread beyond the node's own terminal state.
    class ShaderCompileTask final : public MG_Util::Async::JobNode {
    public:
        ShaderCompileTask(const ShaderStage stage, SharedPtr<const String> source, const Uint64 sourceHash,
                          SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env,
                          SharedPtr<ShaderPreprocessCache> cache, const Uint externalIndex)
            : stage(stage), source(Move(source)), sourceHash(sourceHash), env(Move(env)), cache(Move(cache)),
              externalIndex(externalIndex) {}

        // ---- inputs: immutable after construction, all owned by the node ----
        const ShaderStage stage;
        // The exact text at enqueue. ShaderObject compares this pointer against its own
        // m_source to decide whether its layer-1 memo is armed, which is why glShaderSource
        // only ever swaps the pointer when the text genuinely differs.
        const SharedPtr<const String> source;
        const Uint64 sourceHash;
        const SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv> env;
        // P0b layer 2, or null. Null is the "no context" case (the default fragment shader,
        // the backends' internal blit/mipmap shaders) and doubles as the marker for
        // "compile inline regardless of the async flag" - see ShaderObject::Compile().
        const SharedPtr<ShaderPreprocessCache> cache;
        const Uint externalIndex; // logs only

        // ---- output: valid iff IsComplete(), immutable afterwards ----
        ShaderCompileArtifacts artifacts;

        // Hands out a link-consumable TShader, parsing one on demand when this node has none.
        //
        // TWO WAYS TO GET HERE WITHOUT A STORED PARSE, and they share one implementation:
        //   * the CAS loser. glslang's mapIO mutates the TShader's aliased intermediate, so
        //     the parse this node produced may feed exactly ONE link; every later link (a
        //     relink, or the same shader attached to a second program) needs a fresh one. The
        //     claim is a CAS on this shared node rather than a flag on the ShaderObject
        //     because from stage 4 the two callers can be two ProgramLinkTasks on two
        //     workers: two programs sharing one shader, linked back to back. Copying the
        //     parse out and tracking consumed-ness per program would let both of them decide
        //     they were the first, run mapIO over the same intermediate twice, and ship
        //     silently corrupt SPIR-V.
        //   * an L1c HIT. The compile published a verdict without parsing at all (see
        //     ShaderCompileArtifacts::shader), so this call IS the parse - deferred out of
        //     glCompileShader to the first link that genuinely needs an AST. A link served
        //     from L1 never gets here, which is the whole point: that program's front end
        //     never constructs a glslang object of any kind.
        //
        // Either way the parse runs over artifacts.preprocessedSource against THIS node's own
        // CompileEnv (not against whatever the backend reports now), through the identical
        // CompileShader path - so every claimant produces byte-identical SPIR-V. Callable
        // only once IsComplete() and compileStatus are true. Returns null only if that parse
        // fails, and outReparseLog then carries its diagnostics.
        //
        // Const because the claim is the node's own synchronization, not a mutation of its
        // published artifacts: a claim that is taken and then abandoned (its link was
        // cancelled) costs one extra re-parse later and nothing else.
        SharedPtr<glslang::TShader> ClaimParsedShader(String& outReparseLog) const;

        // Sticky marker for "a ProgramLinkTask has this node in its input snapshot".
        //
        // It exists to keep a cancel from eating a result someone still needs. A pending link
        // holds its dependencies by SharedPtr, so the NODE always outlives the ShaderObject -
        // but Cancel() is not about lifetime, it discards the result. The reachable sequence
        // is the ordinary one: compile, attach, glLinkProgram (enqueued), glDetachShader,
        // glDeleteShader. The detach makes the shader GL-invisible, so the delete frees its
        // name, and ReleaseShaderNameIfOrphaned would cancel a compile the enqueued link is
        // waiting on - turning a link that must report GL_TRUE into GL_FALSE. Set on the GL
        // thread in Link()'s prologue, read on the GL thread by
        // ShaderObject::ReleaseCompileNode - which from stage 6 weighs it together with the
        // adopter count below, because a node can now have both kinds of observer at once.
        //
        // Never cleared: the worst case is one stale node compiling to completion for nobody,
        // which is exactly what the pre-stage-3 implementation always did.
        void MarkLinkReferenced() { m_linkReferenced.store(true, std::memory_order_release); }
        Bool IsLinkReferenced() const { return m_linkReferenced.load(std::memory_order_acquire); }

        // ---- P1 stage 6: the adopter count ----
        // How many live ShaderObjects currently hold this node in their m_compiled.
        //
        // It exists because stage 6 lets a node be SHARED: before it, a node had exactly one
        // shader object, so "this object stopped caring" and "nothing can observe this
        // result" were the same statement and ShaderObject::CancelCompile could cancel
        // unconditionally. Once two GL shader names hold one node, that cancel would kill the
        // other one's pending compile - a compile that must still report GL_TRUE. So a cancel
        // is now authorized by TWO conditions, both checked by the releaser:
        //   * this release brings the count to zero (no shader object is left), AND
        //   * IsLinkReferenced() is false (no enqueued link took the node into its snapshot).
        // The second is the stage-4 pin, unchanged; the first is what stage 6 adds.
        //
        // ---- Why a plain Int and not an atomic ----
        // Every mutation is made from ShaderObject, and every ShaderObject mutation site is a
        // GL entry point on the application's context thread: glCompileShader (adopt/create),
        // glShaderSource with different text, glDeleteShader's orphan sweep, and
        // ~ShaderObject. All of them are the SAME thread, so the count is never concurrently
        // mutated and an atomic would only buy an unneeded lock prefix on the hottest compile
        // path. Workers cannot touch it by construction: a job body's entire contract (see
        // this class's header comment) is that it reads only the node's inputs and writes only
        // `artifacts`, and a plain Int here makes that contract grep-checkable in a way an
        // atomic would quietly hide.
        //
        // The CANCEL that the count authorizes still races the worker, and deliberately so -
        // that is the settled cancel-not-join semantics from stage 3: JobNode::Cancel is
        // cooperative and non-blocking, a node already running settles as Cancelled when its
        // body returns, and a node that has already gone terminal ignores the request.
        // Nothing about that changes here.
        //
        // Exactness under that race: ShaderObject::ReleaseCompileNode returns EARLY, without
        // decrementing and without dropping its reference, when the node is already terminal
        // (there is nothing left to stop). Terminality is sticky, so if a releaser observes a
        // node as NON-terminal then no holder has ever taken that early return on it, and the
        // count it reads is exactly the number of holders. If the worker finishes in the
        // window between that observation and the Cancel(), the Cancel is a no-op on a
        // terminal node - and the count was zero, so there was no other holder to harm.
        void AddAdopter() { ++m_adopters; }
        void ReleaseAdopter() {
            MOBILEGL_ASSERT(m_adopters > 0,
                            "ShaderCompileTask adopter count underflow; a ShaderObject released a node it did not "
                            "hold (every release must pair with exactly one AddAdopter)");
            --m_adopters;
        }
        Int AdopterCount() const { return m_adopters; }

    private:
        void RunBody() override;
        // The real body; RunBody wraps it so a throw becomes a GL-visible compile failure.
        void RunCompilePipeline();

        mutable std::atomic<Bool> m_parseClaimed{false};
        std::atomic<Bool> m_linkReferenced{false};
        // GL-thread-owned; see AddAdopter above for why this is not an atomic.
        Int m_adopters = 0;
    };
} // namespace MobileGL::MG_State::GLState
