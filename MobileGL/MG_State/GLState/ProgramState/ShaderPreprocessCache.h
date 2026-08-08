// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <list>
#include <MG_State/GLState/ProgramState/ShaderObject.h>

namespace MobileGL::MG_State::GLState {
    // Where the shared, source-only half of ShaderObject::Compile() stopped. The two
    // rejection verdicts are kept apart (rather than collapsed into "failed") so a hit
    // reproduces the original diagnosis, not just the original info log.
    enum class ShaderPreprocessOutcome : Uint8 {
        // The source-only half ran clean; preprocessedSource and both maps are valid.
        Preprocessed,
        // ValidateComputeLocalSizeLimits rejected it (compute only).
        ComputeLocalSizeRejected,
        // FindReservedIdentifierViolation rejected it.
        ReservedIdentifierRejected,
        // The source-only half was clean but glslang rejected the preprocessed source.
        // Memoizing this saves the parse itself on every later object with that source.
        ParseFailed,
    };

    // Everything ShaderObject::Compile() derives from the source text alone, i.e.
    // everything that is identical for two shader objects holding byte-identical source.
    struct ShaderPreprocessResult {
        ShaderPreprocessOutcome outcome = ShaderPreprocessOutcome::Preprocessed;
        // Valid unless the preprocessor itself never ran; kept even for the rejection
        // outcomes because that is the text the diagnostics refer to.
        String preprocessedSource;
        UnorderedMap<String, Int> explicitUniformLocations;
        UnorderedMap<String, Uint> explicitOpaqueBindings;
        // The compile info log to publish; empty when outcome == Preprocessed.
        String infoLog;

        Bool Preprocessed() const { return outcome == ShaderPreprocessOutcome::Preprocessed; }
    };

    // P0b layer 2: a per-context, bounded memo of the source-only half of shader
    // compilation, keyed by (stage, xxhash64(source), source length).
    //
    // Motivation: in the Iris shader-pack corpus ~21% of every glCompileShader in a trace
    // is a *different* shader object holding byte-identical source (packs glue the same
    // common/composite GLSL into many program stages), so the preprocess + reserved-
    // identifier scan + explicit-location/binding extraction runs over the same megabytes
    // again and again. Layer 1 (in ShaderObject) covers the same object recompiled with
    // unchanged source; this covers the cross-object case.
    //
    // What is NOT cached: the glslang parse. glslang's TShader is consume-once (mapIO
    // mutates the aliased intermediate at link), so every shader object still needs its
    // own parse; only the text-processing half is shared.
    //
    // Correctness: the 64-bit hash is a lookup accelerator only. Every hit re-compares the
    // full stored original source with memcmp before it is honored, so a hash collision
    // degrades to a miss, never to a wrong answer. That is why the full original text is
    // stored rather than a prefix/suffix digest - the cache is bounded, so the cost is.
    //
    // Eviction: FIFO (insertion order), bounded by BOTH an entry count and a stored-source
    // byte budget, whichever binds first. FIFO rather than LRU because shader-pack loading
    // is a burst of mostly-distinct sources whose reuse clusters around insertion time;
    // LRU's extra list splice on every hit buys nothing measurable here, and FIFO keeps
    // Find() a genuinely const, read-only operation.
    class ShaderPreprocessCache {
    public:
        static constexpr SizeT kMaxEntries = 128;
        static constexpr SizeT kMaxStoredSourceBytes = 8u * 1024u * 1024u;

        // Returns the memoized result for this exact source, or null on a miss. The
        // returned pointer stays valid until the next Insert()/Clear() on this cache.
        const ShaderPreprocessResult* Find(ShaderStage stage, Uint64 sourceHash, const String& source) const;

        // Memoizes `result` for this source. A source whose own storage cost already
        // exceeds the byte budget is simply not cached (caching it would evict everything
        // else and then itself).
        void Insert(ShaderStage stage, Uint64 sourceHash, const String& source, ShaderPreprocessResult result);

        void Clear();

        static Uint64 HashSource(const String& source) {
            return static_cast<Uint64>(XXH64(source.data(), source.length(), 0));
        }

        SizeT GetEntryCount() const { return m_entries.size(); }
        SizeT GetStoredSourceBytes() const { return m_storedSourceBytes; }

    private:
        struct Key {
            ShaderStage stage = ShaderStage::Unknown;
            Uint64 sourceHash = 0;
            SizeT sourceLength = 0;

            Bool operator==(const Key& other) const {
                return stage == other.stage && sourceHash == other.sourceHash && sourceLength == other.sourceLength;
            }
        };

        struct KeyHasher {
            SizeT operator()(const Key& key) const {
                // The source hash already spreads well; fold the two discriminators in so
                // that same-hash-different-stage/length keys land in different buckets.
                Uint64 mixed = key.sourceHash;
                mixed ^= static_cast<Uint64>(key.sourceLength) + 0x9e3779b97f4a7c15ull + (mixed << 6) + (mixed >> 2);
                mixed ^= static_cast<Uint64>(static_cast<Int>(key.stage)) * 0xff51afd7ed558ccdull;
                return static_cast<SizeT>(mixed);
            }
        };

        struct Entry {
            Key key;
            // The full original (pre-preprocess) source, kept so a hit can be confirmed by
            // comparison instead of trusting the hash.
            String originalSource;
            ShaderPreprocessResult result;
        };

        using EntryList = std::list<Entry>;

        static SizeT EntryBytes(const String& source, const ShaderPreprocessResult& result) {
            return source.length() + result.preprocessedSource.length();
        }

        void EraseEntry(EntryList::iterator it);
        void EvictUntilWithinBudget();

        // P1: needs a mutex when compiles go async. Everything here is reached from
        // glCompileShader on the single GL thread that owns the context, so today the
        // cache is deliberately lock-free; the moment shader compilation moves onto a
        // worker pool, Find/Insert/Clear all become critical sections (and Find's returned
        // pointer stops being safe to hold across an Insert).
        EntryList m_entries;                                 // front = oldest (FIFO victim)
        UnorderedMap<Key, EntryList::iterator, KeyHasher> m_index;
        SizeT m_storedSourceBytes = 0;
    };
} // namespace MobileGL::MG_State::GLState
