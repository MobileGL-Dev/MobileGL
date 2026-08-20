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
    // The three-level shader translation memo.
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
    // THE LEVELS FOLLOW THE GL ENTRY POINTS, not the arrows above, and that is the
    // key to reading this file:
    //   * L1c memoizes what one glCompileShader produces - the PARSE VERDICT;
    //   * L1  memoizes what one glLinkProgram produces - the whole front end from
    //     the link through SPIR-V';
    //   * L2  memoizes the segment from SPIR-V' to the emitted backend payload.
    //
    // L1 could never have covered the parse, however wide its payload got, because
    // the parse does not happen during glLinkProgram: it happens one entry point and
    // one job earlier, and by the time a link consults L1 it has already been paid
    // for. That is why the compile half is a separate level rather than a bigger
    // payload - see the L1c section below for the measurement that forced it.
    //
    // L1c and L1 are both backend-agnostic (the same modules feed DirectGLES and
    // DirectVulkan) and share one environment key, CompileEnv::frontendFingerprint.
    // L2 is kept apart on purpose: its key is made almost entirely of BACKEND
    // capability bits, and folding them in would make every DirectGLES capability a
    // reason to miss on the front-end half as well.
    //
    // WITH BOTH FRONT-END LEVELS HIT, NO GLSLANG OBJECT IS CONSTRUCTED AT ALL - no
    // TShader (L1c) and no TProgram (L1) - so the parse, the link and mapIO,
    // GlslangToSpv, spirv-opt, buildReflection and the global-UBO routing are all
    // skipped. On the L1 side that is possible because the payload is the whole
    // front-end OUTPUT (LinkArtifacts + SpirvArtifacts, both plain owned data)
    // rather than the SPIR-V alone, and because the GL query surface no longer reads
    // a live TProgram to answer anything - see ProgramObject::UniformReflection and
    // ProgramLinkTask::SnapshotGlslangReflection.
    //
    // NEITHER LEVEL EVER CACHES A LIVE GLSLANG OBJECT GRAPH, and both had the option:
    // TObjectReflection::type points into the TProgram's own pool allocator, so
    // sharing a TProgram between ProgramObjects is an aliasing hazard, and mapIO
    // mutates a TShader's aliased intermediate, so sharing a parse is a consume-once
    // hazard. L1 sidesteps the first by storing the reflection as owned data; L1c
    // sidesteps the second by storing only the VERDICT and letting the one link that
    // actually needs an AST parse it on demand.
    //
    // CORRECTNESS RULE, non-negotiable. A wrong hit is a silently miscompiled
    // shader - far worse than a slow one. So:
    //   * the key blob carries the FULL bytes of every input, never a digest, and
    //     every candidate hit is confirmed by comparing those bytes. The 64-bit
    //     hash is a bucket selector only; a collision degrades to a miss.
    //   * every input that can change the output is in the blob. Adding an input
    //     to a translation step MEANS adding it to that level's key builder.
    //   * MOBILEGL_SHADER_CACHE=0 turns ALL THREE levels off, so a field miscompile
    //     can be bisected against the cache in one run.
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

    // The process-wide master switch for ALL THREE levels, mirroring MOBILEGL_SHADER_CACHE.
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
        // ORDER-SENSITIVE, unlike NameMap: a transform-feedback capture list is a sequence,
        // and gl_NextBuffer / gl_SkipComponentsN make its order load-bearing.
        void TextList(const Vector<String>& values);

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
    // L1 - the LINK half of the front end: parsed GLSL program -> sanitized SPIR-V
    // modules, plus the whole GL query surface. (The PARSE half is L1c, below.)
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
    // L1 IS BACKEND-AGNOSTIC BY CONTRACT. Two contexts on different GPUs compiling
    // the same GLSL share one L1 entry: nothing that merely steers a BACKEND
    // transpile (backend identity, GLES/Vulkan capability bits, driver extension
    // strings, GPU vendor) is allowed in this key - all of that lives in L2's key,
    // where it belongs. What IS here is the subset of the environment that changes
    // what glslang itself produces; see CompileEnv::frontendFingerprint for the
    // field-by-field classification and the evidence behind each call.
    //
    // WHAT IS IN THE KEY (each one is an input that can change the modules):
    //   * CompileEnv::frontendFingerprint - the glslang resource limits
    //     BuildTBuiltInResource enforces at parse, plus the two inputs to the
    //     reflection vertex-attrib limit. NOT CompileEnv::fingerprint, which also
    //     covers backend identity and the advertised extension vector;
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
    struct SpirvTranslationKeyInputs {
        struct Stage {
            GLenum type = 0;
            StringView preprocessedSource;
        };

        // CompileEnv::frontendFingerprint, NEVER CompileEnv::fingerprint - see the
        // backend-agnosticism note above.
        Uint64 frontendFingerprint = 0;
        Vector<Stage> stages;
        const UnorderedMap<String, Uint>* explicitVertexInLocations = nullptr;
        const UnorderedMap<String, Uint>* explicitFragmentOutLocations = nullptr;
        const UnorderedMap<String, Uint>* explicitFragmentOutIndices = nullptr;
        const UnorderedMap<String, Uint>* explicitOpaqueUniformBindings = nullptr;
        Uint32 shaderCompileFlags = 0;
        Bool enableSpirvValidation = false;
        // ---- inputs that only matter because the PAYLOAD now carries the reflection ----
        // When the payload was SPIR-V alone these were provably irrelevant: transform
        // feedback is resolved by READING the linked intermediates and never writes an XFB
        // qualifier, and the fragment-output limit is a link-failure gate, so neither can
        // move a single word of the generated module. Both DO shape LinkArtifacts
        // (xfbVaryings / xfbStrides / xfbBufferMode / gsStripTriangles, and whether the link
        // is rejected at all), so widening the payload to the whole front end pulled them
        // into the key. Widening a payload means widening the key.
        const Vector<String>* requestedXfbVaryings = nullptr;
        Uint32 xfbBufferMode = 0;
        Int32 maxFragmentOutputColorNumber = 0;
    };

    TranslationCacheKey BuildSpirvTranslationKey(const SpirvTranslationKeyInputs& inputs);

    // The L1 PAYLOAD and its cache instance live in
    // MG_State/GLState/ProgramState/ProgramTranslationCache.h, not here: the payload is a
    // whole ProgramObject::LinkArtifacts + SpirvArtifacts, and MG_Util must not depend on
    // MG_State. Only the key - which is plain bytes - is built here, so both layers agree on
    // one definition of "the same front-end input".

    // =======================================================================
    // L1c - the COMPILE half of the front end: one glCompileShader's PARSE VERDICT.
    // =======================================================================
    //
    // WHY THIS EXISTS. L1 above memoizes one glLinkProgram. It skips the link, mapIO,
    // GlslangToSpv, spirv-opt, buildReflection and the routing pass - but NOT the glslang
    // parse, because the parse does not happen at glLinkProgram. It happens at
    // glCompileShader, one job earlier, and by the time the link hits L1 the parse has
    // already been paid for. Measured: the parse is ~322 us of a ~650 us CTS-shaped program
    // build, and on a Mali Immortalis-G925 an L1-only build of
    // KHR-GL33.texture_swizzle.smoke_access_idx_0_channel_idx_0 (2592 programs) ran 50.65 s
    // against 75.16/72.68 s with the cache off - 1.45-1.48x, which is what "everything but
    // the parse" buys. This level is the other half.
    //
    // WHAT IS MEMOIZED IS THE VERDICT, NOT THE PARSE. glCompileShader produces exactly three
    // parse-derived things: GL_COMPILE_STATUS, the info log, and a glslang::TShader. The
    // first two are a pure function of the key below. The third is CONSUME-ONCE - mapIO
    // mutates its aliased intermediate at link - so it can be neither cached nor shared, and
    // caching a live glslang object graph was rejected for L1 for exactly that reason.
    //
    // So a hit publishes the verdict and NO TShader at all, and the parse becomes LAZY:
    // ShaderCompileTask::ClaimParsedShader already re-parses on demand when the node carries
    // no stored parse, because stage 4 built that path for the CAS loser (one shader linked
    // into a second program). A link that HITS L1 never calls it, so the parse never happens.
    // A link that MISSES calls it and pays the parse there instead - the same single parse,
    // moved, not duplicated.
    //
    // WHAT THIS DELIBERATELY IS NOT: an extension of ShaderCompileAdoptionMap. That map
    // indexes LIVE compile nodes by WeakPtr, per context, so that a burst of shader objects
    // handed byte-identical source shares one job. It structurally cannot serve this case:
    // the CTS shape deletes its shader objects every iteration, so the node expires and the
    // entry with it, and even a hit would hand over a parse whose single use the first link
    // already consumed. Making it hold strong references would pin one glslang arena per
    // distinct source for the life of the context - megabytes per shaderpack, and precisely
    // the live-object-graph hazard this design avoids.
    //
    // BACKEND-AGNOSTIC, on the same contract as L1: the key carries
    // CompileEnv::frontendFingerprint and never CompileEnv::fingerprint.
    //
    // IF THE KEY IS EVER WRONG, the two directions fail very differently, and it is worth
    // knowing which one to fear:
    //   * a wrong `parsed = true` is CAUGHT. The stage holds no AST, so the first link that
    //     needs one re-parses - and that parse fails, ConsumeShaders reports "Internal error:
    //     re-parsing an attached <stage> for linking failed" and the link returns GL_FALSE.
    //     Wrong, loud, and named.
    //   * a wrong `parsed = false` is NOT caught. Nothing re-derives it, so a shader that
    //     would have compiled reports GL_COMPILE_STATUS false with a stale log.
    // Neither is a silent MISCOMPILE - no wrong SPIR-V can be produced through this level,
    // because it caches no translated output at all - but the second is the one that would
    // reach an application as an inexplicable failure. Both are why the key carries the full
    // source bytes and is compared in full.
    struct ShaderParseVerdict {
        // What ShaderCompileTask publishes as GL_COMPILE_STATUS.
        Bool parsed = false;
        // What it publishes as the info log. EMPTY whenever `parsed`, and that is a property
        // of the pipeline rather than of glslang: RunCompilePipeline clears the log on a
        // successful parse, so a successful compile's observable log is empty no matter what
        // glslang wrote into it. Stored rather than assumed so the two cannot drift.
        String infoLog;
    };
    using ShaderParseVerdictPtr = SharedPtr<const ShaderParseVerdict>;

    // WHAT IS IN THE KEY - the complete input set of ShaderCompiler::CompileShader, which is
    // the only thing between this cache and the verdict:
    //   * frontendFingerprint - BuildTBuiltInResource is the one thing ParseShaderSource
    //     reads from the environment, and glslang both ENFORCES those limits at parse and
    //     expands several of them into built-in constants;
    //   * shaderType - it selects the EShLanguage parsed against, and it is also printed
    //     verbatim into the failure log this cache reproduces;
    //   * the FULL preprocessed source, byte for byte. This is the text ParseShaderSource is
    //     handed, and it also covers CompileShader's legacy-#version retry, which is a pure
    //     function of that text (RetargetLegacyVersionDirectiveTo460);
    //   * the ShaderCompileBits - CompileForOpenGL selects a different setEnvClient /
    //     setEnvTarget triple and skips setEnvInputVulkanRulesRelaxed, which changes both
    //     what parses and what the parse produces. Always 0 on both production paths; in the
    //     key so a future non-zero value cannot alias a parse made without it.
    //
    // WHAT IS DELIBERATELY OUT:
    //   * everything else ParseShaderSource touches, because all of it is a COMPILE-TIME
    //     CONSTANT: the 460/ECoreProfile default version, EShMsgDefault, forwardCompatible,
    //     the "#undef VULKAN" preamble, setNanMinMaxClamp/setInvertY/setAutoMapLocations/
    //     setAutoMapBindings, and GLOBAL_UBO_NAME. A build that changes one of them is a
    //     different binary and cannot share an in-memory cache with the old one.
    //   * enableSpirvValidation. It is not an argument of CompileShader at all - the parse
    //     never reaches the SPIR-V validator. (L1 carries it because SanitizeAndOptimizeBinary
    //     does take it.)
    //   * backend identity and advertisedExtensions, on exactly L1's argument: the only
    //     front-end consumer of the extension list REWRITES THE SOURCE TEXT, and the
    //     preprocessed text is in this key verbatim - a strictly finer discriminator.
    //   * the ORIGINAL (pre-preprocess) source. The preprocessed text is what the parse
    //     consumes, so keying on the original would be both coarser in the wrong direction
    //     and redundant; ShaderPreprocessCache is the memo that keys on the original.
    struct ShaderParseVerdictKeyInputs {
        // CompileEnv::frontendFingerprint, NEVER CompileEnv::fingerprint.
        Uint64 frontendFingerprint = 0;
        GLenum shaderType = 0;
        StringView preprocessedSource;
        Uint32 shaderCompileFlags = 0;
    };

    TranslationCacheKey BuildShaderParseVerdictKey(const ShaderParseVerdictKeyInputs& inputs);
    SizeT ShaderParseVerdictBytes(const ShaderParseVerdict& verdict);

    BoundedTranslationCache<ShaderParseVerdict>& GetShaderParseVerdictCache();

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
    //   * the atomic-counter binding top, which SetAtomicCounterBlockBindings turns into the
    //     layout(binding=) qualifier every synthesized counter block is printed with;
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
        // Which GL atomic-counter binding points THIS stage's synthesized
        // gl_AtomicCounterBlock_<N> blocks named, as SetAtomicCounterBlockBindings reported
        // them. Same contract as the XFB names above and here for the same reason: the draw
        // path re-issues exactly these as storage-buffer bindings, so a payload that dropped
        // them would leave every counter buffer unbound on a hit - a program that renders but
        // never increments a counter, which is far harder to notice than a broken shader.
        Vector<Int> atomicCounterGlBindings;
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

        // The top of the reserved storage-block window atomic-counter blocks are moved into
        // (`top - N` for GL binding N). Derived from the driver's
        // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, so it differs per driver, and it is PRINTED
        // INTO the emitted ESSL as a layout(binding=) qualifier - which makes it key material,
        // not just a caller's bookkeeping.
        Int atomicCounterEsslBindingTop = -1;

        // --- SPIRV-Cross options ---
        Uint esslVersion = 300;

        Bool enableSpirvValidation = false;
    };

    TranslationCacheKey BuildEsslTranslationKey(const EsslTranslationKeyInputs& inputs);
    SizeT EsslTranslationResultBytes(const EsslTranslationResult& result);

    BoundedTranslationCache<EsslTranslationResult>& GetEsslTranslationCache();

    // Drops L1c and L2 (L1 lives in MG_State and has its own
    // ClearProgramTranslationCache). Called from the same teardown that resets the
    // glslang prewarm latch: nothing here holds a glslang object, so this is RSS
    // hygiene rather than a correctness requirement.
    void ClearShaderTranslationCaches();

    // One MGLOG_D line per level. Called at teardown and cheap enough to call from
    // a test.
    void LogShaderTranslationCacheStats();
} // namespace MobileGL::MG_Util::ShaderTranspiler
