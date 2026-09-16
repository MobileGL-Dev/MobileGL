// MobileGL - MobileGL/MG_Remote/Server/StagedShadow.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// R-11 - THE SERVER'S OWN COPY OF THE STAGED BYTES. Owner: package v1.
//
// GLESBufferResource::hostBytes (Managers.h:839) is the tree's ONE violation of rule C ("an
// applier entry point may not hold a pointer past its return"): Ops_H_SubData (Managers.cpp:
// 1980-1983) and Ops_H_FlushRange (:2035) record the client's shadow base and six later drains
// read it (:2000, :2062, :2080, :2111, :2741, :2843). In monolith that is correct - the bytes
// belong to a frontend object that outlives the call. Under split the pointer names SEG_STAGE,
// which is valid only until retiredSeq passes the record that named it, and w1's
// MOBILEGL_IPC_AUDIT=1 fills retired staging bytes with 0xDD precisely so an implementation
// that kept the pointer is DISTINGUISHABLE from one that copied. So this copies.
//
// THE SNAPSHOT EXTENT IS EXACTLY WHAT THE RECORD DECLARED, NEVER WIDENED (the integrator's
// ruling on b1's open M-6 half). Widening looked free once before and was not: a page-aligned
// INVALIDATE_RANGE clobbered GPU-written data - an SSBO counter beside the app's SubData - with
// stale shadow bytes (Managers.cpp:1126-1129). Tier 1's INVALIDATE_RANGE is an ASSERTION that
// the old bytes are dead, so declaring bytes covered that nothing staged is not a missing
// optimisation, it is a silent data loss. The coverage set below is therefore exact, and a
// drain that reaches outside it is Fatal rather than a re-read of whatever happens to be there.
//
// WHY IT IS A HEADER AND NOT A BLOCK INSIDE Managers.cpp. Two reasons, and the second is the
// one that matters. Managers.h is package b1's, so v1's R-11 edit may not add a member to
// GLESBufferResource and the storage has to live beside it rather than in it. And a block
// inside Managers.cpp could only ever be exercised by a test that also has a GL context, a
// resource twin and a live session - which is exactly how a rule ends up with no check that
// can fail for its own reason (R-16). Here, CopiesIntoServerStorage is a parameter rather than
// a read of MG_Config::Transport, so a unit case builds one store of each kind and asserts the
// DIFFERENCE between them.

#pragma once
#include <Includes.h>

#include <MG_Util/Debug/Log.h>
#include <MG_Util/Math/VectorTypes.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace MobileGL::MG_Remote::Server {

    // Keyed by the resource twin's ADDRESS, which is stable: the twins are heap-allocated and
    // held by SharedPtr in the backend slot table, and every event that ends a base's life -
    // an orphaning respecify, a successful map_persistent, destroy, context death - already has
    // a call site in Managers.cpp to drop it from.
    class StagedShadowStore {
    public:
        // `copies` is "this process is really split". False reproduces the monolith expression
        // character for character (`raw - offset`), which is what keeps every push and verify
        // lane byte-identical to what it was before R-11.
        explicit StagedShadowStore(Bool copies) : m_copies(copies) {}

        Bool CopiesIntoServerStorage() const { return m_copies; }

        // Copies [offset, offset+size) of the record's staged bytes into server-owned storage
        // and returns the SERVER base (offset 0 of the resource). Under monolith it returns the
        // client base unchanged and allocates nothing.
        const Uint8* Adopt(const void* key, SizeT width, const void* bytes, SizeT offset, SizeT size) {
            const auto* raw = static_cast<const Uint8*>(bytes);
            if (!m_copies) return raw - offset;
            const std::lock_guard<std::mutex> lock(m_mutex);
            Shadow& shadow = m_shadows[key];
            const SizeT needed = std::max<SizeT>(width, offset + size);
            if (shadow.Bytes.size() < needed) shadow.Bytes.resize(needed, 0);
            if (size != 0 && raw != nullptr) {
                std::memcpy(shadow.Bytes.data() + offset, raw, size);
                CoverageAdd(shadow.Covered, offset, offset + size);
            }
            m_any.store(true, std::memory_order_release);
            // Growing REALLOCATES, so every caller assigns the returned base to hostBytes on
            // the same call. No other resource's base moves: each Shadow owns its own vector,
            // and a rehash of the map MOVES that vector, which preserves its data pointer.
            return shadow.Bytes.data();
        }

        void Drop(const void* key) {
            if (!m_any.load(std::memory_order_acquire)) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows.erase(key);
        }

        void DropAll() {
            if (!m_any.load(std::memory_order_acquire)) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_shadows.clear();
        }

        // Fatal when a drain reaches bytes no record staged. It fires ONLY for a base that is
        // this key's server shadow: the legacy arm passes the frontend object's own
        // MappedData(), which is valid for the whole store and is not this rule's subject.
        void RequireCoverage(const void* key, const Uint8* hostBase, SizeT start, SizeT end,
                             const char* site) const {
            if (!m_any.load(std::memory_order_acquire) || hostBase == nullptr) return;
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_shadows.find(key);
            if (it == m_shadows.end() || it->second.Bytes.data() != hostBase) return;
            if (CoverageHas(it->second.Covered, start, end)) return;
            MGLOG_F("MGPipe: Fatal{StageSnapshotTooNarrow, \"%s\"} - the server's ladder wants "
                    "[%zu, %zu) of a buffer whose staged coverage does not include it. Under "
                    "split the authoritative shadow is SERVER-OWNED (rule C) and "
                    "resource_subdata is the only way bytes reach it, so bytes outside a staged "
                    "range have never existed on this side. Re-reading them would move zeroes "
                    "into the store, and an INVALIDATE_RANGE over them would declare live "
                    "GPU-written bytes dead (Managers.cpp:1126-1129). This is a missing record, "
                    "not a missing widening",
                    site, start, end);
            std::abort();
        }

        // Diagnostics the unit cases read, so that a check can assert WHAT HAPPENED rather than
        // that nothing blew up.
        Bool IsCovered(const void* key, SizeT start, SizeT end) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_shadows.find(key);
            if (it == m_shadows.end()) return false;
            return CoverageHas(it->second.Covered, start, end);
        }
        SizeT CoveredRunCount(const void* key) const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_shadows.find(key);
            return it == m_shadows.end() ? 0 : it->second.Covered.size();
        }
        SizeT TrackedResources() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_shadows.size();
        }

        // Is there STILL a live shadow for this key? The m-5 discriminator: after DropAll (context
        // loss) the key is erased, so a twin's cached hostBytes names freed memory and must be
        // nulled; after an ordinary Adopt the key is present and its base is live. The
        // generation-reset block runs on a twin's FIRST ensure too (the generation starts
        // mismatched), and there the shadow a preceding subdata just staged is present - so nulling
        // must key on THIS answer, not on the generation change alone, or the reduced path drops
        // its own bytes.
        Bool HasShadow(const void* key) const {
            if (!m_any.load(std::memory_order_acquire)) return false;
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_shadows.find(key) != m_shadows.end();
        }

        // Adjacent ranges merge - there is no gap between them, so the union really is one run.
        // Ranges with a gap do NOT merge, and that is the whole mechanism: it is what makes a
        // missing record detectable instead of papered over.
        static void CoverageAdd(Vector<Range1D>& covered, SizeT start, SizeT end) {
            if (start >= end) return;
            Vector<Range1D> merged;
            merged.reserve(covered.size() + 1);
            SizeT s = start;
            SizeT e = end;
            for (const auto& range : covered) {
                if (range.end < s || range.start > e) {
                    merged.push_back(range);
                    continue;
                }
                s = std::min(s, range.start);
                e = std::max(e, range.end);
            }
            merged.push_back({s, e});
            std::sort(merged.begin(), merged.end(),
                      [](const Range1D& a, const Range1D& b) { return a.start < b.start; });
            covered = std::move(merged);
        }

        static Bool CoverageHas(const Vector<Range1D>& covered, SizeT start, SizeT end) {
            if (start >= end) return true;
            for (const auto& range : covered) {
                if (range.start <= start && end <= range.end) return true;
            }
            return false;
        }

    private:
        struct Shadow {
            Vector<Uint8> Bytes;
            // Sorted, disjoint, EXACT.
            Vector<Range1D> Covered;
        };

        const Bool m_copies;
        mutable std::mutex m_mutex;
        ska::flat_hash_map<const void*, Shadow> m_shadows;
        // Read on every UploadRangeFrom, so the monolith cost is one acquire load of a
        // never-written flag rather than a mutex and a hash lookup.
        std::atomic<Bool> m_any{false};
    };

} // namespace MobileGL::MG_Remote::Server
