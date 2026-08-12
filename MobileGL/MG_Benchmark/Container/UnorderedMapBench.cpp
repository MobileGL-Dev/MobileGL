// MobileGL - MobileGL/MG_Benchmark/Container/UnorderedMapBench.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The standing performance observatory for MobileGL::UnorderedMap.
//
// This benchmarks the ALIAS, never a concrete table, so whatever UnorderedMap
// names today is what gets measured - swap the container in MG_Util/Types.h and
// re-run this same binary to get a directly comparable set of numbers. That is
// the point of it: the container sits on per-draw paths, so a change to it needs
// evidence, and the evidence should be produced the same way every time.
//
// The workloads are the shapes the tree actually exercises, not generic hash-map
// microbenchmarks. Four key shapes, because they stress a hash function very
// differently:
//   * SEQUENTIAL   dense small integers - GL object names from the index generator
//                  (buffer/texture/framebuffer/sampler registries).
//   * POINTER      real heap addresses - StateBackendObjectRegistry keys on
//                  StateObject*. These are aligned, so their low bits are the
//                  least random part of the key; a table that indexes on raw low
//                  bits clusters badly here and one that mixes first does not.
//                  Taken from the real allocator rather than a synthetic stride,
//                  which would flatter whichever table mixes its bits.
//   * DIGEST       already well-mixed 64-bit values - the XXH64 pipeline,
//                  vertex-input-state and program memos.
//   * NAME         short strings - uniform/attribute name to location maps.
//
// Sizes sweep from 8 upward because the per-draw memos are usually SMALL; a table
// that only wins at 4096 entries has not won anything that matters here.
//
// Run:  build-linux/MobileGL/MG_Benchmark/Container/UnorderedMapBench
//   or: ctest -R UnorderedMapBench     (label: benchmark)

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <benchmark/benchmark.h>

#include "MG_Util/Types.h"

using namespace MobileGL;

namespace {

    constexpr Int64 kMinSize = 8;
    constexpr Int64 kMaxSize = 4096;

    // Keep the real allocations alive for the whole process: the POINTER shape is
    // only honest if the keys are addresses the allocator actually handed out, and
    // they have to stay unique (a freed address can be handed out twice).
    std::vector<std::unique_ptr<char[]>>& PointerKeyStorage() {
        static std::vector<std::unique_ptr<char[]>> storage;
        return storage;
    }

    Vector<Uint64> SequentialKeys(SizeT n) {
        Vector<Uint64> keys;
        keys.reserve(n);
        for (SizeT i = 0; i < n; ++i) keys.push_back(static_cast<Uint64>(i) + 1);
        return keys;
    }

    Vector<Uint64> PointerKeys(SizeT n) {
        auto& storage = PointerKeyStorage();
        Vector<Uint64> keys;
        keys.reserve(n);
        std::mt19937_64 rng(0xBEEF);
        std::vector<std::unique_ptr<char[]>> churn;
        for (SizeT i = 0; i < n; ++i) {
            // State objects are not all one size, and the allocator sees other
            // traffic between them - a single uniform stride is not what this
            // registry ever sees.
            const SizeT sz = 96 + (rng() % 192);
            auto p = std::make_unique<char[]>(sz);
            keys.push_back(reinterpret_cast<Uint64>(p.get()));
            storage.push_back(std::move(p));
            if ((rng() & 3) == 0) churn.push_back(std::make_unique<char[]>(32 + (rng() % 128)));
        }
        return keys;
    }

    Vector<Uint64> DigestKeys(SizeT n) {
        Vector<Uint64> keys;
        keys.reserve(n);
        std::mt19937_64 rng(0xC0FFEE);
        for (SizeT i = 0; i < n; ++i) keys.push_back(rng());
        return keys;
    }

    Vector<String> NameKeys(SizeT n) {
        static const char* kPrefixes[] = {"u_", "a_", "mc_", "iris_", "gl_", "v_"};
        Vector<String> keys;
        keys.reserve(n);
        for (SizeT i = 0; i < n; ++i) {
            keys.push_back(String(kPrefixes[i % 6]) + "Uniform" + std::to_string(i) + "_xyz");
        }
        return keys;
    }

    // Key sets are built once per size and shared: generating them inside the timed
    // loop would measure the generator (and, for POINTER, the allocator) instead of
    // the table.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    const KeyVec& CachedKeys(SizeT n) {
        static UnorderedMap<SizeT, KeyVec> cache;
        auto it = cache.find(n);
        if (it != cache.end()) return it->second;
        return cache.emplace(n, Make(n)).first->second;
    }

    template <typename Key>
    UnorderedMap<Key, Uint64> Populated(const Vector<Key>& keys) {
        UnorderedMap<Key, Uint64> map;
        for (SizeT i = 0; i < keys.size(); ++i) map[keys[i]] = i;
        return map;
    }

    // ---- the workloads ----------------------------------------------------

    // The dominant per-draw operation by a wide margin: a populated cache that is
    // read far more often than it is written.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void LookupHit(benchmark::State& state) {
        const auto& keys = CachedKeys<KeyVec, Make>(static_cast<SizeT>(state.range(0)));
        auto map = Populated(keys);
        for (auto _ : state) {
            for (const auto& k : keys) {
                auto it = map.find(k);
                benchmark::DoNotOptimize(it->second);
            }
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(keys.size()));
    }

    // "Is this resource cached yet?" answered NO - the probe length on a miss is a
    // different cost from a hit, and resource caches ask this constantly.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void LookupMiss(benchmark::State& state) {
        const SizeT n = static_cast<SizeT>(state.range(0));
        const auto& keys = CachedKeys<KeyVec, Make>(n);
        auto map = Populated(keys);
        const KeyVec absent = Make(n); // same shape, never inserted
        for (auto _ : state) {
            for (const auto& k : absent) {
                benchmark::DoNotOptimize(map.find(k) != map.end());
            }
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(absent.size()));
    }

    // Building a cache from empty, rehashes included.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void InsertGrow(benchmark::State& state) {
        const auto& keys = CachedKeys<KeyVec, Make>(static_cast<SizeT>(state.range(0)));
        for (auto _ : state) {
            UnorderedMap<typename KeyVec::value_type, Uint64> map;
            for (SizeT i = 0; i < keys.size(); ++i) map[keys[i]] = i;
            benchmark::DoNotOptimize(map.size());
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(keys.size()));
    }

    // Cache eviction and refill: erase half by key, put them back. This is the
    // aged-out-entry sweep the pipeline and vertex-input caches do.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void EraseChurn(benchmark::State& state) {
        const auto& keys = CachedKeys<KeyVec, Make>(static_cast<SizeT>(state.range(0)));
        for (auto _ : state) {
            state.PauseTiming();
            auto map = Populated(keys);
            state.ResumeTiming();
            for (SizeT i = 0; i < keys.size(); i += 2) benchmark::DoNotOptimize(map.erase(keys[i]));
            for (SizeT i = 0; i < keys.size(); i += 2) map[keys[i]] = i;
            benchmark::DoNotOptimize(map.size());
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(keys.size()));
    }

    // Mass eviction: erase-while-iterating across the whole table. This is the loop
    // shape that a container's erase()-return contract can get wrong, and the one
    // that fed garbage handles to vkDestroyPipeline when it was wrong before.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void EraseSweep(benchmark::State& state) {
        const auto& keys = CachedKeys<KeyVec, Make>(static_cast<SizeT>(state.range(0)));
        for (auto _ : state) {
            state.PauseTiming();
            auto map = Populated(keys);
            state.ResumeTiming();
            for (auto it = map.begin(); it != map.end();) it = map.erase(it);
            benchmark::DoNotOptimize(map.size());
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(keys.size()));
    }

    // Whole-table walks: the per-frame sweeps that age entries out, and the
    // teardown loops that destroy every Vulkan object a cache owns.
    template <typename KeyVec, KeyVec (*Make)(SizeT)>
    void Iterate(benchmark::State& state) {
        const auto& keys = CachedKeys<KeyVec, Make>(static_cast<SizeT>(state.range(0)));
        auto map = Populated(keys);
        for (auto _ : state) {
            Uint64 acc = 0;
            for (const auto& entry : map) acc += entry.second;
            benchmark::DoNotOptimize(acc);
        }
        state.SetItemsProcessed(state.iterations() * static_cast<Int64>(keys.size()));
    }

} // namespace

#define MGL_MAP_BENCH(WORKLOAD, SHAPE, VEC, MAKER)                                                 \
    BENCHMARK_TEMPLATE(WORKLOAD, VEC, MAKER)                                                       \
        ->Name(#WORKLOAD "/" #SHAPE)                                                               \
        ->RangeMultiplier(8)                                                                       \
        ->Range(kMinSize, kMaxSize)

MGL_MAP_BENCH(LookupHit, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(LookupHit, pointer, Vector<Uint64>, PointerKeys);
MGL_MAP_BENCH(LookupHit, digest, Vector<Uint64>, DigestKeys);
MGL_MAP_BENCH(LookupHit, name, Vector<String>, NameKeys);

MGL_MAP_BENCH(LookupMiss, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(LookupMiss, pointer, Vector<Uint64>, PointerKeys);
MGL_MAP_BENCH(LookupMiss, digest, Vector<Uint64>, DigestKeys);
MGL_MAP_BENCH(LookupMiss, name, Vector<String>, NameKeys);

MGL_MAP_BENCH(InsertGrow, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(InsertGrow, pointer, Vector<Uint64>, PointerKeys);
MGL_MAP_BENCH(InsertGrow, digest, Vector<Uint64>, DigestKeys);
MGL_MAP_BENCH(InsertGrow, name, Vector<String>, NameKeys);

MGL_MAP_BENCH(EraseChurn, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(EraseChurn, digest, Vector<Uint64>, DigestKeys);
MGL_MAP_BENCH(EraseChurn, name, Vector<String>, NameKeys);

MGL_MAP_BENCH(EraseSweep, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(EraseSweep, digest, Vector<Uint64>, DigestKeys);

MGL_MAP_BENCH(Iterate, sequential, Vector<Uint64>, SequentialKeys);
MGL_MAP_BENCH(Iterate, digest, Vector<Uint64>, DigestKeys);

BENCHMARK_MAIN();
