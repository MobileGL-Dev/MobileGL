// MobileGL - MobileGL/MG_Test/State/ObjectLifetimeIdTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The invariant every backend memo keyed on a state object now rests on: a heap
// ADDRESS is not an identity, a lifetime id is.
//
// DirectVulkan memoises resolved vertex bindings per VertexArrayObject and folds
// the bound BufferObject's identity into the content hash that validates them.
// Both used to be heap addresses, and the allocator hands a freed address
// straight back: a VAO and a vertex buffer destroyed and immediately recreated
// under a byte-identical attribute layout reproduced BOTH the memo key and the
// validating hash, so the new draw fetched the destroyed buffer's GPU slice.
// GetLifetimeId() is what makes that impossible, so it is worth a test that
// needs no GPU, no context and no driver - only the allocator.
//
// The test does not simulate reuse; it waits for the real allocator to do it
// (which a LIFO free-list does on the very next allocation) and then asserts the
// id differs. If the allocator never repeats an address the run proves nothing,
// and the case says so with a skip rather than passing quietly.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "Includes.h"

#include <MG_State/GLState/BufferState/BufferObject.h>
#include <MG_State/GLState/VertexArrayState/VertexArrayObject.h>

using namespace MobileGL;

namespace {

    // The allocation must actually happen: C++ permits eliding a new/delete pair,
    // and an elided one would let two objects share an address for reasons that
    // have nothing to do with the allocator - which is the only thing under test
    // here. Publishing every pointer through a volatile sink keeps the pairs.
    void* volatile g_addressSink = nullptr;

    // Constructs and destroys `ObjectT` on the heap kAttempts times, watching for
    // the allocator to hand back an address it already used. Every repeat must
    // carry a lifetime id the dead occupant did not have. Returns how many repeats
    // were seen, so the caller can tell "proven" from "never got the chance".
    //
    // Each object type has its own id counter, so a VertexArrayObject and a
    // BufferObject may well both be id 1; ids are only ever compared within a
    // type, which is exactly how the memos use them.
    template <typename ObjectT>
    int ProbeLifetimeIdAcrossAddressReuse(const char* typeName) {
        constexpr int kAttempts = 64;

        std::unordered_map<std::uintptr_t, Uint64> idAtAddress;
        int reuseCount = 0;
        Uint64 previousId = 0;

        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            auto object = std::make_unique<ObjectT>(0u);
            g_addressSink = object.get();
            const auto address = reinterpret_cast<std::uintptr_t>(object.get());
            const Uint64 lifetimeId = object->GetLifetimeId();

            // 0 is the "this slot holds nothing" value in every memo that stores an
            // id, so a live object must never be able to answer to a zeroed slot.
            EXPECT_NE(lifetimeId, 0u) << typeName << " handed out lifetime id 0 (attempt " << attempt
                                      << "), which is the value a zero-initialised memo slot already carries";
            EXPECT_GT(lifetimeId, previousId)
                << typeName << " lifetime ids must be strictly increasing, so an id is never handed out twice "
                << "(attempt " << attempt << ")";
            previousId = lifetimeId;

            const auto inserted = idAtAddress.emplace(address, lifetimeId);
            if (!inserted.second) {
                // The allocator reproduced an address: this is precisely the state in
                // which a memo keyed on the address alone would hit a dead object's
                // entry. The id is the thing that has to say no.
                ++reuseCount;
                EXPECT_NE(lifetimeId, inserted.first->second)
                    << typeName << " reconstructed at the address of a destroyed one reports the DEAD object's "
                    << "lifetime id - a backend memo keyed on it would serve the dead object's resolved state "
                    << "to this object's draws (attempt " << attempt << ")";
                inserted.first->second = lifetimeId;
            }

            // Freed before the next construction on purpose: that ordering is what
            // makes the allocator reuse the block, and it is the ordering the GL
            // workload has (glDeleteVertexArrays, then the next glGenVertexArrays).
            object.reset();
        }

        return reuseCount;
    }

    // Guards against a degenerate "id" that is really just the address in disguise:
    // objects alive at the same time must differ too.
    template <typename ObjectT>
    void ExpectDistinctIdsWhileBothAlive(const char* typeName) {
        auto first = std::make_unique<ObjectT>(0u);
        auto second = std::make_unique<ObjectT>(0u);
        g_addressSink = first.get();
        g_addressSink = second.get();
        EXPECT_NE(first->GetLifetimeId(), second->GetLifetimeId())
            << "two live " << typeName << "s share a lifetime id";
    }

} // namespace

TEST(ObjectLifetimeIdTest, VertexArrayObjectAtARecycledAddressCarriesAFreshLifetimeId) {
    using MG_State::GLState::VertexArrayObject;
    const int reuseCount = ProbeLifetimeIdAcrossAddressReuse<VertexArrayObject>("VertexArrayObject");
    if (reuseCount == 0) {
        GTEST_SKIP() << "inconclusive, not proven: this allocator never handed the same address back across 64 "
                        "construct/destroy rounds, so the recycled-address case was never exercised";
    }
    RecordProperty("address_reuses_observed", reuseCount);
}

TEST(ObjectLifetimeIdTest, BufferObjectAtARecycledAddressCarriesAFreshLifetimeId) {
    using MG_State::GLState::BufferObject;
    const int reuseCount = ProbeLifetimeIdAcrossAddressReuse<BufferObject>("BufferObject");
    if (reuseCount == 0) {
        GTEST_SKIP() << "inconclusive, not proven: this allocator never handed the same address back across 64 "
                        "construct/destroy rounds, so the recycled-address case was never exercised";
    }
    RecordProperty("address_reuses_observed", reuseCount);
}

TEST(ObjectLifetimeIdTest, LiveVertexArrayObjectsHaveDistinctLifetimeIds) {
    ExpectDistinctIdsWhileBothAlive<MG_State::GLState::VertexArrayObject>("VertexArrayObject");
}

TEST(ObjectLifetimeIdTest, LiveBufferObjectsHaveDistinctLifetimeIds) {
    ExpectDistinctIdsWhileBothAlive<MG_State::GLState::BufferObject>("BufferObject");
}
