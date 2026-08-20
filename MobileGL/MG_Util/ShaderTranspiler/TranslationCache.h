// MobileGL - MobileGL/MG_Util/ShaderTranspiler/TranslationCache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <list>
#include <mutex>
#include <set>

namespace MobileGL::MG_Util::ShaderTranspiler {
    // ===========================================================================
    // The two-level shader translation memo.
    //
    // MOTIVATION (measured). KHR-GL33.texture_swizzle.smoke_* builds 2592 programs
    // per case out of a handful of DISTINCT sources - the CTS template substitutes
    // BASIC_TYPE and little else within one case - and the process is CPU-bound at
    // 93% cpu/wall with the device driver's own compiler at 0.15%. Every one of
    // those 2592 programs walks the whole translation chain again:
    //
    //   GLSL --[glslang parse]--> AST --[link + mapIO]--> TProgram
    //        --[GlslangToSpv]--> SPIR-V --[SanitizeAndOptimizeBinary]--> SPIR-V'
    //        --[backend SPIR-V pass chain]--> SPIR-V'' --[SPIRV-Cross]--> ESSL
    //
    // L1 memoizes the segment from the parsed program to SPIR-V'; L2 memoizes the
    // segment from SPIR-V' to the emitted backend payload. The two are kept apart
    // on purpose: L1 is backend-agnostic (the same module feeds DirectGLES and
    // DirectVulkan), while L2's key is made almost entirely of BACKEND capability
    // bits, and folding them into one key would make every DirectGLES capability a
    // reason to miss on the frontend half as well.
    //
    // WHAT IS DELIBERATELY NOT MEMOIZED: the glslang parse and the glslang link.
    // Both produce a TShader/TProgram, and the frontend's whole GL query surface
    // (ProgramObject::LinkArtifacts, BuildGlobalUboRouting) is built by asking that
    // TProgram questions - so skipping them means caching a live glslang object
    // graph and sharing it between ProgramObjects, which is a different change with
    // its own aliasing and consume-once hazards. See the report in the branch
    // history; the parse is ~50% of the per-stage cost and is the next campaign.
    //
    // CORRECTNESS RULE, non-negotiable. A wrong hit is a silently miscompiled
    // shader - far worse than a slow one. So:
    //   * the key blob carries the FULL bytes of every input, never a digest, and
    //     every candidate hit is confirmed by comparing those bytes. The 64-bit
    //     hash is a bucket selector only; a collision degrades to a miss.
    //   * every input that can change the output is in the blob. Adding an input
    //     to a translation step MEANS adding it to that level's key builder.
    //   * MOBILEGL_SHADER_CACHE=0 turns both levels off, so a field miscompile can
    //     be bisected against the cache in one run.
    //
    // NO DISK TIER IN THIS CHANGE. Persistence needs its own invalidation story
    // (driver/vendor string, MobileGL build id, glslang and SPIRV-Cross revisions)
    // and its own answer to "what if the file is hostile", and neither belongs in
    // a performance change. Where it WOULD attach: BoundedTranslationCache::Find,
    // on the miss path, would consult a disk tier keyed by the same blob before
    // returning null, and Insert would write through to it. Nothing in the design
    // below forecloses that - the key is already a self-contained byte string and
    // the payloads are already plain data.
    // ===========================================================================

    // The process-wide master switch, mirroring MOBILEGL_SHADER_CACHE.
    // QuirkOverride semantics: unset (Auto) is ON, an explicitly falsy value is
    // OFF. Read once from MG_Config::Features, so a worker never touches the
    // environment.
    Bool ShaderTranslationCacheEnabled();

    // Serializes the exact bytes of a cache key. Every appender is
    // length-prefixed or fixed-width, so no two different input tuples can
    // serialize to the same byte string by running into each other.
    class TranslationKeyBuilder {
    public:
        void Bytes(const void* data, SizeT length);

        template <typename T>
        void Value(const T& value) {
            static_assert(std::is_trivially_copyable_v<T>,
                          "TranslationKeyBuilder::Value hashes the object representation");
            Bytes(&value, sizeof(T));
        }

        // Length-prefixed, so "ab"+"c" and "a"+"bc" cannot collide.
        void Text(StringView text);
        void Words(const Vector<Uint32>& words);

        // Hash maps and sets are serialized in SORTED order, never in iteration
        // order: ska::flat_hash_map's iteration order depends on insertion history
        // and capacity, so two logically identical maps could otherwise serialize
        // differently and cause spurious misses. Sorting makes the blob canonical.
        // These maps are all tiny (explicit locations, image formats, storage-block
        // rebindings), so the sort is free.
        template <typename ValueT>
        void NameMap(const UnorderedMap<String, ValueT>& map) {
            static_assert(std::is_trivially_copyable_v<ValueT>);
            Vector<Pair<StringView, ValueT>> sorted;
            sorted.reserve(map.size());
            for (const auto& [name, value] : map) sorted.emplace_back(StringView(name), value);
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            Value(static_cast<Uint64>(sorted.size()));
            for (const auto& [name, value] : sorted) {
                Text(name);
                Value(value);
            }
        }

        // std::set is already ordered, but it gets the same length prefix.
        void NameSet(const std::set<String>& names);

        const String& Blob() const { return m_blob; }
        String Take() { return Move(m_blob); }

    private:
        String m_blob;
    };

    // A cache key: the full bytes, plus the hash that selects a bucket for them.
    // The blob is shared rather than copied so that indexing an entry by its key
    // does not double the memory a 100 KB shaderpack stage costs.
    struct TranslationCacheKey {
        Uint64 hash = 0;
        SharedPtr<const String> blob;

        Bool Valid() const { return blob != nullptr; }
        SizeT Bytes() const { return blob ? blob->size() : 0u; }

        // FULL comparison, always. This is what makes a hash collision a miss
        // rather than a miscompiled shader.
        Bool operator==(const TranslationCacheKey& other) const {
            if (hash != other.hash) return false;
            if (blob == other.blob) return true; // the same buffer
            if (!blob || !other.blob) return false;
            return *blob == *other.blob;
        }
    };

    struct TranslationCacheKeyHasher {
        SizeT operator()(const TranslationCacheKey& key) const { return static_cast<SizeT>(key.hash); }
    };

    // Seals a builder's bytes into a key.
    TranslationCacheKey MakeTranslationCacheKey(String blob);
    inline TranslationCacheKey MakeTranslationCacheKey(TranslationKeyBuilder& builder) {
        return MakeTranslationCacheKey(builder.Take());
    }

    struct TranslationCacheStats {
        Uint64 hits = 0;
        Uint64 misses = 0;
        Uint64 inserts = 0;
        Uint64 evictions = 0;
        // Entries whose own key+payload already exceed the whole byte budget.
        // Caching one would evict everything else and then itself.
        Uint64 rejectedOversize = 0;
        // Two workers missed on the same key and both computed it. Harmless (the
        // key covers every input, so both results are equal), but worth counting:
        // a large number would mean the redundancy is no longer a startup artifact.
        Uint64 duplicateInserts = 0;
    };

    // A bounded, thread-safe, process-lifetime memo.
    //
    // EVICTION is FIFO, bounded by BOTH an entry count and a stored-byte budget,
    // whichever binds first - the same policy (and the same reasoning) as
    // ShaderPreprocessCache. Translation workloads are bursts of mostly-distinct
    // inputs whose reuse clusters around insertion time, and FIFO keeps Find() a
    // read-only operation: with N pool workers hammering the same cache, an LRU
    // splice on every hit would turn the shared hit path into a writer.
    //
    // THREAD SAFETY. The mutex guards the containers only; the expensive
    // translation always runs OUTSIDE it, between the Find and the Insert. Two
    // workers that miss on the same key therefore both compute it, and the second
    // Insert is dropped. That is deliberate: the alternative - one worker waits
    // for the other's result - would block a pool worker inside a job body, which
    // is precisely the invariant (JobNode I4) that keeps ShaderCompilePool from
    // deadlocking when the waiting job holds the only worker the awaited job needs.
    // The waste is bounded by the worker count and only happens on the first burst.
    //
    // LIFETIME. Hits hand out shared ownership of the payload, never a pointer into
    // the entry list, so a reader keeps its payload alive across any concurrent
    // eviction - and across Clear() and the cache's own destruction.
    template <typename Payload>
    class BoundedTranslationCache {
    public:
        using PayloadPtr = SharedPtr<const Payload>;

        BoundedTranslationCache(const char* name, SizeT maxEntries, SizeT maxBytes)
            : m_name(name), m_maxEntries(maxEntries), m_maxBytes(maxBytes) {}

        PayloadPtr Find(const TranslationCacheKey& key) const {
            if (!key.Valid()) return nullptr;
            const std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_index.find(key);
            if (it == m_index.end()) {
                ++m_stats.misses;
                return nullptr;
            }
            ++m_stats.hits;
            return it->second->payload;
        }

        void Insert(TranslationCacheKey key, PayloadPtr payload, SizeT payloadBytes) {
            if (!key.Valid() || !payload) return;
            const SizeT entryBytes = key.Bytes() + payloadBytes;
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (entryBytes > m_maxBytes) {
                ++m_stats.rejectedOversize;
                return;
            }
            if (m_index.find(key) != m_index.end()) {
                // A concurrent miss on the same key computed it too. The incumbent
                // is kept: the key covers every input, so the two payloads are
                // equal, and replacing would only move a demonstrably-wanted entry
                // to the back of the FIFO.
                ++m_stats.duplicateInserts;
                return;
            }
            m_entries.push_back(Entry{key, Move(payload), entryBytes});
            m_index.emplace(Move(key), std::prev(m_entries.end()));
            m_storedBytes += entryBytes;
            ++m_stats.inserts;
            EvictUntilWithinBudgetLocked();
        }

        void Clear() {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_index.clear();
            m_entries.clear();
            m_storedBytes = 0;
        }

        TranslationCacheStats Stats() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_stats;
        }

        SizeT EntryCount() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }

        SizeT StoredBytes() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_storedBytes;
        }

        // MGLOG_D, so an INFO build compiles this out entirely.
        void LogStats() const {
            const TranslationCacheStats stats = Stats();
            const Uint64 lookups = stats.hits + stats.misses;
            MGLOG_D("%s: %llu/%llu hits (%.1f%%), %llu inserts, %llu evictions, %llu oversize, "
                    "%llu duplicate, %zu entries / %zu KiB",
                    m_name, static_cast<unsigned long long>(stats.hits),
                    static_cast<unsigned long long>(lookups),
                    lookups ? 100.0 * static_cast<double>(stats.hits) / static_cast<double>(lookups) : 0.0,
                    static_cast<unsigned long long>(stats.inserts),
                    static_cast<unsigned long long>(stats.evictions),
                    static_cast<unsigned long long>(stats.rejectedOversize),
                    static_cast<unsigned long long>(stats.duplicateInserts), EntryCount(),
                    StoredBytes() / 1024u);
        }

        // Tests only: makes the caps small enough to exercise eviction without
        // building megabytes of shaders. Clears the cache, because shrinking the
        // caps under live entries would otherwise leave it over budget.
        void SetCapsForTesting(SizeT maxEntries, SizeT maxBytes) {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_maxEntries = maxEntries;
            m_maxBytes = maxBytes;
            m_index.clear();
            m_entries.clear();
            m_storedBytes = 0;
            m_stats = {};
        }

    private:
        struct Entry {
            TranslationCacheKey key;
            PayloadPtr payload;
            SizeT bytes = 0;
        };
        using EntryList = std::list<Entry>;

        void EvictUntilWithinBudgetLocked() {
            while (!m_entries.empty() &&
                   (m_entries.size() > m_maxEntries || m_storedBytes > m_maxBytes)) {
                const auto victim = m_entries.begin();
                m_storedBytes -= victim->bytes;
                m_index.erase(victim->key);
                m_entries.erase(victim);
                ++m_stats.evictions;
            }
        }

        const char* m_name = "";
        SizeT m_maxEntries = 0;
        SizeT m_maxBytes = 0;

        mutable std::mutex m_mutex;
        mutable TranslationCacheStats m_stats;
        EntryList m_entries; // front = oldest = FIFO victim
        UnorderedMap<TranslationCacheKey, typename EntryList::iterator, TranslationCacheKeyHasher> m_index;
        SizeT m_storedBytes = 0;
    };

    // =======================================================================
    // L1 - the FRONT END: parsed GLSL program -> sanitized SPIR-V modules.
    // =======================================================================
    //
    // The cached artifact is the module AFTER SanitizeAndOptimizeBinary, not the
    // raw GlslangToSpv output. That is a deliberate choice and it is safe:
    // SanitizeAndOptimizeBinary is a fixed 11-pass spirv-opt chain with no
    // arguments but the module, and its two remaining parameters (`validateOutput`,
    // `enableSpirvValidation`) only decide whether the OUTPUT is handed to the
    // validator and logged - RunOptimizerChecked runs the optimizer first and
    // identically either way. Nothing between GlslangToSpv and Sanitize reads
    // backend state. So caching after Sanitize saves the 96 us/stage the chain
    // costs on top of the 40 us GlslangToSpv, and gives the backends exactly the
    // bytes they would have got.
    //
    // WHAT IS IN THE KEY (each one is an input that can change the modules):
    //   * the CompileEnv fingerprint - covers the glslang resource limits
    //     (BuildTBuiltInResource reads env->params), the backend identity, the
    //     advertised extension set and the compute limits;
    //   * per stage, in link order: the GL stage enum and the FULL preprocessed
    //     source, which is literally the text ParseShaderSource was given;
    //   * the four link-time request maps mapIO resolves against
    //     (glBindAttribLocation / glBindFragDataLocation /
    //     glBindFragDataLocationIndexed, and the merged layout(binding=) opaque
    //     units) - these steer TMglGlslIoResolver and therefore the Locations and
    //     Bindings baked into every module;
    //   * the ShaderCompileBits the parse ran under (always 0 in production; in
    //     the key so a future non-zero value cannot alias);
    //   * the SPIR-V validation switch (byte-identical output either way, but it
    //     costs one byte to be sure).
    //
    // The key is a PROGRAM-level key, not a per-stage one, and that is forced:
    // glslang's mapIO resolves a fragment stage's input Locations against the
    // vertex stage's outputs, so a stage's SPIR-V is NOT a function of that
    // stage's source alone. A per-stage key here would be exactly the silent
    // miscompile this cache must never produce.
    struct SpirvTranslationResult {
        // One module per stage, in the same order as ProgramLinkTask's
        // spirvHandoff.shaderTypes.
        Vector<Vector<Uint32>> modules;
    };
    using SpirvTranslationResultPtr = SharedPtr<const SpirvTranslationResult>;

    struct SpirvTranslationKeyInputs {
        struct Stage {
            GLenum type = 0;
            StringView preprocessedSource;
        };

        Uint64 envFingerprint = 0;
        Vector<Stage> stages;
        const UnorderedMap<String, Uint>* explicitVertexInLocations = nullptr;
        const UnorderedMap<String, Uint>* explicitFragmentOutLocations = nullptr;
        const UnorderedMap<String, Uint>* explicitFragmentOutIndices = nullptr;
        const UnorderedMap<String, Uint>* explicitOpaqueUniformBindings = nullptr;
        Uint32 shaderCompileFlags = 0;
        Bool enableSpirvValidation = false;
    };

    TranslationCacheKey BuildSpirvTranslationKey(const SpirvTranslationKeyInputs& inputs);
    SizeT SpirvTranslationResultBytes(const SpirvTranslationResult& result);

    // Process-global, and safe to be: the CompileEnv fingerprint is in the key, so
    // a module computed under one context's limits can never be handed to another
    // context with different ones. Global rather than per-context because the
    // producer (ProgramSpirvTask) runs on a pool worker and must not reach
    // MG_State::pGLContext.
    BoundedTranslationCache<SpirvTranslationResult>& GetSpirvTranslationCache();

    // =======================================================================
    // L2 - the BACK END: sanitized SPIR-V -> DirectGLES ESSL payload.
    // =======================================================================
    //
    // DIRECTGLES ONLY. DirectVulkan runs a different pass chain, steered by Vulkan
    // device features, and gets no L2 in this change; giving it one means giving it
    // its OWN instance with its OWN key, never this one.
    //
    // The memoized segment is BackendProgramObjectImpl::SyncToBackend's per-stage
    // block from the draw-parameter lowering down to (and including) the
    // SPIRV-Cross Compile() that produces the ESSL text. The text-level passes that
    // follow it are deliberately outside: they are cheap string work, and they read
    // a long tail of live per-program state (RebindImageUniformsToFrontendUnits
    // walks the ProgramObject's uniform reflection, the norm-clamp masks and the
    // fragColor broadcast count are live globals) whose inclusion would make the
    // key both huge and fragile for no measurable saving.
    //
    // WHAT IS IN THE KEY:
    //   * the FULL SPIR-V module (the input);
    //   * the GL stage enum - three passes are stage-gated (draw parameters and
    //     array vertex inputs on vertex, fragment-output index legalization on
    //     fragment);
    //   * SupportsViewportArray - arms LowerViewportIndexForEssl;
    //   * the four sample ceilings (color / integer / depth / advertised) - both
    //     ARM ClampMultisampleFetchesForEssl and PARAMETERIZE it;
    //   * SupportsNoperspectiveInterpolation - arms EmulateNoPerspectiveForEssl;
    //   * the transform-feedback capture block names - the argument to
    //     FlattenXfbInterfaceBlocksForEssl, and the reason the payload has to carry
    //     the names it actually flattened;
    //   * the image-format bake map (uniform name -> GL internal format), which is
    //     derived from LIVE glBindImageTexture state and is the one genuinely
    //     per-draw-state input in here;
    //   * the storage-block binding overrides handed to SPIRV-Cross;
    //   * the ESSL version SPIRV-Cross targets (ResolveBackendEsslVersion, i.e. the
    //     driver's GLES version) - the remaining two SPIRV-Cross options are
    //     compile-time constants (GLSL_ES true, VULKAN_SEMANTICS false);
    //   * the SPIR-V validation switch, as in L1.
    //
    // Unconditional passes (StripUboMemberRelaxedPrecision, LowerRectImages,
    // Lower1DArrayImages) take no input but the module and so need no key material.
    struct EsslTranslationResult {
        String essl;
        // Which interface blocks FlattenXfbInterfaceBlocksForEssl actually rewrote
        // in THIS stage. The caller unions these across stages and the transform-
        // feedback capture list follows them, so a payload that dropped them would
        // silently un-rename every capture on a cache hit.
        std::set<String> flattenedXfbBlockNames;
    };
    using EsslTranslationResultPtr = SharedPtr<const EsslTranslationResult>;

    struct EsslTranslationKeyInputs {
        const Vector<Uint32>* spirv = nullptr;
        GLenum shaderType = 0;

        // --- driver capability bits that arm or steer a pass ---
        Bool supportsViewportArray = false;
        Bool supportsNoperspectiveInterpolation = false;
        Int32 maxColorTextureSamples = 0;
        Int32 maxIntegerSamples = 0;
        Int32 maxDepthTextureSamples = 0;
        Int32 advertisedMaxSamples = 0;

        // --- per-program / per-context inputs ---
        const std::set<String>* xfbCaptureBlockNames = nullptr;
        const UnorderedMap<String, Uint>* glFormatByUniformName = nullptr;
        const UnorderedMap<String, Int>* storageBlockBindingOverrides = nullptr;

        // --- SPIRV-Cross options ---
        Uint esslVersion = 300;

        Bool enableSpirvValidation = false;
    };

    TranslationCacheKey BuildEsslTranslationKey(const EsslTranslationKeyInputs& inputs);
    SizeT EsslTranslationResultBytes(const EsslTranslationResult& result);

    BoundedTranslationCache<EsslTranslationResult>& GetEsslTranslationCache();

    // Drops both levels. Called from the same teardown that resets the glslang
    // prewarm latch: nothing here holds a glslang object, so this is RSS hygiene
    // rather than a correctness requirement.
    void ClearShaderTranslationCaches();

    // One MGLOG_D line per level. Called at teardown and cheap enough to call from
    // a test.
    void LogShaderTranslationCacheStats();
} // namespace MobileGL::MG_Util::ShaderTranspiler
