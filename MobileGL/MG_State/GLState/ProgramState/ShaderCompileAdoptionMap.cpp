// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderCompileAdoptionMap.h"

#include "ShaderCompileTask.h"

namespace MobileGL::MG_State::GLState {
    SharedPtr<ShaderCompileTask> ShaderCompileAdoptionMap::FindAdoptable(const ShaderStage stage,
                                                                        const Uint64 sourceHash, const String& source,
                                                                        const Uint64 envFingerprint) {
        const ShaderSourceKey key{.stage = stage,
                                  .sourceHash = sourceHash,
                                  .sourceLength = source.length(),
                                  .envFingerprint = envFingerprint};

        const auto it = m_entries.find(key);
        if (it == m_entries.end()) return nullptr;

        SharedPtr<ShaderCompileTask> node = it->second.lock();
        // Expired (every shader object that held it has released it), settled as Cancelled
        // (the enqueue lost a race with teardown, or the body threw), or CANCELLATION
        // REQUESTED but not yet settled (a releaser fired Cancel() while a worker was still
        // inside RunBody(), so the node is stuck at Running until the body returns - see
        // JobNode::Run: once m_cancelled is set, the node is DOOMED to end up Cancelled no
        // matter how the body finishes, it just has not gotten there yet). All three can
        // never publish artifacts a caller may rely on, so all three are misses. Only the
        // first two are dead weight worth pruning from the index here - a cancellation-
        // requested-but-still-running node is still reachable from its own (about to
        // release) ShaderObject and will get pruned once it actually settles, so leave the
        // entry alone and just refuse to hand this node out.
        if (!node || node->IsCancelled()) {
            m_entries.erase(it);
            return nullptr;
        }
        if (node->IsCancellationRequested()) return nullptr;

        // Never let correctness ride on a 64-bit hash. Lengths already matched (they are part
        // of the key), so this is a plain memcmp - and it is the ONLY thing that authorizes
        // two GL shader names to share one compile.
        if (*node->source != source) return nullptr;

        ++m_adoptionCount;
        return node;
    }

    void ShaderCompileAdoptionMap::Register(const SharedPtr<ShaderCompileTask>& node) {
        if (!node) return;

        SweepIfCrowded();
        // operator[] rather than a find/insert pair: an existing entry for this key is either
        // a re-registration of the same source (the previous node expired or was cancelled)
        // or an astronomically rare hash collision. The newcomer wins in both cases.
        m_entries[ShaderSourceKey{.stage = node->stage,
                                  .sourceHash = node->sourceHash,
                                  .sourceLength = node->source->length(),
                                  .envFingerprint = node->env->fingerprint}] = node;
    }

    void ShaderCompileAdoptionMap::Clear() {
        m_entries.clear();
        m_sweepThreshold = kMinSweepThreshold;
    }

    void ShaderCompileAdoptionMap::SweepIfCrowded() {
        if (m_entries.size() < m_sweepThreshold) return;

        // Collect first, erase after: the map is open-addressed and erases by shifting the
        // rest of the probe cluster into the hole, so an erase moves entries other than the
        // erased one. Copying the keys out sidesteps that entirely, and this path is cold
        // enough that the extra vector is not worth reasoning about the alternative.
        Vector<ShaderSourceKey> dead;
        for (const auto& entry : m_entries) {
            const SharedPtr<ShaderCompileTask> node = entry.second.lock();
            if (!node || node->IsCancelled()) dead.push_back(entry.first);
        }
        for (const ShaderSourceKey& key : dead) {
            m_entries.erase(key);
        }

        // Amortization: after a sweep the map holds exactly the nodes still reachable from
        // some shader object, so letting it double before the next sweep makes the whole
        // scheme O(1) per Register() while keeping the map O(live nodes).
        m_sweepThreshold = std::max(kMinSweepThreshold, m_entries.size() * 2);
    }
} // namespace MobileGL::MG_State::GLState
