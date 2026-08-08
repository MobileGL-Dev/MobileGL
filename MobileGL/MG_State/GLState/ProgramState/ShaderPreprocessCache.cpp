// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderPreprocessCache.h"

namespace MobileGL::MG_State::GLState {
    const ShaderPreprocessResult* ShaderPreprocessCache::Find(const ShaderStage stage, const Uint64 sourceHash,
                                                              const String& source) const {
        const Key key{.stage = stage, .sourceHash = sourceHash, .sourceLength = source.length()};
        const auto it = m_index.find(key);
        if (it == m_index.end()) return nullptr;

        // Never let correctness ride on a 64-bit hash: confirm the hit byte for byte.
        // Lengths already matched (they are part of the key), so this is a plain memcmp.
        const Entry& entry = *it->second;
        if (entry.originalSource != source) return nullptr;

        return &entry.result;
    }

    void ShaderPreprocessCache::Insert(const ShaderStage stage, const Uint64 sourceHash, const String& source,
                                       ShaderPreprocessResult result) {
        const SizeT entryBytes = EntryBytes(source, result);
        // A single source bigger than the whole budget would evict every other entry and
        // then itself; refuse it instead of thrashing the cache empty.
        if (entryBytes > kMaxStoredSourceBytes) return;

        const Key key{.stage = stage, .sourceHash = sourceHash, .sourceLength = source.length()};
        if (const auto existing = m_index.find(key); existing != m_index.end()) {
            // Either a re-insert of the same source (harmless) or a genuine hash collision
            // with a different source. Both are resolved by letting the newcomer win: one
            // entry per key keeps the index a plain map, and a collision is astronomically
            // rare enough that the loser simply misses.
            EraseEntry(existing->second);
        }

        m_entries.push_back(Entry{.key = key, .originalSource = source, .result = Move(result)});
        m_index[key] = std::prev(m_entries.end());
        m_storedSourceBytes += entryBytes;

        EvictUntilWithinBudget();
    }

    void ShaderPreprocessCache::Clear() {
        m_entries.clear();
        m_index.clear();
        m_storedSourceBytes = 0;
    }

    void ShaderPreprocessCache::EraseEntry(const EntryList::iterator it) {
        const SizeT bytes = EntryBytes(it->originalSource, it->result);
        m_storedSourceBytes = bytes > m_storedSourceBytes ? 0 : m_storedSourceBytes - bytes;
        m_index.erase(it->key);
        m_entries.erase(it);
    }

    void ShaderPreprocessCache::EvictUntilWithinBudget() {
        // FIFO: the oldest insertion goes first. Insert() already refuses entries larger
        // than the byte budget, so this loop always terminates with at least the entry
        // that was just added still resident.
        while (!m_entries.empty() &&
               (m_entries.size() > kMaxEntries || m_storedSourceBytes > kMaxStoredSourceBytes)) {
            EraseEntry(m_entries.begin());
        }
    }
} // namespace MobileGL::MG_State::GLState
