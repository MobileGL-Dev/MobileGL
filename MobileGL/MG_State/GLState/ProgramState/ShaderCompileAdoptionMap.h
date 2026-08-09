// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/ProgramState/ShaderSourceKey.h>

namespace MobileGL::MG_State::GLState {
    class ShaderCompileTask;

    // P1 stage 6: the per-context index of compile job nodes that a NEW shader object may
    // adopt instead of enqueueing a duplicate of.
    //
    // Why it is not the P0b preprocess cache. That cache only helps once a compile has
    // FINISHED - it memoizes the source-only half of the pipeline, and a worker consults it
    // from inside the job body. Under asynchronous compilation the dominant shape is
    // different: a shaderpack load hands N different shader objects byte-identical source
    // within the same GL-thread burst (measured across bsl/complementary/bliss, ~21% of all
    // Compile() calls are such cross-object duplicates), and all N are enqueued before any of
    // them completes. Every one of those workers then misses the cache, runs the whole
    // pipeline, and races the others to insert the same entry. This map closes that window on
    // the GL thread, at enqueue: the second object through takes the FIRST object's node.
    //
    // What "adopt" means: the two shader objects end up holding the same SharedPtr in their
    // m_compiled. They are two distinct GL names with two distinct info-log/COMPILE_STATUS
    // queries, but both queries read one set of artifacts - which is exactly right, because
    // the pipeline is a pure function of the key below and the full source text. Nothing is
    // copied and no worker ever waits (P1 invariant I4 is untouched: this only ever REMOVES
    // work from the pool). The single consume-once resource, the glslang parse, is already
    // guarded for sharing by ShaderCompileTask::ClaimParsedShader's CAS, which stage 4 built
    // for exactly this shape - one node, several links.
    //
    // ---- Threading: GL thread only, and therefore lock-free ----
    // Every entry point below is reached from glCompileShader (ShaderObject::Compile) and
    // from nowhere else. That is one GL entry point on the application's context thread, so
    // the map needs no mutex, unlike the preprocess cache which several workers hit at once.
    // The weak pointers are the ONLY thing this class stores, precisely so it can never keep
    // a node - or the artifacts a node owns - alive past its last real holder.
    //
    // ---- Lifetime and pruning ----
    // WeakPtr, never SharedPtr: the map is an index, not an owner. An entry whose node has
    // been released by every shader object simply expires, and a node that was CANCELLED
    // carries no result at all, so both are treated as misses and pruned where they are
    // found. Pruning is otherwise amortized: Register() sweeps the whole map whenever it has
    // grown past twice its size at the last sweep, which bounds the map at O(live nodes)
    // without a per-call cost.
    class ShaderCompileAdoptionMap {
    public:
        // Never sweep below this: a shaderpack burst is a few hundred distinct sources, and
        // an entry is a key plus a weak pointer.
        static constexpr SizeT kMinSweepThreshold = 256;

        // The adoptable node for this exact source under this exact environment, or null.
        //
        // A hit is honored only after the FULL source text has been compared byte for byte
        // against the candidate node's own snapshot: the hash in the key is a lookup
        // accelerator, never the answer (ShaderSourceKey). A node that has settled as
        // Cancelled is never handed out - it published nothing, so adopting it would give the
        // new object a compile that can never report anything but GL_FALSE. Nor is a node
        // whose cancellation has merely been REQUESTED but not yet settled (still Running,
        // with IsCancellationRequested() true): JobNode::Run forces such a node to Cancelled
        // the moment its body returns regardless of how the body finished, so it is already
        // doomed and handing it out would just move the same GL_FALSE-with-no-log outcome to
        // a second, unrelated shader object.
        //
        // A COMPLETED node is adoptable, and deliberately so: the new object gets the right
        // answer for zero work, which is the same deal the P0b cache offers one layer down.
        SharedPtr<ShaderCompileTask> FindAdoptable(ShaderStage stage, Uint64 sourceHash, const String& source,
                                                   Uint64 envFingerprint);

        // Indexes `node` as the adoptable one for its key. A key already present is
        // overwritten: the newcomer is at least as fresh as whatever was there, and one entry
        // per key keeps this a plain map.
        void Register(const SharedPtr<ShaderCompileTask>& node);

        void Clear();

        // ---- diagnostics only; nothing in the GL frontend branches on these ----
        // Monotonic count of nodes handed out by FindAdoptable, i.e. of glCompileShader calls
        // that did NOT enqueue a job because an equivalent one already existed. Tests read it
        // as a delta across a burst.
        Uint64 GetAdoptionCount() const { return m_adoptionCount; }
        SizeT GetEntryCount() const { return m_entries.size(); }

    private:
        void SweepIfCrowded();

        UnorderedMap<ShaderSourceKey, WeakPtr<ShaderCompileTask>, ShaderSourceKeyHasher> m_entries;
        SizeT m_sweepThreshold = kMinSweepThreshold;
        Uint64 m_adoptionCount = 0;
    };
} // namespace MobileGL::MG_State::GLState
